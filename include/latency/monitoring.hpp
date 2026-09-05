// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace latency {

/** Metrics extracted from one QD monitoring interval. */
struct MonitoringSample {
    /** Benchmark profile inferred from the latency summary filename. */
    std::string profile;
    /** Process that emitted the record: `server` or `client`. */
    std::string process;
    /** QD endpoint name found in the log record. */
    std::string endpoint;
    /** Inclusive interval start formatted as UTC ISO-8601. */
    std::string intervalStartUtc;
    /** Inclusive interval end formatted as UTC ISO-8601. */
    std::string intervalEndUtc;
    /** Whether the whole interval is inside the latency measurement phase. */
    bool inMeasurement{};
    /** Expected number of published events per second. */
    double nominalEventsPerSecond{};
    /** QD subscription count. */
    std::optional<double> subscription;
    /** QD sticky-record count. */
    std::optional<double> sticky;
    /** QD storage-record count. */
    std::optional<double> storage;
    /** QD outgoing-buffer size. */
    std::optional<double> buffer;
    /** Number of records dropped by QD. */
    std::optional<double> dropped;
    /** Read throughput in bytes per second. */
    std::optional<double> readBps;
    /** Incoming subscription records per second. */
    std::optional<double> readSubscriptionRps;
    /** Incoming data records per second. */
    std::optional<double> readDataRps;
    /** Incoming data lag in microseconds. */
    std::optional<double> readDataLagUs;
    /** Write throughput in bytes per second. */
    std::optional<double> writeBps;
    /** Outgoing subscription records per second. */
    std::optional<double> writeSubscriptionRps;
    /** Outgoing data records per second. */
    std::optional<double> writeDataRps;
    /** Outgoing data lag in microseconds. */
    std::optional<double> writeDataLagUs;
    /** Round-trip time in microseconds. */
    std::optional<double> rttUs;
    /** Process CPU utilization reported by QD. */
    std::optional<double> cpuPercent;
};

/** Aggregate for one monitoring metric over measurement-contained intervals. */
struct MonitoringAggregate {
    /** Benchmark profile. */
    std::string profile;
    /** Source process: `server` or `client`. */
    std::string process;
    /** Expected number of published events per second. */
    double nominalEventsPerSecond{};
    /** Metric column name from `monitoring.csv`. */
    std::string metric;
    /** Number of available values. */
    std::size_t samples{};
    /** Smallest value. */
    double minimum{};
    /** Arithmetic mean. */
    double mean{};
    /** Largest value. */
    double maximum{};
    /** Sum of all values. */
    double sum{};
};

/** Complete result of monitoring-log analysis. */
struct MonitoringAnalysis {
    /** Parsed interval records. */
    std::vector<MonitoringSample> samples;
    /** Per-profile and per-process metric aggregates. */
    std::vector<MonitoringAggregate> aggregates;
};

/** A benchmark output prefix split into a scenario and repetition number. */
struct BenchmarkProfile {
    std::string scenario;
    std::size_t repetition{1};

    friend bool operator==(const BenchmarkProfile &, const BenchmarkProfile &) = default;
};

/** Minimum, median, and maximum across independent benchmark runs. */
struct RunComparison {
    std::size_t runs{};
    double minimum{};
    double median{};
    double maximum{};
};

/** Splits `q1k-r02` into scenario `q1k` and repetition `2`; an absent suffix means repetition 1. */
BenchmarkProfile parseBenchmarkProfile(std::string_view profile);

/** Calculates a run-level range and Excel-compatible median. */
RunComparison compareRuns(std::vector<double> values);

/**
 * Analyzes matching latency summaries and QD server/client logs in a benchmark directory.
 *
 * @param runDirectory Directory containing `<profile>-summary.csv` and corresponding log files.
 * @param monitoringPeriod Configured QD monitoring period, used for the first interval in each log.
 * @return Parsed samples and aggregates, or a human-readable error.
 */
std::expected<MonitoringAnalysis, std::string> analyzeMonitoringDirectory(const std::filesystem::path &runDirectory,
                                                                          std::chrono::milliseconds monitoringPeriod);

/**
 * Writes `monitoring.csv` and `monitoring-summary.csv` to a benchmark directory.
 *
 * @param runDirectory Destination benchmark directory.
 * @param analysis Analysis result to serialize.
 * @return Nothing on success, or a human-readable error.
 */
std::expected<void, std::string> writeMonitoringAnalysis(const std::filesystem::path &runDirectory,
                                                         const MonitoringAnalysis &analysis);

/** Writes latency/monitoring repetition comparisons and a concise Markdown report. */
std::expected<void, std::string> writeBenchmarkComparison(const std::filesystem::path &runDirectory,
                                                          const MonitoringAnalysis &analysis);

} // namespace latency
