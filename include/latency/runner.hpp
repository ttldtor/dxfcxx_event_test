// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <expected>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace latency {

/// Describes the purpose, controlled variables, and interpretation boundary of a benchmark experiment.
struct BenchmarkExperiment {
    /// Human-readable report title.
    std::string title;

    /// Question that the experiment is intended to answer.
    std::string objective;

    /// Independent variable changed between profiles or invocations.
    std::string variable;

    /// Workload and environment properties intentionally kept constant.
    std::string controls;

    /// Measurements used to evaluate the experiment.
    std::string successCriteria;

    /// Conclusions that the experiment cannot establish.
    std::string limitations;
};

/// Describes one workload profile from a benchmark suite configuration.
struct BenchmarkSuiteProfile {
    /// Stable profile name used in output file prefixes.
    std::string name;

    /// Server task expressed in the benchmark task DSL.
    std::string task;

    /// Optional endpoint-role override for this profile.
    std::optional<std::string> clientRole;

    /// Optional native event-notification batch-limit override.
    std::optional<std::string> eventsBatchLimit;

    /// Optional market-event notification aggregation-period override.
    std::optional<std::string> aggregationPeriod;
};

/// Contains global settings and workload profiles parsed from a suite configuration.
struct BenchmarkSuite {
    /// Optional experiment description embedded into generated reports.
    BenchmarkExperiment experiment;

    /// Number of complete suite repetitions.
    std::size_t repetitions{};

    /// Warm-up duration passed to the client.
    std::string warmup;

    /// Measurement duration passed to the client.
    std::string duration;

    /// Statistics window duration passed to the client.
    std::string window;

    /// Maximum wait for a complete publication batch.
    std::string batchTimeout;

    /// Maximum wait for initial profiles and recurring events.
    std::string startupTimeout;

    /// QD monitoring interval used by all benchmark processes.
    std::string monitoringPeriod;

    /// Default client endpoint role.
    std::string clientRole;

    /// Artificial delay applied before each market-event listener callback.
    std::string listenerDelay{"0"};

    /// Default native event-notification batch limit.
    std::string eventsBatchLimit{"optimal"};

    /// Default market-event notification aggregation period.
    std::string aggregationPeriod{"0"};

    /// Pause between consecutive benchmark runs, in seconds.
    std::size_t cooldownSeconds{};

    /// Client connector address.
    std::string address;

    /// Server listening address.
    std::string listenAddress;

    /// Workload profiles in their configured order.
    std::vector<BenchmarkSuiteProfile> profiles;
};

/// Holds command-line values that override suite-wide client settings.
struct BenchmarkOverrides {
    /// Optional default client-role override.
    std::optional<std::string> clientRole;

    /// Optional listener-delay override.
    std::optional<std::string> listenerDelay;

    /// Optional event-notification batch-limit override.
    std::optional<std::string> eventsBatchLimit;

    /// Optional market-event notification aggregation-period override.
    std::optional<std::string> aggregationPeriod;
};

/// Describes one fully resolved server/client execution in a benchmark plan.
struct BenchmarkRun {
    /// Source profile name.
    std::string profile;

    /// One-based repetition number.
    std::size_t repetition{};

    /// Unique output file prefix.
    std::string prefix;

    /// Server task expressed in the benchmark task DSL.
    std::string task;

    /// Effective client endpoint role.
    std::string clientRole;

    /// Effective listener delay.
    std::string listenerDelay;

    /// Effective native event-notification batch limit.
    std::string eventsBatchLimit;

    /// Effective market-event notification aggregation period.
    std::string aggregationPeriod;
};

/// Parses and validates a benchmark suite configuration.
/// @param input Text stream containing `KEY=value` settings and `PROFILE` entries.
/// @return Parsed suite, or a human-readable validation error.
std::expected<BenchmarkSuite, std::string> parseBenchmarkSuite(std::istream &input);

/// Reads, parses, and validates a benchmark suite configuration file.
/// @param path Path to the suite configuration file.
/// @return Parsed suite, or a human-readable I/O or validation error.
std::expected<BenchmarkSuite, std::string> readBenchmarkSuite(const std::filesystem::path &path);

/// Resolves profile overrides and rotates profile order between repetitions.
/// @param suite Parsed benchmark suite.
/// @param overrides Optional suite-wide command-line overrides.
/// @return Ordered list of fully resolved benchmark executions.
std::vector<BenchmarkRun> buildBenchmarkPlan(const BenchmarkSuite &suite, const BenchmarkOverrides &overrides = {});

/// Executes a complete benchmark suite and invokes the result analyzer.
/// @param binaryDirectory Directory containing all benchmark executables.
/// @param outputRoot Root directory below which a timestamped result directory is created.
/// @param configPath Suite configuration file to parse and preserve with the results.
/// @param overrides Optional suite-wide command-line overrides.
/// @param dryRun Whether to print the resolved plan without starting child processes.
/// @return Zero on success, one on execution failure, or two on invalid input.
int runBenchmarkSuite(const std::filesystem::path &binaryDirectory, const std::filesystem::path &outputRoot,
                      const std::filesystem::path &configPath, const BenchmarkOverrides &overrides, bool dryRun);

} // namespace latency
