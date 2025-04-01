#include <stdio.h>
#include <stdlib.h>

#include <time.h>
#include <assert.h>

#include <rte_log.h>

#include <rte_pause.h>

#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>

#include "packet_forwarder.h"

#include "config.h"

#include "types.h"
#include "utils.h"
#include "dpdk_utils.h"
#include "dpdk_port.h"

#define DEF_RX_QUEUE_COUNT 3
#define MAX_RX_QUEUE_PER_PORT 16

#define PACKET_BURST_SIZE 32
#define PACKET_PREFETCH_OFFSET 3

#ifdef SLOW_MOTION
#define TX_DELAY_MS 10
#define RX_DELAY_SEC 2
#define POLL_DELAY_SEC 3
#define MAX_SEND_RETRIES 10
#else
#define RX_DELAY_SEC 1
#define POLL_DELAY_SEC 2
#define MAX_SEND_RETRIES 3
#endif

#define NEARBY_PORT(p) \
    ({ __typeof__ (p) _p = (p); \
       __typeof__ (p) _np = _p ^ 1; \
       (_np < RTE_MAX_ETHPORTS) && rte_eth_dev_is_valid_port(_np) ? _np : _p; })

volatile bool is_running;

static LCoreConfigs lcore_configs;

/**
 * \brief Очистить тег VLAN TCI (внешней сети) и связанные флаги
 * \details Обнуляется поле mbuf->vlan_tci_outer и снимаются соответствующие
 * биты флага mbuf->ol_flags. Заголовоки VLAN (внутренней и внешней сети) из
 * самого кадра Ethernet, хранящегося в данных пакета, удаляются раньше
 * (оборудованием/драйвером, средствами DPDK) или позже в функции forwardPacket()
 * после их обрабокти
 * \note Если есть флаг RTE_MBUF_F_RX_QINQ, то флаг RTE_MBUF_F_RX_VLAN
 * тоже должен быть установлен
 * \warning Нет проверки на нулевой указатель, только для использования
 * внутри функции forwardPacket(). Вынесена для повышение читаемости кода
 * \param[in] mbuf Пакет
 * \return
 * 0 - если флага RTE_MBUF_F_RX_QINQ не было и очистка не выполнялась
 * 1 - если флаг RTE_MBUF_F_RX_QINQ был и очистка выполнена
 */
static inline
bool cleanVlanTciOuter(struct rte_mbuf* mbuf)
{
    if (!(mbuf->ol_flags & RTE_MBUF_F_RX_QINQ))
        return false;

    if (mbuf->ol_flags & RTE_MBUF_F_RX_QINQ_STRIPPED)
    {
        RTE_LOG(DEBUG, USER1, "VLAN stripping must be disabled\n");
        mbuf->ol_flags &= ~RTE_MBUF_F_RX_QINQ_STRIPPED;
    }

    mbuf->vlan_tci_outer = 0;
    mbuf->ol_flags &= ~RTE_MBUF_F_RX_QINQ;

    return true;
}

/**
 * \brief Очистить тег VLAN TCI (внутренней сети) и связанные флаги
 * \details Обнуляется поле mbuf->vlan_tci и снимаются соответствующие биты
 * флага mbuf->ol_flags. Заголовоки VLAN (внутренней и внешней сети) из
 * самого кадра Ethernet, хранящегося в данных пакета, удаляются раньше
 * (оборудованием/драйвером, средствами DPDK) или позже в функции forwardPacket()
 * после их обрабокти
 * \warning Нет проверки на нулевой указатель, только для использования
 * внутри функции forwardPacket(). Вынесена для повышение читаемости кода
 * \param[in] mbuf Пакет
 * \return
 * 0 - если флага RTE_MBUF_F_RX_VLAN не было и очистка не выполнялась
 * 1 - если флаг RTE_MBUF_F_RX_VLAN был и очистка выполнена
 */
static inline
bool cleanVlanTciInner(struct rte_mbuf* mbuf)
{
    if (!(mbuf->ol_flags & RTE_MBUF_F_RX_VLAN))
        return false;

    if (mbuf->ol_flags & RTE_MBUF_F_RX_VLAN_STRIPPED)
    {
        RTE_LOG(DEBUG, USER1, "VLAN stripping must be disabled\n");
        mbuf->ol_flags &= ~RTE_MBUF_F_RX_VLAN_STRIPPED;
    }

    mbuf->vlan_tci = 0;
    mbuf->ol_flags &= ~RTE_MBUF_F_RX_VLAN;

    return true;
}

/**
 * \brief Очистить теги VLAN TCI (внутренней и внешней сети) и связанные флаги
 * \details Обнуляются поля mbuf->vlan_tci и mbuf->vlan_tci_outer, а также
 * снимаются соответствующие биты флага mbuf->ol_flags. Заголовоки VLAN (внутренней
 * и внешней сети) из самого кадра Ethernet, хранящегося в данных пакета,
 * удаляются раньше (оборудованием/драйвером, средствами DPDK) или позже в функции
 * forwardPacket() после их обрабокти
 * \param[in] mbuf Пакет
 */
static inline
void cleanVlanTci(struct rte_mbuf* mbuf)
{
    if (!cleanVlanTciInner(mbuf))
        return;

    cleanVlanTciOuter(mbuf);
}

/**
 * \brief Получить заголовок Ethernet
 * \details Возвращает указатель на заголовок Ethernet в переданном пакете,
 * а также тип Ethernet кадра и смещение в байтах на размер заголовков VLAN
 * при их наличии, которое нужно учитывать при работе с данными пакета
 * \warning Нет проверки на нулевые указателт, только для использования
 * внутри функции forwardPacket(). Вынесена для повышение читаемости кода
 * \param[in] mbuf Пакет
 * \param[out] ether_type Тип кадра Ethernet
 * \param[out] vlan_offset Суммарный размер заголовков VLAN
 * \return Указатель на заголовок Ethernet
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
 * \brief Заполнить заголовок Ethernet
 * \details В качестве MAC-адреса получателя используется сгенерированный
 * MAC-адрес 5E:BA:0F:CE:0A:XX, где XX - случайное число от 0 до 255, или
 * случайный (не мультикаст), если первый был некорректен.
 * В качестве MAC-адреса отправителя используется реальный порта отправки
 * или случайный (тоже не мультикаст), если первый не удалось получить
 * \warning Нет проверки на нулевой указатель, только для использования
 * внутри функции forwardPacket(). Вынесена для повышение читаемости кода
 * \param[out] ether_header Указатель на заголовок Ethernet
 * \param[in] ether_type Тип кадра Ethernet
 * \param[in] tx_port_id Номер порта для отправки пакета
 */
static inline
void fillEthernetHeader(struct rte_ether_hdr* ether_header,
                        uint16_t ether_type,
                        uint16_t tx_port_id)
{
    // Слишком часто, лучше перенести в lcoreLoop()
    // и вызывать перед чтением пакетов из очереди!
    rte_srand(rte_rdtsc());
    const uint64_t random_number = (rte_rand() % 256) << 40;

    uint8_t* target_mac_addr = (uint8_t*)&ether_header->dst_addr.addr_bytes[0];
    // При заполнении адреса получателя используется 6 младших байт (LE).
    // Оставшиеся 2 старших байта можно обнулить или заполнить любыми
    // значениями, так как это поле является первым в структуре, а
    // остальные два заполняются в этой же функции позже, поэтому даже
    // если залезть двумя байтами в адрес отправителя, то они всё равно
    // будут перезаписаны реальным адресом порта отправки
    *((uint64_t*)target_mac_addr) = 0xE0A5FBE0AC + random_number;
    if (!rte_is_valid_assigned_ether_addr(&ether_header->dst_addr))
        rte_eth_random_addr(target_mac_addr);

    struct rte_ether_addr source_mac_addr;
    if (!!rte_eth_macaddr_get(tx_port_id, &source_mac_addr))
        rte_eth_random_addr(&source_mac_addr.addr_bytes[0]);
    rte_ether_addr_copy(&source_mac_addr, &ether_header->src_addr);

    ether_header->ether_type = ether_type;
}

/**
 * \brief Отправить пакеты
 * \details Один или несколько, в цикле с задержкой, если с первого раза
 * отправить все пакеты не удалось. Количество попыток и задержка между
 * ними задаются соответствующими макросами
 * \warning Эту функцию нельзя вызывать напрямую. Она ничего не проверяет
 * (в том числе указатели на ноль) и ничего не считает. Вызывается только
 * из функций trySendPacket() и resendPackets()
 * \param[in] Указатель на конфигурацию логического ядра
 * \param[in] packets Массив отправляемых пакетов
 * \param[in] packet_count Количество отправляемых пакетов
 * \return Количество отправленных пакетов
 */
static inline
uint16_t sendPackets(LCoreConfigConstPtr lcore_config,
                     struct rte_mbuf** packets,
                     uint16_t packet_count)
{
    uint8_t retry_count = 0;
    uint16_t sent_packet_count, packet_number = 0;
    do {
#ifdef SLOW_MOTION
        if (retry_count) rte_delay_ms(TX_DELAY_MS);
#else
        if (retry_count) rte_pause();
#endif
        sent_packet_count = rte_eth_tx_burst(lcore_config->tx_port_id,
                                             lcore_config->queue_id,
                                             &packets[packet_number],
                                             packet_count);

        packet_count -= sent_packet_count;
        packet_number += sent_packet_count;
    } while(packet_count && (++retry_count < MAX_SEND_RETRIES));

    return packet_number;
}

/**
 * \brief Повторить отправку пакетов
 * \details Функция обратного вызова - обработчик ошибок отправки пакетов.
 * Проверяет пакеты и отправляет их повторно, делая дамп некорректных
 * (точно некорректный только один, первый найденный, остальные не проверяются)
 * и неотправленных (повторно) пакетов
 * \note Всё проверяет и считает, можно вызывать откуда угодно. Указатель на
 * статистику хранится внутри конфигурации логического ядра. Статистика будет
 * изменяться здесь и в подобных функциях несмотря на то, что сама конфигурация
 * константна. Структура со статистикой помечена volatile, а её поля изменяются
 * атомарно с помощью функционала GCC. Статистика может не собираться, такое
 * использование является допустимым, запись об этом в лог сделает основной поток
 * на уровне WARNING.
 * Для целей отладки считается количество успешных операций, т.е. вместе со
 * счётчком отправленных пакетов. Также здесь считается количество некорректных
 * пакетов и пакетов, которые всё же не удалось отправить - за это отвечает один
 * и тот же счётчик, который также считает количество пакетов, при обработке
 * которых произошли ошибки
 * \param[in] unsent_packets Массив неотправленных пакетов
 * \param[in] unsent_packet_count Количество неотправленных пакетов
 * \param[in] user_data Указатель на конфигурацию логического ядра
 */
static
void resendPackets(struct rte_mbuf** unsent_packets,
                   uint16_t unsent_packet_count,
                   const void* user_data)
{
    LCoreConfigConstPtr lcore_config = (LCoreConfigConstPtr)user_data;
    if (!lcore_config)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%u] Internal error: no configuration\n",
                __func__, rte_lcore_id());
        return;
    }

    assert(lcore_config->lcore_id == rte_lcore_id());

    if (!unsent_packets)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%u] Internal error: no packets\n",
                __func__, lcore_config->lcore_id);
        return;
    }

    const uint16_t prepared_packet_count = rte_eth_tx_prepare(lcore_config->tx_port_id,
                                                              lcore_config->queue_id,
                                                              unsent_packets,
                                                              unsent_packet_count);
    if (prepared_packet_count < unsent_packet_count)
    {
        RTE_LOG(ERR, USER1,
                "Failed to prepare %hu packets: %s\n",
                unsent_packet_count - prepared_packet_count, rte_strerror(rte_errno));

        if (!!lcore_config->packet_stats)
            __atomic_fetch_add(&lcore_config->packet_stats->proc_error_count,
                               unsent_packet_count - prepared_packet_count,
                               __ATOMIC_SEQ_CST);

        dumpAndFreePackets(&unsent_packets[prepared_packet_count],
                           unsent_packet_count - prepared_packet_count);
    }

    if (!prepared_packet_count)
        return;

    const uint16_t sent_packet_count = sendPackets(lcore_config,
                                                   unsent_packets,
                                                   prepared_packet_count);
    if (sent_packet_count < prepared_packet_count)
    {
        RTE_LOG(ERR, USER1,
                "Failed to send %hu packets\n",
                prepared_packet_count - sent_packet_count);

        if (!!lcore_config->packet_stats)
            __atomic_fetch_add(&lcore_config->packet_stats->proc_error_count,
                               prepared_packet_count - sent_packet_count,
                               __ATOMIC_SEQ_CST);

        dumpAndFreePackets(&unsent_packets[sent_packet_count],
                           prepared_packet_count - sent_packet_count);
    }

    if (!sent_packet_count)
        return;

    if (!!lcore_config->packet_stats)
    {
#ifndef NDEBUG
        __atomic_fetch_add(&lcore_config->packet_stats->retx_ops, 1, __ATOMIC_SEQ_CST);
#endif
        __atomic_fetch_add(&lcore_config->packet_stats->tx_packet_count,
                           sent_packet_count,
                           __ATOMIC_SEQ_CST);
    }
}

/**
 * \brief Попытаться отправить пакет
 * \details Попробовать добавить пакет в буфер исходящих пакетов, в случае
 * его отсутствия - попробовать отправить пакет напрямую, если это
 * не удалось, то попробовать отправить пакет повторно с
 * предварительной проверкой и, при необходимости, последующим дампом
 * \warning Эту функцию нельзя вызывать напрямую. Она ничего не проверяет
 * (в том числе указатели на ноль), но ведёт подсчёт статистики.
 * Вызывается только из функций forwardPacket(). Вынесена для повышение
 * читаемости кода
 * \note Допустимо использование без буфера, об этом будет запись в лог
 * на уровне DEBUG. Подробности в примечании к функции resendPackets()
 * про статистику
 * \param[in] lcore_config Указатель на конфигурацию логического ядра
 * \param[in] mbuf Отправляемый пакет
 */
static inline
void trySendPacket(LCoreConfigConstPtr lcore_config, struct rte_mbuf* mbuf)
{
    uint16_t tx_packet_count;
    if (likely(lcore_config->tx_packet_buffer))
        tx_packet_count = rte_eth_tx_buffer(lcore_config->tx_port_id,
                                            lcore_config->queue_id,
                                            lcore_config->tx_packet_buffer,
                                            mbuf);
    else
    {
        RTE_LOG(DEBUG, USER1,
                "[%s][%u] Internal error: no buffer\n",
                __func__, lcore_config->lcore_id);

        tx_packet_count = sendPackets(lcore_config, &mbuf, 1);
        if (!tx_packet_count)
        {
            resendPackets(&mbuf, 1, lcore_config);
            return;
        }
    }

    if (!!lcore_config->packet_stats)
    {
#ifndef NDEBUG
        __atomic_fetch_add(&lcore_config->packet_stats->tx_ops, 1, __ATOMIC_SEQ_CST);
#endif
        __atomic_fetch_add(&lcore_config->packet_stats->tx_packet_count,
                           tx_packet_count,
                           __ATOMIC_SEQ_CST);
    }
}

/**
 * \brief Переслать пакет
 * \details Из пакета удаляются заголовки Ethernet и VLAN (внешней и внутренней сети),
 * а тег VLAN TCI и связанные флаги в структуре mbuf очищаются. Затем вновь
 * добавляется заголовок Ethernet, заполняются и проверяются его поля.
 * Полученный в результате пакет буферизуется (при наличии буфера) и пересылается.
 * Адрес получателя для пакетов IPv4/6 и ARP логируется на уровне DEBUG
 * \warning Эту функцию нельзя вызывать напрямую. Она ничего не проверяет
 * (в том числе указатели на ноль), но ведёт подсчёт статистики.
 * Вызывается только из функций forwardPacket(). Вынесена для повышение
 * читаемости кода
 * \note Здесь считается количество отброшенных пакетов, а также считается
 * количество пакетов, при обработке которых произошли ошибки. Подробности
 * в примечании к функции resendPackets() про статистику
 * \param[in] lcore_config Указатель на конфигурацию логического ядра
 * \param[in] mbuf Пакет для пересылки
 */
static inline
void forwardPacket(LCoreConfigConstPtr lcore_config, struct rte_mbuf* mbuf)
{
    assert(!!lcore_config && !!mbuf);

    cleanVlanTci(mbuf);

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

        if (!!lcore_config->packet_stats)
            __atomic_fetch_add(&lcore_config->packet_stats->drp_packet_count, 1, __ATOMIC_SEQ_CST);

        rte_pktmbuf_free(mbuf);
        return;
    }

    if (!rte_pktmbuf_adj(mbuf, (uint16_t)(sizeof(struct rte_ether_hdr) + vlan_offset)))
    {
        RTE_LOG(ERR, USER1, "Adjust failed: too big headers\n");

        if (!!lcore_config->packet_stats)
            __atomic_fetch_add(&lcore_config->packet_stats->proc_error_count, 1, __ATOMIC_SEQ_CST);

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

        if (!!lcore_config->packet_stats)
            __atomic_fetch_add(&lcore_config->packet_stats->proc_error_count, 1, __ATOMIC_SEQ_CST);

        rte_pktmbuf_free(mbuf);
        return;
    }

    fillEthernetHeader(ether_header, ether_type, lcore_config->tx_port_id);
    trySendPacket(lcore_config, mbuf);
}

/**
 * \brief Цикл приёма/передачи пакетов
 * \details На каждое логическое ядро по одному циклу. Выполняется в отдельном
 * потоке и имеет свою пару очередей на приём/передачу пакетов
 * \note Здесь считается количество принятых пакетов, а также отправленных
 * в результате принудительной очистки буфера (если он есть) при завершении
 * работы. Подробности в примечании к функции resendPackets() про статистику
 * \param[in] argument Указатель на конфигурацию логического ядра
 * \return
 * EXIT_SUCCESS - в случае планового завершения (по флагу is_running)
 * EXIT_FAILURE - в случае отсутствия конфигурации
 */
static
int lcoreLoop(void* argument)
{
    LCoreConfigConstPtr lcore_config = (LCoreConfigConstPtr)argument;
    if (!lcore_config)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%u] Internal error: no configuration\n",
                __func__, rte_lcore_id());
        return EXIT_FAILURE;
    }

    assert(lcore_config->lcore_id == rte_lcore_id());

    uint16_t packet_count, packet_number;
    struct rte_mbuf* rx_packet_buffer[PACKET_BURST_SIZE];

    while (is_running)
    {
        if (!(packet_count = rte_eth_rx_burst(lcore_config->rx_port_id,
                                              lcore_config->queue_id,
                                              rx_packet_buffer,
                                              PACKET_BURST_SIZE)))
        {
            RTE_LOG(DEBUG, USER1,
                    "[%u][%hu:%hu] No packets available\n",
                    lcore_config->lcore_id,
                    lcore_config->rx_port_id,
                    lcore_config->queue_id);

            rte_delay_ms(RX_DELAY_SEC * 1000);
            continue;
        }

        if (!!lcore_config->packet_stats)
        {
#ifndef NDEBUG
            __atomic_fetch_add(&lcore_config->packet_stats->rx_ops, 1, __ATOMIC_SEQ_CST);
#endif
            __atomic_fetch_add(&lcore_config->packet_stats->rx_packet_count,
                               packet_count,
                               __ATOMIC_SEQ_CST);
        }

        for (packet_number = 0;
             (packet_number < PACKET_PREFETCH_OFFSET) && (packet_number < packet_count);
             ++packet_number)
        {
            rte_prefetch0(
                rte_pktmbuf_mtod(
                    rx_packet_buffer[packet_number],
                    void*
                )
            );
        }

        for (packet_number = 0;
             packet_number < (packet_count - PACKET_PREFETCH_OFFSET);
             ++packet_number)
        {
            rte_prefetch0(
                rte_pktmbuf_mtod(
                    rx_packet_buffer[packet_number + PACKET_PREFETCH_OFFSET],
                    void*
                )
            );

            forwardPacket(lcore_config, rx_packet_buffer[packet_number]);
        }

        for (; packet_number < packet_count; ++packet_number)
            forwardPacket(lcore_config, rx_packet_buffer[packet_number]);
    }

    if (!lcore_config->tx_packet_buffer)
    {
        RTE_LOG(DEBUG, USER1,
                "[%s][%u] Internal error: no buffer\n",
                __func__, lcore_config->lcore_id);

        return EXIT_SUCCESS;
    }

    if (!!(packet_count = rte_eth_tx_buffer_flush(lcore_config->tx_port_id,
                                                  lcore_config->queue_id,
                                                  lcore_config->tx_packet_buffer))
        && !!lcore_config->packet_stats)
    {
#ifndef NDEBUG
        __atomic_fetch_add(&lcore_config->packet_stats->tx_ops, 1, __ATOMIC_SEQ_CST);
#endif
        __atomic_fetch_add(&lcore_config->packet_stats->tx_packet_count,
                           packet_count,
                           __ATOMIC_SEQ_CST);
    }

    return EXIT_SUCCESS;
}

/**
 * \brief Запустить циклы приёма/передачи пакетов
 * \param[in,out] lcore_id Указатель на номер логического ядра
 * \param[in] rx_port_config Указатель на конфигурацию порта приёма
 * \param[in] tx_port_config Указатель на конфигурацию порта отправки
 * \return Количество запущенных циклов приёма/передачи пакетов
 */
static
unsigned startLcoreLoops(unsigned* lcore_id,
                         PortConfigConstPtr rx_port_config,
                         PortConfigConstPtr tx_port_config)
{
    if (!rx_port_config || !tx_port_config)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%u] Internal error: no configuration\n",
                __func__, *lcore_id);
        return 0;
    }

    int ret;
    unsigned lcore_loop_count = 0;
    for (uint16_t queue_id = 0; queue_id < rx_port_config->rx_queue_count; ++queue_id)
    {
        if ((*lcore_id = rte_get_next_lcore(*lcore_id, 1, 0)) >= RTE_MAX_LCORE)
        {
            RTE_LOG(WARNING, USER1,
                    "[%hu:%hu] Wrong usage: not enough lcores\n",
                    rx_port_config->port_id,
                    queue_id);
            break;
        }

        LCoreConfigPtr lcore_config = &lcore_configs[*lcore_id];
        lcore_config->lcore_id = *lcore_id;
        lcore_config->rx_port_id = rx_port_config->port_id;
        lcore_config->tx_port_id = tx_port_config->port_id;
        lcore_config->queue_id = queue_id;
        lcore_config->packet_stats = calloc(1, sizeof(PacketStats));

        createTxPacketBuffer(lcore_config,
                             PACKET_BURST_SIZE,
                             resendPackets);

        if (!!(ret = rte_eal_remote_launch(lcoreLoop,
                                           lcore_config,
                                           lcore_config->lcore_id)))
        {
            RTE_LOG(ERR, USER1,
                    "Failed to start lcore loop %u: %s\n",
                    lcore_config->lcore_id, rte_strerror(-ret));
            continue;
        }

        ++lcore_loop_count;
    }

    return lcore_loop_count;
}

/**
 * \brief Цикл сбора и вывода статистики
 * \details Статистика содержит количество принятых, пересланных и отоброшенных пакетов,
 * а также количество пакетов, при обработке или передаче которых произошли ошибки
 * \note При наличии простаивающих логических ядер в лог будет добавлено предупреждение
 * об этом. Потоки могут находится в состоянии ожидания по двум причинам: их изначально
 * было больше, чем нужно (а нужно КОЛ-ВО ПОРТОВ * КОЛ-ВО ПАР ОЧЕРЕДЕЙ + СТАТИСТИКА),
 * или потому что для каких-то из них функция lcoreLoop() завершилась при запуске из-за
 * отсутствия конфигурации логического ядра. Статистика может не собираться - это
 * допустимо, но в лог будет выводиться предупреждение об этом ("no meter").
 * \warning Этот цикл не реагирует на флаг is_running, он ждёт завершения работы потоков,
 * которые пересылают пакеты, что собрать полную статистику.
 * \param[in] lcore_loop_count Количество запущенных циклов приёма/передачи пакетов
 */
static inline
void mainLoop(unsigned lcore_loop_count)
{
    assert(rte_get_main_lcore() == rte_lcore_id());

    do
    {
        rte_delay_ms(POLL_DELAY_SEC * 1000);

        PacketStats packet_stats;
        memset(&packet_stats, 0, sizeof(packet_stats));

        unsigned lcore_id, lcore_count = 1;
        RTE_LCORE_FOREACH_WORKER(lcore_id)
        {
#ifndef NDEBUG
            printf("[DBG] lcore %u is %s\n",
                   lcore_id, rte_eal_get_lcore_state(lcore_id) == RUNNING ? "running" : "waiting");
            fflush(stdout);
#endif
            if (lcore_count > lcore_loop_count)
            {
                RTE_LOG(WARNING, USER1, "Wrong usage: lcore %hu is idle\n", lcore_id);
                continue;
            }

            if (rte_eal_get_lcore_state(lcore_id) == RUNNING)
                ++lcore_count;
            else
                --lcore_loop_count;

            PacketStatsPtr packet_stats_per_lcore = lcore_configs[lcore_id].packet_stats;
            if (!packet_stats_per_lcore)
            {
                RTE_LOG(WARNING, USER1, "[%u] Internal error: no meter\n", lcore_id);
                continue;
            }

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
        printf("[DBG] is_running flag is %s\n",
               is_running ? "TRUE" : "FALSE");
#endif
        fflush(stdout);
    } while (lcore_loop_count);
}

void startForwarder(int argc, char** argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
    {
        printf("EAL initialization failed: %d\n", rte_errno);
        exit(EXIT_FAILURE);
    }

    argc -= ret;
    argv += ret;

    /**
     * \brief Требуемое количество пар очередй приёма/передачи
     * \details Инициализируется значением по умолчанию, а установленное в
     * результате проверки возможностей драйвера будет выведено в лог.
     * Очередь приёма пакетов находится на порту приёма, а очередь отправки,
     * соответственно, на порту оптравки, а пересылкой занимается один поток
     * (логическое ядро DPDK). Количество очередей на отправку не должно быть
     * меньше количетсва очередей на приём, выбирается наименьшее из них. Всё
     * это происходит в функции adjustQueueCount(), уровень логирования - INFO
     */
    uint16_t req_rx_queue_count = DEF_RX_QUEUE_COUNT;
    if (getOption(argc, argv, 'q', &req_rx_queue_count) &&
        req_rx_queue_count > MAX_RX_QUEUE_PER_PORT)
        rte_exit(EXIT_FAILURE, "Wrong usage: bad argument value (q)\n");

    uint16_t rx_port_number = -1;
    if (getOption(argc, argv, 'p', &rx_port_number) &&
        !rte_eth_dev_is_valid_port(rx_port_number))
        rte_exit(EXIT_FAILURE, "Wrong usage: bad argument value (p)\n");

    if (!rte_eth_dev_count_avail())
        rte_exit(EXIT_FAILURE,
                 "Wrong usage: no devices available\n"
                 "Total number of devices: %hu\n",
                 rte_eth_dev_count_total());

    if (!(rte_lcore_count() > 1))
        rte_exit(EXIT_FAILURE, "Wrong usage: not enough lcores\n");

    PortConfigs port_configs;
    startAllDevices(port_configs, req_rx_queue_count);

    is_running = true;

    unsigned lcore_id = -1;
    unsigned lcore_loop_count = 0;
    if (rx_port_number != (uint16_t)-1)
        lcore_loop_count = startLcoreLoops(&lcore_id,
                                           &port_configs[rx_port_number],
                                           &port_configs[NEARBY_PORT(rx_port_number)]);
    else
    {
        uint16_t port_id;
        RTE_ETH_FOREACH_DEV(port_id)
            lcore_loop_count += startLcoreLoops(&lcore_id,
                                                &port_configs[port_id],
                                                &port_configs[NEARBY_PORT(port_id)]);
    }

    if (likely(lcore_loop_count))
    {
        mainLoop(lcore_loop_count);
        rte_eal_mp_wait_lcore();
    }
    else
    {
        printf("Failed to start lcore loops\n");
        fflush(stdout);
    }

    is_running = false;

    RTE_LCORE_FOREACH_WORKER(lcore_id)
    {
        LCoreConfigPtr lcore_config = &lcore_configs[lcore_id];

        freeTxPacketBuffer(lcore_config);

        if (!!lcore_config->packet_stats)
        {
            free((void*)lcore_config->packet_stats);
            lcore_config->packet_stats = NULL;
        }

    }

    stopAllDevices();
    if (!!(ret = rte_eal_cleanup()))
    {
        printf("EAL cleanup failed: %d\n", -ret);
        fflush(stdout);
    }
}
