#include "latency/core.hpp"
#include "latency/monitoring.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;

struct Config {
    std::filesystem::path runDirectory;
    std::chrono::milliseconds monitoringPeriod{10s};
};

Config parseArgs(int argc, char **argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            std::cout << "Usage: latency_analyzer [options]\n"
                         "  --run-directory PATH     benchmark directory (required)\n"
                         "  --monitoring-period 10s  QD monitoring interval\n";
            std::exit(0);
        }

        if (i + 1 >= argc) {
            throw std::invalid_argument("missing value for " + arg);
        }

        const std::string value = argv[++i];

        if (arg == "--run-directory") {
            config.runDirectory = value;
        } else if (arg == "--monitoring-period") {
            auto duration = latency::parseDuration(value);

            if (!duration) {
                throw std::invalid_argument(duration.error());
            }

            config.monitoringPeriod = *duration;
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    if (config.runDirectory.empty()) {
        throw std::invalid_argument("--run-directory is required");
    }

    return config;
}
} // namespace

int main(int argc, char **argv) {
    try {
        const auto config = parseArgs(argc, argv);
        auto analysis = latency::analyzeMonitoringDirectory(config.runDirectory, config.monitoringPeriod);

        if (!analysis) {
            throw std::runtime_error(analysis.error());
        }

        auto written = latency::writeMonitoringAnalysis(config.runDirectory, *analysis);

        if (!written) {
            throw std::runtime_error(written.error());
        }

        std::cout << "Wrote " << (config.runDirectory / "monitoring.csv").string() << '\n'
                  << "Wrote " << (config.runDirectory / "monitoring-summary.csv").string() << '\n';

        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
