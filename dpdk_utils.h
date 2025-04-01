#ifndef DPDK_UTILS_H
#define DPDK_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "types.h"

struct rte_mbuf;

/**
 * \brief Создать буфер для исходящий пакетов
 * \details Выделяет память в куче под буфер, инициализирует его
 * и назначает обработчик ошибок отправки пакетов. Если указатель на
 * функцию обратного вызова NULL, то будет назначен обработчик по
 * умолчанию DPDK, который просто отбрасывает пакеты
 * \param[in] lcore_config Конфигурация логического ядра
 * \param[in] buffer_size Размер буфера в пакетах
 * \param[in] error_handler Обработчик ошибок отправки пакетов
 * \return Результат (успешность) выполнения операции
 */
bool createTxPacketBuffer(LCoreConfigPtr lcore_config,
                          size_t buffer_size,
                          ResendPacketsCallback error_handler);

/**
 * \brief Высвободить ресурсы (память) буфера исходящих пакетов
 * \param[in] lcore_config Конфигурация логического ядра
 */
void freeTxPacketBuffer(LCoreConfigPtr lcore_config);

/**
 * \brief Выгрузить данные и высвободить ресурсы (память) пакета
 * \details Сбросить информацию о пакете (без содержимого) в файл
 * и вернуть используемую им память обратно в пул
 * \param[in] packets Массив пакетов
 * \param[in] packet_count Количество пакетов
 */
void dumpAndFreePackets(struct rte_mbuf** packets, uint16_t packet_count);

#endif // DPDK_UTILS_H
