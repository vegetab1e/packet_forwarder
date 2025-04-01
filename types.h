#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

#include <rte_build_config.h>

struct rte_mbuf;

typedef struct rte_eth_dev_tx_buffer* TxPacketBufferPtr;

typedef struct _LCoreConfig
{
    unsigned lcore_id;
    uint16_t rx_port_id;
    uint16_t tx_port_id;
    uint16_t queue_id;

    TxPacketBufferPtr tx_packet_buffer;

    volatile struct _PacketStats
    {
        uint64_t rx_packet_count;
        uint64_t tx_packet_count;
        uint64_t drp_packet_count;
        uint64_t proc_error_count;
#ifndef NDEBUG
        uint64_t rx_ops;
        uint64_t tx_ops;
        uint64_t retx_ops;
#endif
    } *packet_stats;
} LCoreConfig,
  LCoreConfigs[RTE_MAX_LCORE],
 *LCoreConfigPtr;

typedef const LCoreConfig* LCoreConfigConstPtr;

typedef struct _PacketStats PacketStats;
typedef volatile PacketStats* PacketStatsPtr;

typedef struct _PortConfig
{
    uint16_t port_id;
    int socket_id;
    uint16_t rx_queue_size;
    uint16_t tx_queue_size;
    uint16_t rx_queue_count;
    uint16_t tx_queue_count;
} PortConfig,
  PortConfigs[RTE_MAX_ETHPORTS],
 *PortConfigPtr;

typedef const PortConfig* PortConfigConstPtr;

typedef void (*ResendPacketsCallback)(struct rte_mbuf** unsent_packets,
                                      uint16_t unsent_packet_count,
                                      const void* user_data);

#endif // TYPES_H
