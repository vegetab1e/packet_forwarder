#include "dpdk_utils.h"
#include "utils.h"

#include <rte_malloc.h>
#include <rte_ethdev.h>

struct rte_eth_dev_tx_buffer*
createTxPacketBuffer(size_t buffer_size,
                     uint16_t tx_port_id,
                     ResendPacketsCallback error_handler)
{
    struct rte_eth_dev_tx_buffer* tx_packet_buffer = rte_zmalloc_socket("tx_buffer",
                                                                        RTE_ETH_TX_BUFFER_SIZE(buffer_size), 0,
                                                                        rte_eth_dev_socket_id(tx_port_id));
    if (!tx_packet_buffer)
    {
        RTE_LOG(ERR, USER1, "Failed to allocate memory: %s\n", rte_strerror(rte_errno));
        return NULL;
    }

    int ret;
    if (!!(ret = rte_eth_tx_buffer_init(tx_packet_buffer, buffer_size)))
    {
        RTE_LOG(ERR, USER1, "Failed to initialize buffer: %s\n", rte_strerror(-ret));
        return NULL;
    }

    if (!!(ret = rte_eth_tx_buffer_set_err_callback(tx_packet_buffer,
                                                    error_handler,
                                                    NULL)))
    {
        RTE_LOG(ERR, USER1, "Failed to set callback: %s\n", rte_strerror(-ret));
        return NULL;
    }

    return tx_packet_buffer;
}

void freeTxPacketBuffers(struct rte_eth_dev_tx_buffer** tx_packet_buffers,
                         uint16_t buffer_count)
{
    for (size_t i = 0; i < buffer_count; ++i)
        if (!!tx_packet_buffers[i])
        {
            rte_free(tx_packet_buffers[i]);
            tx_packet_buffers[i] = NULL;
        }
}

void dumpAndFreePackets(struct rte_mbuf** packets, uint16_t packet_count)
{
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
