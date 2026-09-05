// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/runner.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

/// Prints command-line usage for the native benchmark runner.
void printUsage() {
    std::cout << R"(Usage: latency_runner --binary-directory <path> [options]

Options:
  --output-root <path>         Result root (default: benchmark-results)
  --config <path>              Suite configuration file
  --client-role <role>         Override feed or stream-feed
  --listener-delay <duration>  Override artificial listener delay
  --events-batch-limit <value> Override dxFeed event notification batch limit
  --dry-run                    Print the execution plan without running it
  --help                       Show this help
)";
}

/// Reads the value following an option and advances the argument index.
std::optional<std::string> readValue(std::span<char *> arguments, std::size_t &index, std::string_view option) {
    if (++index >= arguments.size()) {
        std::cerr << "Missing value for " << option << '\n';

        return std::nullopt;
    }

    return arguments[index];
}

} // namespace

/** Runs the native benchmark-suite orchestrator. */
int main(int argc, char **argv) {
    const std::span arguments{argv, static_cast<std::size_t>(argc)};
    std::optional<std::filesystem::path> binaryDirectory;
    std::filesystem::path outputRoot{"benchmark-results"};
    std::filesystem::path config{std::filesystem::path{argv[0]}.parent_path() / "benchmark-suite.conf"};
    latency::BenchmarkOverrides overrides;
    bool dryRun{};

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view option{arguments[index]};

        if (option == "--help") {
            printUsage();

            return 0;
        }

        if (option == "--dry-run") {
            dryRun = true;
            continue;
        }

        const auto value = readValue(arguments, index, option);

        if (!value) {
            return 2;
        }

        if (option == "--binary-directory") {
            binaryDirectory = *value;
        } else if (option == "--output-root") {
            outputRoot = *value;
        } else if (option == "--config") {
            config = *value;
        } else if (option == "--client-role") {
            overrides.clientRole = *value;
        } else if (option == "--listener-delay") {
            overrides.listenerDelay = *value;
        } else if (option == "--events-batch-limit") {
            overrides.eventsBatchLimit = *value;
        } else {
            std::cerr << "Unknown argument: " << option << '\n';

            return 2;
        }
    }

    if (!binaryDirectory) {
        std::cerr << "--binary-directory is required\n";

        return 2;
    }

    return latency::runBenchmarkSuite(*binaryDirectory, outputRoot, config, overrides, dryRun);
}
