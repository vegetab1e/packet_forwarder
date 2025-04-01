#include <assert.h>

#include <rte_malloc.h>
#include <rte_ethdev.h>

#include "dpdk_utils.h"

#include "utils.h"

bool createTxPacketBuffer(LCoreConfigPtr lcore_config,
                          size_t buffer_size,
                          ResendPacketsCallback error_handler)
{
    if (!lcore_config)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%u] Internal error: no configuration\n",
                __func__,
                rte_lcore_id());
        return false;
    }

    assert(rte_get_main_lcore() == rte_lcore_id());

    lcore_config->tx_packet_buffer = rte_zmalloc_socket("tx_buffer",
                                                        RTE_ETH_TX_BUFFER_SIZE(buffer_size), 0,
                                                        rte_eth_dev_socket_id(lcore_config->tx_port_id));
    if (!lcore_config->tx_packet_buffer)
    {
        RTE_LOG(ERR, USER1,
                "[%u] Failed to allocate memory: %s\n",
                lcore_config->lcore_id, rte_strerror(rte_errno));
        return false;
    }

    int ret;
    if (!!(ret = rte_eth_tx_buffer_init(lcore_config->tx_packet_buffer, buffer_size)))
    {
        RTE_LOG(ERR, USER1,
                "[%u] Failed to initialize buffer: %s\n",
                lcore_config->lcore_id, rte_strerror(-ret));

        rte_free(lcore_config->tx_packet_buffer);
        lcore_config->tx_packet_buffer = NULL;

        return false;
    }

    if (!!(ret = rte_eth_tx_buffer_set_err_callback(lcore_config->tx_packet_buffer,
                                                    !!error_handler ? (buffer_tx_error_fn)error_handler
                                                                    : rte_eth_tx_buffer_drop_callback,
                                                    lcore_config)))
    {
        RTE_LOG(ERR, USER1,
                "[%u] Failed to set callback: %s\n",
                lcore_config->lcore_id, rte_strerror(-ret));

        rte_free(lcore_config->tx_packet_buffer);
        lcore_config->tx_packet_buffer = NULL;

        return false;
    }

    return true;
}

void freeTxPacketBuffer(LCoreConfigPtr lcore_config)
{
    if (!lcore_config)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%u] Internal error: no configuration\n",
                __func__,
                rte_lcore_id());
        return;
    }

    if (!!lcore_config->tx_packet_buffer)
    {
        rte_free(lcore_config->tx_packet_buffer);
        lcore_config->tx_packet_buffer = NULL;
    }
}

void dumpAndFreePackets(struct rte_mbuf** packets, uint16_t packet_count)
{
    if (!packets)
    {
        RTE_LOG(ERR, USER1,
                "[%s][%u] Internal error: no packets\n",
                __func__,
                rte_lcore_id());
        return;
    }

    FILE* dump = openDump();
    if (!!dump)
    {
        for (uint16_t packet_number = 0; packet_number < packet_count; ++packet_number)
        {
            rte_pktmbuf_dump(dump, packets[packet_number], 0);
            rte_pktmbuf_free(packets[packet_number]);
        }

        fclose(dump);
    }
    else
        rte_pktmbuf_free_bulk(packets, packet_count);
}
