/* Copyright (c) Weidong Fang. All rights reserved. */

#include "logger.h"
#include "runner.h"
#include "helper.h"

using namespace nexer::test;

int main(int argc, char **argv) {
    strcpy(exename, argv[0]);
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    log_set_level(Logger::Level::DEBUG);

    switch (argc) {
    case 1:
        Run();
        break;
    case 2:
        Run(argv[1]);
        break;
    default:
        if (strcmp(argv[1], "helper")) {
            log_error("Invalid arguments '%s'", argv[1]);
            return 1;
        }
        return RunHelper(argv[2], argc - 3, argc > 3 ? &argv[3] : nullptr);
    }

    return 0;
}
