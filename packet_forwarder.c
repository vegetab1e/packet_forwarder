#include "packet_forwarder.h"
#include "dpdk_utils.h"
#include "dpdk_port.h"
#include "utils.h"
#include "types.h"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include <time.h>
#include <assert.h>

#include <rte_log.h>
#include <rte_debug.h>

#include <rte_pause.h>

#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250

#define RX_QUEUE_SIZE 256
#define TX_QUEUE_SIZE 256

#define RX_QUEUE_COUNT 3
#define MAX_RX_QUEUE_PER_PORT 16

#define PACKET_BURST_SIZE 32
#define PACKET_PREFETCH_OFFSET 3

#ifdef SLOW_MOTION
#define TX_DELAY_MS 100
#define RX_DELAY_SEC 2
#define POLL_DELAY_SEC 3
#else
#define RX_DELAY_SEC 1
#define POLL_DELAY_SEC 2
#endif

#define MAX_SEND_RETRIES 10

volatile bool is_running;

static PortConfigs port_configs;
static LCoreConfigs lcore_configs;
static TxPacketBuffers tx_packet_buffers;

/**
 * @brief Получить заголовок Ethernet
 * Возвращает указатель на заголовок Ethernet в переданном пакете, а также
 * тип Ethernet кадра и смещение на размер заголовков VLAN при их наличии,
 * которое нужно учитывать при работе с данными пакета
 * @param[in] mbuf Пакет
 * @param[out] ether_type Тип кадра Ethernet
 * @param[out] vlan_offset Суммарный размер заголовков VLAN
 * @return Указатель на заголовок Ethernet
 */
static inline
struct rte_ether_hdr*
getEthernetHeader(struct rte_mbuf* mbuf, uint16_t* ether_type, uint16_t* vlan_offset)
{
    struct rte_ether_hdr* ether_header = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);

    *ether_type = ether_header->ether_type;
    *vlan_offset = 0;

    if (rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN) == *ether_type)
    {
        const struct rte_vlan_hdr* vlan_header = (const struct rte_vlan_hdr*)(ether_header + 1);

        *ether_type = vlan_header->eth_proto;
        *vlan_offset = sizeof(struct rte_vlan_hdr);

        if (rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN) == *ether_type)
        {
            *ether_type = (++vlan_header)->eth_proto;
            *vlan_offset += sizeof(struct rte_vlan_hdr);
        }

        RTE_LOG(DEBUG, USER1, "VLAN tagged frame, offset: %u\n", *vlan_offset);
    }

    return ether_header;
}

/**
 * @brief Заполнить заголовок Ethernet
 * В качестве MAC-адреса получателя используется сгенерированный MAC-адрес
 * 5E:BA:0F:CE:0A:XX, где XX - случайное число от 0 до 255, или
 * случайный (не мультикаст), если первый был некорректен.
 * В качестве MAC-адреса отправителя используется реальный порта отправки
 * или случайный (тоже не мультикаст), если первый не удалось получить
 */
static inline
void fillEthernetHeader(struct rte_ether_hdr* ether_header,
                        uint16_t ether_type,
                        uint16_t tx_port_id)
{
    srand(time(NULL));
    const uint64_t random_number = (uint64_t)(rand() % 256) << 40;

    uint8_t* target_mac_addr = (uint8_t*)&ether_header->dst_addr.addr_bytes[0];
    *((uint64_t*)target_mac_addr) = 0xACE0FBA5E + random_number;
    if (!rte_is_valid_assigned_ether_addr(&ether_header->dst_addr))
        rte_eth_random_addr(target_mac_addr);

    struct rte_ether_addr source_mac_addr;
    if (!!rte_eth_macaddr_get(tx_port_id, &source_mac_addr))
        rte_eth_random_addr(&source_mac_addr.addr_bytes[0]);
    rte_ether_addr_copy(&source_mac_addr, &ether_header->src_addr);

    ether_header->ether_type = ether_type;
}

/**
 * @brief Очистить тег VLAN TCI (внешняя сеть) и связанные флаги
 * Обнуляется поле mbuf->vlan_tci_outer и снимаются соответствующие биты флага
 * mbuf->ol_flags. Заголовоки VLAN (внешней и внутренней сети) из самого кадра
 * Ethernet, хранящегося в данных пакета, удаляются раньше (оборудованием,
 * средствами DPDK) или позже в функции forwardPacket() после их обрабокти
 * @param[in] mbuf Пакет
 */
static inline
void cleanupVlanTciOuter(struct rte_mbuf* mbuf)
{
    assert(!!mbuf);

    if (!(mbuf->ol_flags & RTE_MBUF_F_RX_QINQ))
        return;

    if (mbuf->ol_flags & RTE_MBUF_F_RX_QINQ_STRIPPED)
    {
        RTE_LOG(WARNING, USER1, "VLAN stripping must be disabled\n");
        mbuf->ol_flags &= ~RTE_MBUF_F_RX_QINQ_STRIPPED;
    }

    mbuf->vlan_tci_outer = 0;
    mbuf->ol_flags &= ~RTE_MBUF_F_RX_QINQ;
}

/**
 * @brief Очистить тег VLAN TCI (внутренняя сеть) и связанные флаги
 * Обнуляется поле mbuf->vlan_tci и снимаются соответствующие биты флага
 * mbuf->ol_flags. Заголовоки VLAN (внешней и внутренней сети) из самого кадра
 * Ethernet, хранящегося в данных пакета, удаляются раньше (оборудованием,
 * средствами DPDK) или позже в функции forwardPacket() после их обрабокти
 * @param[in] mbuf Пакет
 */
static inline
void cleanupVlanTciInner(struct rte_mbuf* mbuf)
{
    assert(!!mbuf);

    if (!(mbuf->ol_flags & RTE_MBUF_F_RX_VLAN))
        return;

    if (mbuf->ol_flags & RTE_MBUF_F_RX_VLAN_STRIPPED)
    {
        RTE_LOG(WARNING, USER1, "VLAN stripping must be disabled\n");
        mbuf->ol_flags &= ~RTE_MBUF_F_RX_VLAN_STRIPPED;
    }

    mbuf->vlan_tci = 0;
    mbuf->ol_flags &= ~RTE_MBUF_F_RX_VLAN;
}

/**
 * @brief Отправить пакеты
 * Один или несколько, в цикле с задержкой, если с первого раза
 * отправить все пакеты не удалось. Количество попыток и задержка
 * между ними задаются соответствующими макросами
 * @param[in] tx_port_id Номер сетевого порта для отправки пакетов
 * @param[in] queue_id Номер очереди исходящих пакетов
 * @param[in] packets Массив отправляемых пакетов
 * @param[in] packet_count Количество отправляемых пакетов
 * @return Количество отправленных пакетов
 */
static inline
uint16_t sendPackets(uint16_t port_id, uint16_t queue_id,
                     struct rte_mbuf** packets, uint16_t packet_count)
{
    uint8_t retry_count = 0;
    uint16_t sent_packet_count, packet_number = 0;
    do {
#ifdef SLOW_MOTION
        if (retry_count) rte_delay_ms(TX_DELAY_MS);
#else
        if (retry_count) rte_pause();
#endif
        sent_packet_count = rte_eth_tx_burst(port_id,
                                             queue_id,
                                             &packets[packet_number],
                                             packet_count);

        packet_count -= sent_packet_count;
        packet_number += sent_packet_count;

        ++retry_count;
    } while(packet_count && (retry_count < MAX_SEND_RETRIES));

    return packet_number;
}

/**
 * @brief Повторить отправку пакетов
 * Функция обратного вызова - обработчик ошибок отправки пакетов.
 * Проверяет пакеты и отправляет их повторно, делая дамп некорректных
 * (точно некорректный только один, первый найденный, остальные не проверяются)
 * и неотправленных (повторно) пакетов.
 * @param[in] unsent_packets Массив неотправленных пакетов
 * @param[in] unsent_packet_count Количество неотправленных пакетов
 * @param[in] userdata Не используется
 */
static
void resendPackets(struct rte_mbuf** unsent_packets,
                   uint16_t unsent_packet_count,
                   __attribute__((__unused__)) void* user_data)
{
    if (!unsent_packets)
    {
        RTE_LOG(ERR, USER1, "Internal error: no packets\n");
        return;
    }

    const LCoreConfigPtr lcore_config = &lcore_configs[rte_lcore_id()];
    PacketStatsPtr packet_stats = &lcore_config->packet_stats;

#ifndef NDEBUG
    if (!!packet_stats)
        __atomic_fetch_add(&packet_stats->retx_ops, 1, __ATOMIC_SEQ_CST);
#endif

    const uint16_t prepared_packet_count = rte_eth_tx_prepare(lcore_config->tx_port_id,
                                                              lcore_config->queue_id,
                                                              unsent_packets,
                                                              unsent_packet_count);
    if (prepared_packet_count < unsent_packet_count)
    {
        RTE_LOG(ERR, USER1,
                "Failed to prepare %hu packets: %s\n",
                unsent_packet_count - prepared_packet_count, rte_strerror(rte_errno));

        if (!!packet_stats)
            __atomic_fetch_add(&packet_stats->proc_error_count, unsent_packet_count - prepared_packet_count, __ATOMIC_SEQ_CST);

        dumpAndFreePackets(&unsent_packets[prepared_packet_count],
                           unsent_packet_count - prepared_packet_count);
    }

    if (!prepared_packet_count)
        return;

    const uint16_t sent_packet_count = sendPackets(lcore_config->tx_port_id,
                                                   lcore_config->queue_id,
                                                   unsent_packets,
                                                   prepared_packet_count);
    if (sent_packet_count < prepared_packet_count)
    {
        RTE_LOG(ERR, USER1, "Failed to send %hu packets\n", prepared_packet_count - sent_packet_count);

        if (!!packet_stats)
            __atomic_fetch_add(&packet_stats->proc_error_count, prepared_packet_count - sent_packet_count, __ATOMIC_SEQ_CST);

        dumpAndFreePackets(&unsent_packets[sent_packet_count],
                           prepared_packet_count - sent_packet_count);
    }

    if (!sent_packet_count)
        return;

    if (!!packet_stats)
        __atomic_fetch_add(&packet_stats->tx_packet_count, sent_packet_count, __ATOMIC_SEQ_CST);
}

/**
 * @brief Переслать пакет
 * Из пакета удаляются заголовки Ethernet и VLAN (внешней и внутренней сети),
 * а тег VLAN TCI и связанные флаги в структуре mbuf очищаются. Затем вновь добавляется
 * заголовок Ethernet, заполняются и проверяются его поля. Полученный в результате пакет
 * буферизуется (при наличии буфера) и пересылается.
 * @param[in] mbuf Пакет для пересылки
 * @param[in] tx_port_id Нномер сетевого порта для отправки пакетов
 * @param[in] queue_id Номер очереди исходящих пакетов
 * @param[out] tx_packet_buffer Буфер исходящих пакетов
 * @param[in,out] packet_stats Указатель на структуру со статистикой
 */
static inline
void forwardPacket(struct rte_mbuf* mbuf,
                   uint16_t tx_port_id,
                   uint16_t queue_id,
                   TxPacketBufferPtr tx_packet_buffer,
                   PacketStatsPtr packet_stats)
{
    if (!mbuf)
    {
        RTE_LOG(ERR, USER1, "Internal error: no packet\n");
        return;
    }

    if (!packet_stats)
        RTE_LOG(WARNING, USER1, "Internal error: no meter provided\n");

    cleanupVlanTciOuter(mbuf);
    cleanupVlanTciInner(mbuf);

    uint16_t ether_type, vlan_offset;
    struct rte_ether_hdr* ether_header = getEthernetHeader(mbuf, &ether_type, &vlan_offset);

    if (rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) != ether_type &&
        rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6) != ether_type)
    {
        if (rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP) == ether_type)
        {
            const struct rte_arp_hdr* arp_header = (const struct rte_arp_hdr*)((char*)(ether_header + 1) + vlan_offset);

            char buffer[INET_ADDRSTRLEN];
            RTE_LOG(DEBUG, USER1, "ARP packet dropped, target address: %s\n",
                    inet_ntop(AF_INET, &arp_header->arp_data.arp_tip, buffer, sizeof(buffer)));
        }

        if (!!packet_stats)
            __atomic_fetch_add(&packet_stats->drp_packet_count, 1, __ATOMIC_SEQ_CST);

        rte_pktmbuf_free(mbuf);
        return;
    }

    if (!rte_pktmbuf_adj(mbuf, (uint16_t)(sizeof(struct rte_ether_hdr) + vlan_offset)))
    {
        RTE_LOG(ERR, USER1, "Adjust failed: too big headers\n");

        if (!!packet_stats)
            __atomic_fetch_add(&packet_stats->proc_error_count, 1, __ATOMIC_SEQ_CST);

        rte_pktmbuf_free(mbuf);
        return;
    }

    if (rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) == ether_type)
    {
        const struct rte_ipv4_hdr* ipv4_header = rte_pktmbuf_mtod(mbuf, const struct rte_ipv4_hdr*);

        char buffer[INET_ADDRSTRLEN];
        RTE_LOG(DEBUG, USER1,"IPv4 packet received, target address: %s\n",
                inet_ntop(AF_INET, &ipv4_header->dst_addr, buffer, sizeof(buffer)));
    }
    else
    {
        const struct rte_ipv6_hdr* ipv6_header = rte_pktmbuf_mtod(mbuf, const struct rte_ipv6_hdr*);

        char buffer[INET6_ADDRSTRLEN];
        RTE_LOG(DEBUG, USER1, "IPv6 packet received, target address: %s\n",
                inet_ntop(AF_INET6, ipv6_header->dst_addr.a, buffer, sizeof(buffer)));
    }

    ether_header = (struct rte_ether_hdr*)rte_pktmbuf_prepend(mbuf, (uint16_t)sizeof(struct rte_ether_hdr));
    if (!ether_header)
    {
        RTE_LOG(ERR, USER1, "Prepend failed: no headroom\n");

        if (!!packet_stats)
            __atomic_fetch_add(&packet_stats->proc_error_count, 1, __ATOMIC_SEQ_CST);

        rte_pktmbuf_free(mbuf);
        return;
    }

    fillEthernetHeader(ether_header, ether_type, tx_port_id);

    uint16_t tx_packet_count;
    if (likely(tx_packet_buffer))
        tx_packet_count = rte_eth_tx_buffer(tx_port_id, queue_id, tx_packet_buffer, mbuf);
    else
    {
        RTE_LOG(DEBUG, USER1, "Internal error: no buffer provided\n");

        tx_packet_count = sendPackets(tx_port_id, queue_id, &mbuf, 1);
        if (!tx_packet_count)
        {
            resendPackets(&mbuf, 1, NULL);
            return;
        }
    }

    if (!!packet_stats)
    {
#ifndef NDEBUG
        __atomic_fetch_add(&packet_stats->tx_ops, 1, __ATOMIC_SEQ_CST);
#endif
        __atomic_fetch_add(&packet_stats->tx_packet_count, tx_packet_count, __ATOMIC_SEQ_CST);
    }
}

/**
 * @brief Цикл приёма и передачи пакетов
 * На каждое логическое ядро по одному циклу. Выполняется в отдельном
 * потоке и имеет свою пару очередей на приём и передачу пакетов
 * @param[in] argument Не используется
 * @return Всегда ноль
 */
static
int lcoreLoop(__attribute__((__unused__)) void* argument)
{
    LCoreConfigPtr lcore_config = &lcore_configs[rte_lcore_id()];

    assert(lcore_config->lcore_id == rte_lcore_id());

    const uint16_t rx_port_id = lcore_config->rx_port_id;
    const uint16_t tx_port_id = lcore_config->tx_port_id;
    const uint16_t queue_id = lcore_config->queue_id;

    struct rte_mbuf* rx_packet_buffer[PACKET_BURST_SIZE];
    TxPacketBufferPtr tx_packet_buffer = lcore_config->tx_packet_buffer;
    PacketStatsPtr packet_stats = &lcore_config->packet_stats;

    uint16_t packet_count, packet_number;
    while (is_running)
    {
        if (!(packet_count = rte_eth_rx_burst(rx_port_id, queue_id, rx_packet_buffer, PACKET_BURST_SIZE)))
        {
            RTE_LOG(DEBUG, USER1, "No packets available, lcore id: %u\n", rte_lcore_id());
            rte_delay_ms(RX_DELAY_SEC * 1000);
            continue;
        }

#ifndef NDEBUG
        __atomic_fetch_add(&packet_stats->rx_ops, 1, __ATOMIC_SEQ_CST);
#endif
        __atomic_fetch_add(&packet_stats->rx_packet_count, packet_count, __ATOMIC_SEQ_CST);

        for (packet_number = 0; packet_number < PACKET_PREFETCH_OFFSET && packet_number < packet_count; ++packet_number)
            rte_prefetch0(rte_pktmbuf_mtod(rx_packet_buffer[packet_number], void*));

        for (packet_number = 0; packet_number < (packet_count - PACKET_PREFETCH_OFFSET); ++packet_number)
        {
            rte_prefetch0(rte_pktmbuf_mtod(rx_packet_buffer[packet_number + PACKET_PREFETCH_OFFSET], void*));
            forwardPacket(rx_packet_buffer[packet_number], tx_port_id, queue_id, tx_packet_buffer, packet_stats);
        }

        for (; packet_number < packet_count; ++packet_number)
            forwardPacket(rx_packet_buffer[packet_number], tx_port_id, queue_id, tx_packet_buffer, packet_stats);
    }

    packet_count = rte_eth_tx_buffer_flush(tx_port_id, queue_id, tx_packet_buffer);
#ifndef NDEBUG
    __atomic_fetch_add(&packet_stats->tx_ops, 1, __ATOMIC_SEQ_CST);
#endif
    __atomic_fetch_add(&packet_stats->tx_packet_count, packet_count, __ATOMIC_SEQ_CST);

    return 0;
}

/**
 * @brief Запустить циклы приёма/передачи пакетов
 * @param[in] port_id Номер сетевого порта для приёма пакетов
 * @param[in,out] lcore_id Указатель на номер логического ядра
 * @return Количество запущенных циклов приёма/передачи пакетов
 */
static
unsigned startLcoreLoops(uint16_t port_id, unsigned* lcore_id)
{
    const PortConfigPtr rx_port_config = &port_configs[port_id];
    const PortConfigPtr tx_port_config = &port_configs[rte_eth_dev_is_valid_port(port_id ^ 1) ? port_id ^ 1 : port_id];

    int ret;
    unsigned lcore_loop_count = 0;
    for (uint16_t queue_id = 0; queue_id < rx_port_config->rx_queue_count; ++queue_id)
    {
        if ((*lcore_id = rte_get_next_lcore(*lcore_id, 1, 0)) >= RTE_MAX_LCORE)
        {
            RTE_LOG(WARNING, USER1, "Wrong usage: not enough cores\n");
            continue;
        }

        LCoreConfigPtr lcore_config = &lcore_configs[*lcore_id];
        lcore_config->lcore_id = *lcore_id;
        lcore_config->rx_port_id = rx_port_config->port_id;
        lcore_config->tx_port_id = tx_port_config->port_id;
        lcore_config->queue_id = queue_id;
        lcore_config->tx_packet_buffer = tx_packet_buffers[lcore_config->queue_id]
                                       = createTxPacketBuffer(PACKET_BURST_SIZE,
                                                              tx_port_config->port_id,
                                                              resendPackets);

        if (!lcore_config->tx_packet_buffer)
            RTE_LOG(ERR, USER1, "Failed to create TX buffer, lcore id: %u\n", lcore_config->lcore_id);

        if (!!(ret = rte_eal_remote_launch(lcoreLoop, NULL, lcore_config->lcore_id)))
        {
            RTE_LOG(ERR, USER1,
                    "Failed to start lcore loop (%u): %s\n",
                    lcore_config->lcore_id, rte_strerror(-ret));
            continue;
        }

        ++lcore_loop_count;
    }

    return lcore_loop_count;
}

/**
 * @brief Цикл сбора и вывода статистики
 * Статистика содержит количество принятых, пересланных и отоброшенных пакетов,
 * а также количество пакетов, при обработке или передаче которых произошли ошибки
 * @param[in] lcore_loop_count Количество запущенных циклов приёма/передачи пакетов
 */
static inline
void mainLoop(unsigned lcore_loop_count)
{
    assert(rte_get_main_lcore() == rte_lcore_id());

    if (rte_get_main_lcore() < lcore_loop_count)
        lcore_loop_count++;

    do
    {
        rte_delay_ms(POLL_DELAY_SEC * 1000);

        struct PacketStats packet_stats;
        memset(&packet_stats, 0, sizeof(packet_stats));

        unsigned lcore_id;
        RTE_LCORE_FOREACH_WORKER(lcore_id)
        {
#ifndef NDEBUG
            printf("[DBG][%s] lcore %u is %s\n",
                   __func__,
                   lcore_id,
                   rte_eal_get_lcore_state(lcore_id) == RUNNING ? "running" : "waiting");
            fflush(stdout);
#endif
            if (lcore_id >= lcore_loop_count)
            {
                RTE_LOG(DEBUG, USER1, "Wrong usage: lcore %hu is idle\n", lcore_id);
                continue;
            }

            const LCoreConfigPtr lcore_config = &lcore_configs[lcore_id];
            const PacketStatsPtr packet_stats_per_lcore = &lcore_config->packet_stats;

            packet_stats.rx_packet_count += packet_stats_per_lcore->rx_packet_count;
            packet_stats.tx_packet_count += packet_stats_per_lcore->tx_packet_count;
            packet_stats.drp_packet_count += packet_stats_per_lcore->drp_packet_count;
            packet_stats.proc_error_count += packet_stats_per_lcore->proc_error_count;
#ifndef NDEBUG
            packet_stats.rx_ops += packet_stats_per_lcore->rx_ops;
            packet_stats.tx_ops += packet_stats_per_lcore->tx_ops;
            packet_stats.retx_ops += packet_stats_per_lcore->retx_ops;
#endif
        }

        printf("RX packets: %lu\n" \
               "TX packets: %lu\n" \
               "Dropped packets: %lu\n" \
               "Processing errors: %lu\n",
               packet_stats.rx_packet_count,
               packet_stats.tx_packet_count,
               packet_stats.drp_packet_count,
               packet_stats.proc_error_count);
#ifndef NDEBUG
        printf("[DBG] RX operations: %lu\n" \
               "[DBG] TX operations: %lu\n" \
               "[DBG] ReTX operations: %lu\n",
               packet_stats.rx_ops,
               packet_stats.tx_ops,
               packet_stats.retx_ops);
#endif
        fflush(stdout);
    } while (is_running);
}

void startForwarder(int argc, char** argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL Initialization failed: %s\n", rte_strerror(rte_errno));

    argc -= ret;
    argv += ret;

    uint16_t rx_queue_count = RX_QUEUE_COUNT;
    if (getOption(argc, argv, 'q', &rx_queue_count) &&
        rx_queue_count > MAX_RX_QUEUE_PER_PORT)
    {
        printf("Wrong usage: bad argument value (q)\n");
        exit(EXIT_FAILURE);
    }

    uint16_t rx_port_number = -1;
    if (getOption(argc, argv, 'p', &rx_port_number) &&
        !rte_eth_dev_is_valid_port(rx_port_number))
    {
        printf("Wrong usage: bad argument value (p)\n");
        exit(EXIT_FAILURE);
    }

    if (!rte_eth_dev_count_avail())
    {
        printf("Wrong usage: no devices available\n"
               "Total number of devices: %hu\n",
               rte_eth_dev_count_total());
        exit(EXIT_FAILURE);
    }

    struct rte_mempool* mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
                                                            NUM_MBUFS,
                                                            MBUF_CACHE_SIZE,
                                                            0,
                                                            RTE_MBUF_DEFAULT_BUF_SIZE,
                                                            rte_socket_id());
    if (!mbuf_pool)
        rte_panic("Failed to create memory pool: %s\n", rte_strerror(rte_errno));

    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id)
    {
        PortConfigPtr port_config = &port_configs[port_id];
        port_config->port_id = port_id;
        port_config->socket_id = SOCKET_ID_ANY;
        port_config->rx_desc_count = RX_QUEUE_SIZE;
        port_config->tx_desc_count = TX_QUEUE_SIZE;
        port_config->rx_queue_count = rx_queue_count;
        port_config->tx_queue_count = rx_queue_count;

        if (!configurePort(port_config, mbuf_pool))
            rte_exit(EXIT_FAILURE, "Failed to configure port %hu\n", port_config->port_id);

        if (!bringUpPort(port_config, true))
            rte_exit(EXIT_FAILURE, "Failed to bring up port %hu\n", port_config->port_id);
    }

    is_running = true;

    unsigned lcore_id = -1;
    unsigned lcore_loop_count = 0;
    if (rx_port_number != (uint16_t)-1)
        lcore_loop_count = startLcoreLoops(rx_port_number, &lcore_id);
    else
        RTE_ETH_FOREACH_DEV(port_id)
            lcore_loop_count += startLcoreLoops(port_id, &lcore_id);

    if (likely(lcore_loop_count))
    {
        mainLoop(lcore_loop_count);
        rte_eal_mp_wait_lcore();
    }
    else
        is_running = false;

    freeTxPacketBuffers(tx_packet_buffers, RTE_MAX_LCORE);

    rte_eal_cleanup();
}
