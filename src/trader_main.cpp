#include "strategies/TriangularArb.h"
#include <iostream>
#include <string>
#include <getopt.h>

void printUsage(const std::string& programName) {
    std::cout << "Usage: " << programName << " --config <path_to_ini>" << std::endl;
    std::cout << "       --config, -c : Path to the configuration INI file." << std::endl;
}

int main(int argc, char* argv[]) {
    std::string configFile;

    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "c:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                configFile = optarg;
                break;
            case 'h':
                printUsage(argv[0]);
                return 0;
            case '?':
            default:
                printUsage(argv[0]);
                return 1;
        }
    }

    if (configFile.empty()) {
        std::cerr << "Error: --config parameter is required." << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    try {
        auto config = TriangularArb::loadConfig(configFile);
        TriangularArb strategy(config);
        strategy.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
