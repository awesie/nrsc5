#include <getopt.h>
#include <nrsc5.h>

#include <stdio.h>

#include "log.h"

static void help(const char *progname)
{
    fprintf(stderr, "Usage: %s [-v] [-q] [-l log-level] [-d device-args]\n", progname);
}

int main(int argc, char *argv[])
{
    const char *device_args = NULL, *name;
    float freq;
    int opt;
    nrsc5_t *radio = NULL;

    while ((opt = getopt(argc, argv, "r:w:o:d:g:ql:v")) != -1)
    {
        switch (opt)
        {
        case 'd':
            device_args = optarg;
            break;
        case 'q':
            log_set_quiet(1);
            break;
        case 'l':
            log_set_level(atoi(optarg));
            break;
        case 'v':
            printf("nrsc5 revision %s\n", GIT_COMMIT_HASH);
            return 1;
        default:
            help(argv[0]);
            return 1;
        }
    }

    if (optind != argc)
    {
        help(argv[0]);
        return 1;
    }

    if (nrsc5_open(&radio, device_args) != 0)
    {
        log_fatal("Open device failed.");
        return 1;
    }

    freq = NRSC5_SCAN_BEGIN;
    while (nrsc5_scan(radio, freq, NRSC5_SCAN_END, NRSC5_SCAN_SKIP, &freq, &name) == 0)
    {
        printf("%.0f\t%s\n", freq, name);
        freq += NRSC5_SCAN_SKIP;
    }

    return 0;
}
