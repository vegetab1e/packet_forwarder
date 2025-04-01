#include "dpdk_port.h"

#include "config.h"

#include <memory.h>
#include <assert.h>

#include <rte_errno.h>
#include <rte_ethdev.h>

#define MIN(a,b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
       _a < _b ? _a : _b; })

#ifdef THRESHOLDS_OPTIMIZATION
#define PREF_TX_FREE_THRESH 32
#define PREF_TX_RS_THRESH 32

#define PREF_TX_PTHRESH 36
#define PREF_TX_HTHRESH 0
#define PREF_TX_WTHRESH 0

/**
 * @brief Проверить пороговые значения очередей отправки для порта
 * Выполняет проверку на основве рекомендаций, взятых из документации
 * Intel и на тематических ресурсах. Способ неоднозначный, использовался
 * при отладке и оставлен для этих же целей
 * @param[in] def_tx_conf Значения порогов по умолчанию
 * @param[in] tx_desc_count Размер очереди в дескрипторах
 * @return Результат (успешность) выполнения проверки
 */
static inline
bool checkTxThresholds(const struct rte_eth_txconf* tx_conf,
                       uint16_t tx_desc_count)
{
    if (!(tx_conf->tx_thresh.wthresh == 0 && tx_conf->tx_rs_thresh > 1))
    {
        RTE_LOG(INFO, USER1,
                "TX wthresh (%u) should be set to 0 when "
                "tx_rs_thresh (%u) is greater than 1\n",
                tx_conf->tx_thresh.wthresh, tx_conf->tx_rs_thresh);
        return false;
    }
    if (!(tx_conf->tx_rs_thresh > 0 || tx_conf->tx_free_thresh > 0))
    {
        RTE_LOG(INFO, USER1,
                "x_rs_thresh (%u) must be greater than 0 and "
                "x_free_thresh (%u) must be greater than 0\n",
                tx_conf->tx_rs_thresh, tx_conf->tx_free_thresh);
        return false;
    }

    if (tx_conf->tx_rs_thresh >= (tx_desc_count - 2))
    {
        RTE_LOG(INFO, USER1,
                "tx_rs_thresh (%u) must be less than the "
                "number of TX descriptors (%u) minus 2\n",
                tx_conf->tx_rs_thresh, tx_desc_count);
        return false;
    }
    if (tx_conf->tx_free_thresh >= (tx_desc_count - 3))
    {
        RTE_LOG(INFO, USER1,
                "tx_free_thresh (%u) must be less than the "
                "number of TX descriptors (%u) minus 3\n",
                tx_conf->tx_free_thresh, tx_desc_count);
        return false;
    }
    if (tx_conf->tx_rs_thresh > tx_conf->tx_free_thresh)
    {
        RTE_LOG(INFO, USER1,
                "tx_rs_thresh (%u) must be less than or "
                "equal to tx_free_thresh (%u)\n",
                tx_conf->tx_rs_thresh, tx_conf->tx_free_thresh);
        return false;
    }

    if ((tx_desc_count % tx_conf->tx_rs_thresh) != 0)
    {
        RTE_LOG(INFO, USER1,
                "tx_rs_thresh (%u) must be a divisor of the "
                "number of TX descriptors (%u)\n",
                tx_conf->tx_rs_thresh, tx_desc_count);
        return false;
    }

    return true;
}

/**
 * @brief Настроить пороговые значения очередей отправки для порта
 * Попытка оптимизации пороговов, значения взяты из документации Intel
 * и на тематических ресурсах. Едва ли полезная, но вряд ли вредная.
 * Bспользовалfсm при отладке и оставленf для этих же целей.
 * Полученные значения проверяются (способ неоднозначный) и в случае
 * неудовлетворения критериям возвращаются значения по умолчанию
 * @param[in] def_tx_conf Значения порогов по умолчанию
 * @param[in] tx_desc_count Размер очереди в дескрипторах
 * @return Полученные значения или значения по умолчанию
 */
static inline
struct rte_eth_txconf
configureTxThresholds(const struct rte_eth_txconf* def_tx_conf,
                      uint16_t tx_desc_count)
{
    const struct rte_eth_txconf tx_conf = {
        {
            .pthresh = def_tx_conf->tx_thresh.pthresh ? def_tx_conf->tx_thresh.pthresh : PREF_TX_PTHRESH,
            .hthresh = def_tx_conf->tx_thresh.hthresh ? def_tx_conf->tx_thresh.hthresh : PREF_TX_HTHRESH,
            .wthresh = def_tx_conf->tx_thresh.wthresh ? def_tx_conf->tx_thresh.wthresh : PREF_TX_WTHRESH,
        },
        .tx_rs_thresh   = def_tx_conf->tx_rs_thresh   ? def_tx_conf->tx_rs_thresh   : PREF_TX_RS_THRESH,
        .tx_free_thresh = def_tx_conf->tx_free_thresh ? def_tx_conf->tx_free_thresh : PREF_TX_FREE_THRESH
    };

    if (!checkTxThresholds(&tx_conf, tx_desc_count))
    {
        RTE_LOG(WARNING, USER1, "Thresholds configuration is not preferred\n");
        return *def_tx_conf;
    }

    return tx_conf;
}
#endif

#ifdef DISABLE_VLAN_STRIPPING
/**
 * @brief Запретить удаление заголовков VLAN (внешней и внутренней сети) из кадров Ethernet
 * На уровне сетевого порта. Если данный функционал не поддерживается оборудованием,
 * то операция считается выполненной успешно, так как заголовки VLAN из кадров Ethernet
 * вырезаться не будут, что и требовалось получить в результате, тем не менее сообщение
 * о неудачной попытке изменения настроек будет добавлено в лог (предупреждение, не ошибка)
 * @param[in] port_id Номер сетевого порта
 * @return Результат (успешность) выполнения операции
 */
static inline
bool disableVlanStripping(uint16_t port_id)
{
    int ret = rte_eth_dev_get_vlan_offload(port_id);
    if (ret < 0)
    {
        RTE_LOG(ERR, USER1, "rte_eth_dev_get_vlan_offload() failed: %s\n", rte_strerror(-ret));
        return false;
    }

    const int vlan_offload_flags = (ret & ~(RTE_ETH_VLAN_STRIP_OFFLOAD | RTE_ETH_QINQ_STRIP_OFFLOAD));
    if (!!(ret = rte_eth_dev_set_vlan_offload(port_id, vlan_offload_flags)))
    {
        if (ret == -ENOTSUP)
        {
            RTE_LOG(WARNING, USER1, "VLAN stripping is not supported (per-port)\n");
            return true;
        }

        RTE_LOG(ERR, USER1, "rte_eth_dev_set_vlan_offload() failed: %s\n", rte_strerror(-ret));
        return false;
    }

    return true;
}
#endif

static inline
void adjustQueueCount(const struct rte_eth_dev_info* dev_info, PortConfigPtr port_config)
{
    port_config->rx_queue_count = MIN(port_config->rx_queue_count, dev_info->max_rx_queues);
    port_config->tx_queue_count = MIN(port_config->tx_queue_count, dev_info->max_tx_queues);
    port_config->rx_queue_count = MIN(port_config->rx_queue_count, port_config->tx_queue_count);

    RTE_LOG(INFO, USER1,
            "RX/TX queue count: %hu/%hu\n",
            port_config->rx_queue_count, port_config->tx_queue_count);
}

bool configurePort(PortConfigPtr port_config, struct rte_mempool* mbuf_pool)
{
    assert(!!mbuf_pool);

    int ret = rte_eth_dev_is_valid_port(port_config->port_id);
    if (!ret)
    {
        RTE_LOG(ERR, USER1, "rte_eth_dev_is_valid_port() failed: %s\n", rte_strerror(ret));
        return false;
    }

    struct rte_eth_dev_info dev_info;
    if (!!(ret = rte_eth_dev_info_get(port_config->port_id, &dev_info)))
    {
        RTE_LOG(ERR, USER1, "rte_eth_dev_info_get() failed: %s\n", rte_strerror(-ret));
        return false;
    }

    struct rte_eth_conf eth_conf;
    memset(&eth_conf, 0, sizeof(eth_conf));
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        eth_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

#ifdef DISABLE_VLAN_INSERTING
    const uint64_t vlan_offload_flags = RTE_ETH_TX_OFFLOAD_VLAN_INSERT | RTE_ETH_TX_OFFLOAD_QINQ_INSERT;
    if (dev_info.tx_offload_capa & vlan_offload_flags)
        eth_conf.txmode.offloads &= ~vlan_offload_flags;
    else
        RTE_LOG(WARNING, USER1, "VLAN inserting not supported (per-port)\n");
#endif

    adjustQueueCount(&dev_info, port_config);

    if (!!(ret = rte_eth_dev_configure(port_config->port_id,
                                       port_config->rx_queue_count,
                                       port_config->tx_queue_count,
                                       &eth_conf)))
    {
        RTE_LOG(ERR, USER1, "rte_eth_dev_configure() failed: %s\n", rte_strerror(-ret));
        return false;
    }

#ifdef DISABLE_VLAN_STRIPPING
    if (!disableVlanStripping(port_config->port_id))
    {
        RTE_LOG(WARNING, USER1, "Failed to disable VLAN stripping on port %hu\n", port_config->port_id);
        return false;
    }
#endif

    if (!!(ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_config->port_id,
                                                  &port_config->rx_desc_count,
                                                  &port_config->tx_desc_count)))
    {
        RTE_LOG(ERR, USER1, "rte_eth_dev_adjust_nb_rx_tx_desc() failed: %s\n", rte_strerror(-ret));
        return false;
    }

    RTE_LOG(INFO, USER1,
            "RX/TX queue size: %hu/%hu\n",
            port_config->rx_desc_count, port_config->tx_desc_count);

    port_config->socket_id = rte_eth_dev_socket_id(port_config->port_id);
    if (port_config->socket_id == SOCKET_ID_ANY && rte_errno == EINVAL)
    {
        RTE_LOG(ERR, USER1, "rte_eth_dev_socket_id() failed: %s\n", rte_strerror(rte_errno));
        return false;
    }

    for (uint16_t queue_id = 0; queue_id < port_config->rx_queue_count; ++queue_id)
    {
        if (!!(ret = rte_eth_rx_queue_setup(port_config->port_id,
                                            queue_id,
                                            port_config->rx_desc_count,
                                            port_config->socket_id,
                                            NULL,
                                            mbuf_pool)))
        {
            RTE_LOG(ERR, USER1, "rte_eth_rx_queue_setup() failed: %s\n", rte_strerror(-ret));
            return false;
        }
#ifdef DISABLE_VLAN_STRIPPING
        if (!!(ret = rte_eth_dev_set_vlan_strip_on_queue(port_config->port_id, queue_id, 0)))
        {
            if (ret != -ENOTSUP)
            {
                RTE_LOG(ERR, USER1, "rte_eth_dev_set_vlan_strip_on_queue() failed: %s\n", rte_strerror(-ret));
                return false;
            }

            RTE_LOG(WARNING, USER1, "VLAN stripping is not supported (per-queue)\n");
        }
#endif
    }

#ifdef THRESHOLDS_OPTIMIZATION
    struct rte_eth_txconf tx_conf = configureTxThresholds(&dev_info.default_txconf, port_config->tx_desc_count);
#else
    struct rte_eth_txconf tx_conf = dev_info.default_txconf;
#endif

    tx_conf.offloads = eth_conf.txmode.offloads;

#ifdef DISABLE_VLAN_INSERTING
    if ((dev_info.tx_queue_offload_capa | dev_info.tx_offload_capa) & vlan_offload_flags)
        tx_conf.offloads &= ~vlan_offload_flags;
    else
        RTE_LOG(WARNING, USER1, "VLAN inserting not supported (per-queue)\n");
#endif

    for (uint16_t queue_id = 0; queue_id < port_config->tx_queue_count; ++queue_id)
        if (!!(ret = rte_eth_tx_queue_setup(port_config->port_id,
                                            queue_id,
                                            port_config->tx_desc_count,
                                            port_config->socket_id,
                                            &tx_conf)))
        {
            RTE_LOG(ERR, USER1, "rte_eth_tx_queue_setup() failed: %s\n", rte_strerror(-ret));
            return false;
        }

    struct rte_ether_addr mac_addr;
    if (!!(ret = rte_eth_macaddr_get(port_config->port_id, &mac_addr)))
    {
        RTE_LOG(ERR, USER1, "rte_eth_macaddr_get() failed: %s\n", rte_strerror(-ret));
        return false;
    }

    RTE_LOG(INFO, USER1, "Port %u MAC: " RTE_ETHER_ADDR_PRT_FMT "\n",
            port_config->port_id, RTE_ETHER_ADDR_BYTES(&mac_addr));

    return true;
}

bool bringUpPort(const PortConfigPtr port_config, bool promiscuous_mode)
{
    int ret = rte_eth_dev_start(port_config->port_id);
    if (!!ret)
    {
        RTE_LOG(ERR, USER1, "rte_eth_dev_start() failed: %s\n", rte_strerror(-ret));
        return false;
    }

    if (!promiscuous_mode)
        return true;

    if (!!(ret = rte_eth_promiscuous_enable(port_config->port_id)))
    {
        if (ret == -ENOTSUP)
        {
            RTE_LOG(WARNING, USER1, "Promiscuous mode is not supported\n");
            return true;
        }

        RTE_LOG(ERR, USER1, "rte_eth_promiscuous_enable() failed: %s\n", rte_strerror(-ret));
        return false;
    }

    return true;
}
