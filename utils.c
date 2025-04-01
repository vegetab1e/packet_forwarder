#include <stdlib.h>

#include <time.h>

#include <getopt.h>
#include <limits.h>
#include <errno.h>

#include "utils.h"

FILE* openDump()
{
    char filename[64];
    const time_t timestamp = time(NULL);

    FILE* dump = fopen(!!strftime(filename,
                                  sizeof(filename),
                                  "%d%m%y.dump",
                                  localtime(&timestamp))
                       ? filename : "dump", "a");

    return dump;
}

bool getOption(int argc, char** argv, int in, uint16_t* out)
{
    if (!argv || !out)
    {
        printf("[%s] Internal error: null pointer(s)\n", __func__);
        return false;
    }

    int option;
    unsigned long value;
    while ((option = getopt(argc, argv, "p:q:")) != -1)
    {
        if (option == in)
        {
            errno = 0;
            value = strtoul(optarg, NULL, 10);
            if ((value == 0 || value == ULONG_MAX) && !!errno)
            {
                optind = 1;
                return false;
            }

            *out = (uint16_t)value;

            optind = 1;
            return true;
        }
    }

    optind = 1;
    return false;
}
