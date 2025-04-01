#include <stdbool.h>
#include <signal.h>

#include "packet_forwarder.h"

extern volatile bool is_running;

void signalHandler(int signal_num)
{
    if (signal_num == SIGINT ||
        signal_num == SIGTERM)
        is_running = false;
}

int main(int argc, char** argv)
{
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    startForwarder(argc, argv);

    return 0;
}
