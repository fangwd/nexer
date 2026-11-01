#include "helper.h"
#include "nexer/core/logger.h"
#include "runner.h"

using namespace nexer::test;

int main(int argc, char **argv) {
    strcpy(exename, argv[0]);
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    default_logger.open("test.log", Logger::Level::DEBUG);
    log_set_level(Logger::Level::DEBUG);

    int failed = 0;
    switch (argc) {
    case 1:
        failed = Run();
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

    remove(NEXER_TEST_DB);

    return failed == 0 ? 0 : 1;
}
