#include <memory.h>
#include <assert.h>

#include <rte_log.h>
#include <rte_errno.h>
#include <rte_debug.h>

#include <rte_flow.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <rte_ethdev.h>

#include "dpdk_port.h"

#include "config.h"

#ifdef THRESHOLDS_OPTIMIZATION
#include "dpdk_thresh.h"
#endif

#define MIN(a,b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
       _a < _b ? _a : _b; })

#define NUM_MBUFS 4095
#define MBUF_CACHE_SIZE 195

#define RX_QUEUE_SIZE 256
#define TX_QUEUE_SIZE 256

static struct rte_mempool* mbuf_pool;

/**
 * \brief Вывести MAC-адрес сетевого порта в лог (уровень INFO)
 * \param[in] port_id Номер сетевого порта
 * \return Результат (успешность) выполнения операции
 */
static inline
bool logMAC(uint16_t port_id)
{
    struct rte_ether_addr mac_addr;
    int ret = rte_eth_macaddr_get(port_id, &mac_addr);
    if (!!ret)
    {
        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_macaddr_get() failed: %s\n",
                port_id, rte_strerror(-ret));
        return false;
    }

    RTE_LOG(INFO, USER1,
            "[%hu] MAC: " RTE_ETHER_ADDR_PRT_FMT "\n",
            port_id, RTE_ETHER_ADDR_BYTES(&mac_addr));

    return true;
}

/**
 * \brief Подогнать количество очередей приёма/передачи под возможности порта
 * \details Проверить количество очередей приёма/передачи на соответствие
 * ограничениям из информации об устройстве Ethernet и, при необходимости,
 * привести их в соответствие границам
 * \param[in] port_config Конфигурация сетевого порта
 * \param[in] dev_info Информация об устройстве Ethernet
 */
static inline
void adjustQueueCount(PortConfigPtr port_config, const struct rte_eth_dev_info* dev_info)
{
    port_config->rx_queue_count = MIN(port_config->rx_queue_count, dev_info->max_rx_queues);
    port_config->tx_queue_count = MIN(port_config->tx_queue_count, dev_info->max_tx_queues);
    port_config->rx_queue_count = MIN(port_config->rx_queue_count, port_config->tx_queue_count);

    RTE_LOG(INFO, USER1,
            "[%hu] RX/TX queue count: %hu/%hu\n",
            port_config->port_id,
            port_config->rx_queue_count,
            port_config->tx_queue_count);
}

/**
 * \brief Подогнать размеры очередей приёма/передачи под возможности порта
 * \details Проверить размеры очередей приёма/передачи на соответствие
 * ограничениям из информации об устройстве Ethernet и, при необходимости,
 * привести их в соответствие границам
 * \param[in] port_config Конфигурация сетевого порта
 * @return Результат (успешность) выполнения операции
 */
static inline
bool adjustQueueSize(PortConfigPtr port_config)
{
    int ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_config->port_id,
                                               &port_config->rx_queue_size,
                                               &port_config->tx_queue_size);
    if (!!ret)
    {
        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_dev_adjust_nb_rx_tx_desc() failed: %s\n",
                port_config->port_id,
                rte_strerror(-ret));
        return false;
    }

    RTE_LOG(INFO, USER1,
            "[%hu] RX/TX queue size: %hu/%hu\n",
            port_config->port_id,
            port_config->rx_queue_size,
            port_config->tx_queue_size);
    return true;
}

/**
 * \brief Настроить очереди приёма пакетов
 * \details Выделяет память и выполняет настройку очередей входящих
 * пакетов в соответствии с переданной конфигурацией сетевого порта
 * \param[in] port_config Конфигурация сетевого порта
 * \param[in] rx_offload_flags Флаги разгрузки RX
 * \param[in] dev_info Информация об устройстве Ethernet
 * \param[in] eth_conf Конфигурация порта Ethernet
 * \return Результат (успешность) выполнения операции
 */
static inline
bool setUpRxQueues(PortConfigConstPtr port_config,
#ifdef DISABLE_VLAN_STRIPPING_PER_QUEUE
                   uint64_t rx_offload_flags,
#endif
                   const struct rte_eth_dev_info* dev_info,
                   const struct rte_eth_conf* eth_conf)
{
    struct rte_eth_rxconf rx_conf = dev_info->default_rxconf;
    rx_conf.offloads = eth_conf->rxmode.offloads;

#ifdef DISABLE_VLAN_STRIPPING_PER_QUEUE
    if (dev_info->rx_queue_offload_capa & rx_offload_flags)
        rx_conf.offloads &= ~rx_offload_flags;
    else
        RTE_LOG(WARNING, USER1,
                "[%hu] VLAN inserting is not supported (per-queue)\n",
                port_config->port_id);
#endif

    int ret;
    for (uint16_t queue_id = 0; queue_id < port_config->rx_queue_count; ++queue_id)
        if (!!(ret = rte_eth_rx_queue_setup(port_config->port_id,
                                            queue_id,
                                            port_config->rx_queue_size,
                                            port_config->socket_id,
                                            &rx_conf,
                                            mbuf_pool)))
        {
            RTE_LOG(ERR, USER1,
                    "[%hu:%hu] rte_eth_rx_queue_setup() failed: %s\n",
                    port_config->port_id, queue_id, rte_strerror(-ret));
            return false;
        }

    return true;
}

/**
 * \brief Настроить очереди передачи пакетов
 * \details Выделяет память и выполняет настройку очередей исходящих
 * пакетов в соответствии с переданной конфигурацией сетевого порта
 * \param[in] port_config Конфигурация сетевого порта
 * \param[in] tx_offload_flags Флаги разгрузки TX
 * \param[in] dev_info Информация об устройстве Ethernet
 * \param[in] eth_conf Конфигурация порта Ethernet
 * \return Результат (успешность) выполнения операции
 */
static inline
bool setUpTxQueues(PortConfigConstPtr port_config,
#ifdef DISABLE_VLAN_INSERTING_PER_QUEUE
                   uint64_t tx_offload_flags,
#endif
                   const struct rte_eth_dev_info* dev_info,
                   const struct rte_eth_conf* eth_conf)
{
#ifdef THRESHOLDS_OPTIMIZATION
    struct rte_eth_txconf tx_conf;
    configureTxThresholds(&tx_conf,
                          &dev_info->default_txconf,
                          port_config->tx_queue_size,
                          port_config->port_id);
#else
    struct rte_eth_txconf tx_conf = dev_info->default_txconf;
#endif

    tx_conf.offloads = eth_conf->txmode.offloads;

#ifdef DISABLE_VLAN_INSERTING_PER_QUEUE
    if (dev_info->tx_queue_offload_capa & tx_offload_flags)
        tx_conf.offloads &= ~tx_offload_flags;
    else
        RTE_LOG(WARNING, USER1,
                "[%hu] VLAN inserting is not supported (per-queue)\n",
                port_config->port_id);
#endif

    int ret;
    for (uint16_t queue_id = 0; queue_id < port_config->tx_queue_count; ++queue_id)
        if (!!(ret = rte_eth_tx_queue_setup(port_config->port_id,
                                            queue_id,
                                            port_config->tx_queue_size,
                                            port_config->socket_id,
                                            &tx_conf)))
        {
            RTE_LOG(ERR, USER1,
                    "[%hu:%hu] rte_eth_tx_queue_setup() failed: %s\n",
                    port_config->port_id, queue_id, rte_strerror(-ret));
            return false;
        }

    return true;
}

/**
 * \brief Настроить сетевой порт
 * \details Выполняет инициализацию сетевого порта. Задаёт количество очередей
 * на приёма и отправку пакетов, их размер. В зависимости от макросов, пороговые
 * значения очередей исходящих пакетов, запреты на вырезание/вставку заголовков VLAN
 * на уровне порта/очереди (если при этом определено, что данный функционал не
 * поддерживается, то инициализация считается выполненной успешно, а в лог будет
 * добавлено предупреждение)
 * \param[in,out] port_config Конфигурация сетевого порта
 * \param[in] mbuf_pool Пул памяти для получаемых и отправляемых пакетов
 * \return Результат (успешность) выполнения операции
 */
static inline
bool configurePort(PortConfigPtr port_config, struct rte_mempool* mbuf_pool)
{
    assert(!!port_config && !!mbuf_pool);

    int ret = rte_eth_dev_is_valid_port(port_config->port_id);
    if (!ret)
    {
        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_dev_is_valid_port() failed: %s\n",
                port_config->port_id, rte_strerror(ret));
        return false;
    }

    struct rte_eth_dev_info dev_info;
    if (!!(ret = rte_eth_dev_info_get(port_config->port_id, &dev_info)))
    {
        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_dev_info_get() failed: %s\n",
                port_config->port_id, rte_strerror(-ret));
        return false;
    }

    struct rte_eth_conf eth_conf;
    memset(&eth_conf, 0, sizeof(eth_conf));

#if defined(DISABLE_VLAN_STRIPPING_PER_PORT) \
 || defined(DISABLE_VLAN_STRIPPING_PER_QUEUE)
    const uint64_t rx_offload_flags = RTE_ETH_RX_OFFLOAD_VLAN_STRIP |
                                      RTE_ETH_RX_OFFLOAD_QINQ_STRIP;
#endif

#ifdef DISABLE_VLAN_STRIPPING_PER_PORT
    if (dev_info.rx_offload_capa & rx_offload_flags)
        eth_conf.rxmode.offloads &= ~rx_offload_flags;
    else
        RTE_LOG(WARNING, USER1,
                "[%hu] VLAN stripping is not supported (per-port)\n",
                port_config->port_id);
#endif

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        eth_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    else
        RTE_LOG(WARNING, USER1,
                "[%hu] Optimization for fast release of mbufs is not supported\n",
                port_config->port_id);

#if defined(DISABLE_VLAN_INSERTING_PER_PORT) \
 || defined(DISABLE_VLAN_INSERTING_PER_QUEUE)
    const uint64_t tx_offload_flags = RTE_ETH_TX_OFFLOAD_VLAN_INSERT |
                                      RTE_ETH_TX_OFFLOAD_QINQ_INSERT;
#endif

#ifdef DISABLE_VLAN_INSERTING_PER_PORT
    if (dev_info.tx_offload_capa & tx_offload_flags)
        eth_conf.txmode.offloads &= ~tx_offload_flags;
    else
        RTE_LOG(WARNING, USER1,
                "[%hu] VLAN inserting is not supported (per-port)\n",
                port_config->port_id);
#endif

    adjustQueueCount(port_config, &dev_info);

    if (!!(ret = rte_eth_dev_configure(port_config->port_id,
                                       port_config->rx_queue_count,
                                       port_config->tx_queue_count,
                                       &eth_conf)))
    {
        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_dev_configure() failed: %s\n",
                port_config->port_id, rte_strerror(-ret));
        return false;
    }

    port_config->socket_id = rte_eth_dev_socket_id(port_config->port_id);
    if (port_config->socket_id == SOCKET_ID_ANY && rte_errno == EINVAL)
    {
        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_dev_socket_id() failed: %s\n",
                port_config->port_id, rte_strerror(rte_errno));
        return false;
    }

    if (!adjustQueueSize(port_config))
    {
        RTE_LOG(WARNING, USER1,
                "[%hu] Failed to adjust RT/TX queue size\n",
                port_config->port_id);
        return false;
    }

    if (!setUpRxQueues(port_config,
#ifdef DISABLE_VLAN_STRIPPING_PER_QUEUE
                       rx_offload_flags,
#endif
                       &dev_info,
                       &eth_conf)
        ||
        !setUpTxQueues(port_config,
#ifdef DISABLE_VLAN_INSERTING_PER_QUEUE
                       tx_offload_flags,
#endif
                       &dev_info,
                       &eth_conf))
    {
        RTE_LOG(ERR, USER1,
                "[%hu] Failed to set up RT/TX queues\n",
                port_config->port_id);
        return false;
    }

    logMAC(port_config->port_id);

    return true;
}

/**
 * \brief "Поднять" сетевой порт
 * \details Запуск приёма и передачи пакетов. Если "неразборчивый" режим режим
 * не поддерживается, то это является допустимым и операция считается выполненной
 * успешно, однако сообщение о неудачной попытке изменения настроек будет добавлено
 * в лог (предупреждение, не ошибка)
 * \param[in] port_config Конфигурация сетевого порта
 * \param[in] promiscuous_mode "Неразборчивый" режим
 * \return Результат (успешность) выполнения операции
 */
static inline
bool bringUpPort(PortConfigConstPtr port_config, bool promiscuous_mode)
{
    int ret = rte_eth_dev_start(port_config->port_id);
    if (!!ret)
    {
        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_dev_start() failed: %s\n",
                port_config->port_id, rte_strerror(-ret));
        return false;
    }

    if (!promiscuous_mode)
        return true;

    if (!!(ret = rte_eth_promiscuous_enable(port_config->port_id)))
    {
        if (ret == -ENOTSUP)
        {
            RTE_LOG(WARNING, USER1,
                    "[%hu] Promiscuous mode is not supported\n",
                    port_config->port_id);
            return true;
        }

        RTE_LOG(ERR, USER1,
                "[%hu] rte_eth_promiscuous_enable() failed: %s\n",
                port_config->port_id, rte_strerror(-ret));
        return false;
    }

    return true;
}

void startAllDevices(PortConfigs port_configs, uint16_t req_rx_queue_count)
{
    if (!port_configs)
        rte_exit(EXIT_FAILURE,
                 "[%s] Internal error: no configuration(s)\n",
                 __func__);

    if (!!mbuf_pool)
        rte_exit(EXIT_FAILURE, "Internal error: memory pool already exists\n");

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
                                        NUM_MBUFS,
                                        MBUF_CACHE_SIZE,
                                        0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE,
                                        rte_socket_id());
    if (!mbuf_pool)
        rte_panic("Failed to create memory pool: %s\n",
                  rte_strerror(rte_errno));

    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id)
    {
        PortConfigPtr port_config = &port_configs[port_id];
        port_config->port_id = port_id;
        port_config->socket_id = SOCKET_ID_ANY;
        port_config->rx_queue_size = RX_QUEUE_SIZE;
        port_config->tx_queue_size = TX_QUEUE_SIZE;
        port_config->rx_queue_count = req_rx_queue_count;
        port_config->tx_queue_count = req_rx_queue_count;

        if (!configurePort(port_config, mbuf_pool))
            rte_panic("Failed to configure port %hu\n",
                      port_config->port_id);
        if (!bringUpPort(port_config, true))
            rte_panic("Failed to bring up port %hu\n",
                     port_config->port_id);
    }
}

void stopAllDevices()
{
    int ret;
    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id)
    {
        if (!!(ret = rte_eth_dev_stop(port_id)))
            RTE_LOG(ERR, USER1,
                    "rte_eth_dev_stop() failed: %s\n",
                    rte_strerror(-ret));
        if (!!(ret = rte_eth_dev_close(port_id)))
            RTE_LOG(ERR, USER1,
                    "rte_eth_dev_close() failed: %s\n",
                    rte_strerror(-ret));;
    }

    if (!!mbuf_pool)
    {
        rte_mempool_free(mbuf_pool);
        mbuf_pool = NULL;
    }
}
