#include <stdbool.h>

#include <rte_ethdev.h>

#include "dpdk_thresh.h"

#define PREF_TX_FREE_THRESH 32
#define PREF_TX_RS_THRESH 32

#define PREF_TX_PTHRESH 32
#define PREF_TX_HTHRESH 0
#define PREF_TX_WTHRESH 0

/**
 * \brief Проверить пороговые значения очередей отправки
 * \details Выполняет проверку на основве рекомендаций, взятых из документации
 * Intel и на тематических ресурсах. Способ неоднозначный, использовался при
 * отладке, оставлен для этих же целей
 * \param[in] def_tx_conf Значения порогов
 * \param[in] tx_desc_count Размер очереди в дескрипторах
 * \return Результат (успешность) выполнения проверки
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

bool configureTxThresholds(struct rte_eth_txconf* tx_conf,
                           const struct rte_eth_txconf* def_tx_conf,
                           uint16_t tx_desc_count,
                           uint16_t port_id)
{
    if (!tx_conf || !def_tx_conf)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%hu] Internal error: no configuration(s)\n",
                __func__, port_id);
        return false;
    }

    memcpy(tx_conf, def_tx_conf, sizeof(struct rte_eth_txconf));

    const struct rte_eth_txconf pref_tx_conf = {
        {
            .pthresh = def_tx_conf->tx_thresh.pthresh ? def_tx_conf->tx_thresh.pthresh : PREF_TX_PTHRESH,
            .hthresh = def_tx_conf->tx_thresh.hthresh ? def_tx_conf->tx_thresh.hthresh : PREF_TX_HTHRESH,
            .wthresh = def_tx_conf->tx_thresh.wthresh ? def_tx_conf->tx_thresh.wthresh : PREF_TX_WTHRESH,
        },
        .tx_rs_thresh   = def_tx_conf->tx_rs_thresh   ? def_tx_conf->tx_rs_thresh   : PREF_TX_RS_THRESH,
        .tx_free_thresh = def_tx_conf->tx_free_thresh ? def_tx_conf->tx_free_thresh : PREF_TX_FREE_THRESH
    };

    if (!checkTxThresholds(&pref_tx_conf, tx_desc_count))
    {
        RTE_LOG(WARNING, USER1,
                "[%hu] Thresholds configuration is not preferred\n",
                port_id);
        return false;
    }

    tx_conf->tx_thresh.pthresh = pref_tx_conf.tx_thresh.pthresh;
    tx_conf->tx_thresh.hthresh = pref_tx_conf.tx_thresh.hthresh;
    tx_conf->tx_thresh.wthresh = pref_tx_conf.tx_thresh.wthresh;

    tx_conf->tx_rs_thresh   = pref_tx_conf.tx_rs_thresh;
    tx_conf->tx_free_thresh = pref_tx_conf.tx_free_thresh;

    RTE_LOG(INFO, USER1,
            "[%hu] Thresholds configuration complete successfully\n",
            port_id);
    return true;
}











