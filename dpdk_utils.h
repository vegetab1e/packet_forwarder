#ifndef DPDK_UTILS_H
#define DPDK_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "types.h"

struct rte_mbuf;
struct rte_eth_dev_tx_buffer;

/**
 * @brief Создать буфер для исходящий пакетов
 * Выделяет память в куче под буфер, инициализирует его
 * и назначает обработчик ошибок отправки пакетов
 * @param[in] buffer_size Размер буфера в пакетах
 * @param[in] error_handler Обработчик ошибок отправки пакетов
 * @param[in] port_id Номер сетевого порта для отправки пакетов
 * @return Указатель на созданный буфер или NULL
 */
struct rte_eth_dev_tx_buffer*
createTxPacketBuffer(size_t buffer_size,
                     uint16_t tx_port_id,
                     ResendPacketsCallback error_handler);

/**
 * @brief Высвободить ресурсы (память) буферов исходящих пакетов
 * @param[in] tx_packet_buffers Массив буферов
 * @param[in] buffer_count Количество буферов
 */
void freeTxPacketBuffers(struct rte_eth_dev_tx_buffer** tx_packet_buffers,
                         uint16_t buffer_count);

/**
 * @brief Выгрузить данные и высвободить ресурсы (память) пакета
 * Сбросить информацию о пакете (без содержимого) в файл
 * и вернуть используемую им память обратно в пул
 * @param[in] packets Массив пакетов
 * @param[in] packet_count Количество пакетов
 */
void dumpAndFreePackets(struct rte_mbuf** packets, uint16_t packet_count);

#endif // DPDK_UTILS_H
