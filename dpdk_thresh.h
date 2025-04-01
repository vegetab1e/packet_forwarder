#ifndef DPDK_THRESH_H
#define DPDK_THRESH_H

#include <stdint.h>
#include <stdbool.h>

struct rte_eth_txconf;

/**
 * \brief Настроить пороговые значения очередей отправки
 * \details Попытка оптимизации порогов, значения взяты из документации Intel и
 * на тематических ресурсах. Едва ли полезная, но вряд ли вредная. Использовалась
 * при отладке, оставлена для этих же целей. Полученные значения проверяются
 * (способ также неоднозначный) и в случае неудовлетворения критериям возвращаются
 * значения, переданные в функцию при запуске, без каких-либо изменений. Результат
 * (удалось/неудалось) также будет выведен в лог (на уровне INFO/WARNING)
 * \param[out] Указатель для сохранения полученных значений порогов
 * \param[in] def_tx_conf Значения порогов по умолчанию
 * \param[in] tx_desc_count Размер очереди в дескрипторах
 * \param[in] port_id Номер порта (для лога)
 * \return Результат (успешность) выполнения настройки
 */
bool configureTxThresholds(struct rte_eth_txconf* tx_conf,
                           const struct rte_eth_txconf* def_tx_conf,
                           uint16_t tx_desc_count,
                           uint16_t port_id);

#endif // DPDK_THRESH_H
