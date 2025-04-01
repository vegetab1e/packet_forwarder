#ifndef PACKET_FORWARDER_H
#define PACKET_FORWARDER_H

/**
 * \brief Запустить форвардер
 * \details Пересылает пакеты в соответсвии с конфигурацией, задаваемой
 * аргументами командной строки запуска приложения, подробнее в README
 * \param[in] argc Количество переданных при запуске приложения аргументов
 * \param[in] argv Массив переданных при запуске приложения аргументов
 */
void startForwarder(int argc, char** argv);

#endif // PACKET_FORWARDER_H
