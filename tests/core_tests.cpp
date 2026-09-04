#include "latency/core.hpp"
#include "latency/monitoring.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <ranges>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main(int argc, char **argv) {
    using namespace latency;
    const auto parsed = parseTask("SUB:Q100;S1;T5");
    check(parsed.has_value(), "valid mixed task parses");

    if (parsed) {
        check(parsed->toString() == "SUB:Q100;S1;T5", "task round trip");
        check(parsed->eventCount() == 106, "total quantity");
        const auto q = parsed->symbols(EventKind::QUOTE);
        check(q.size() == 100 && q.front() == "Q00" && q.back() == "Q99", "Q100 symbols");
    }

    check(!parseTask("SUB:"), "empty task rejected");
    check(!parseTask("SUB:Q0"), "zero rejected");
    check(!parseTask("SUB:Q1;Q2"), "duplicate rejected");
    check(!parseTask("SUB:X1"), "unknown type rejected");
    check(!parseTask("SUB:Q1junk"), "trailing input rejected");
    check(!parseTask("SUB:Q1;"), "trailing separator rejected");
    check(!parseTask("SUB:Q999999999999999999999999999999"), "overflow rejected");
    check(parseDuration("10s") == std::chrono::seconds{10}, "seconds duration");
    check(parseDuration("2m") == std::chrono::minutes{2}, "minutes duration");
    check(!parseDuration("0s"), "zero duration rejected");
    const auto monitoringPeriod = parseMonitoringPeriod("10s");
    check(monitoringPeriod && *monitoringPeriod == std::chrono::seconds{10}, "monitoring period");
    const auto disabledMonitoring = parseMonitoringPeriod("0");
    check(disabledMonitoring && !*disabledMonitoring, "monitoring disabled");
    check(!parseMonitoringPeriod("off"), "invalid monitoring period rejected");
    check(monitoringPeriodPropertyValue(std::chrono::seconds{10}) == "10s", "whole monitoring seconds");
    check(monitoringPeriodPropertyValue(std::chrono::milliseconds{1500}) == "1.5s", "fractional monitoring seconds");
    check(monitoringPeriodPropertyValue(std::nullopt) == "0", "disabled monitoring property");

    const auto stats = calculateStatistics({1, 1, 2, 2, 100});
    check(stats.count == 5 && stats.p50 == 2 && stats.q1 == 1 && stats.q3 == 2, "percentiles");
    check(stats.outlierThreshold == 3.5 && stats.outlierCount == 1, "IQR outlier");
    const auto flat = calculateStatistics({5, 5, 5, 5, 6});
    check(flat.iqr == 0 && flat.outlierCount == 1, "zero IQR behavior");
    check(calculateStatistics({}).count == 0, "empty statistics");
    check(nanosecondsToMicroseconds(123'456) == 123.456, "nanoseconds to microseconds");

    if (argc == 2) {
        const auto fixture = std::filesystem::path{argv[1]};
        const auto analysis = analyzeMonitoringDirectory(fixture, std::chrono::seconds{10});
        check(analysis.has_value(), "monitoring fixture parses");

        if (analysis) {
            check(analysis->samples.size() == 4, "all monitoring intervals parsed");
            check(analysis->samples.front().subscription == 3'001, "numbers with separators parsed");
            check(analysis->samples.front().readDataRps == 3'002, "read details parsed");
            check(analysis->samples.front().readDataLagUs == -800, "negative numbers parsed");
            check(analysis->samples.front().cpuPercent == 0.1, "fractional numbers parsed");
            check(!analysis->samples.front().sticky, "missing optional metric preserved");
            check(!analysis->aggregates.empty(), "measurement aggregates produced");
            const auto cpu = std::ranges::find_if(analysis->aggregates, [](const MonitoringAggregate &aggregate) {
                return aggregate.profile == "example" && aggregate.process == "client" &&
                       aggregate.metric == "cpu_percent";
            });
            check(cpu != analysis->aggregates.end() && cpu->samples == 2 && std::abs(cpu->mean - 0.15) < 0.0001,
                  "monitoring aggregate calculated");
        }

        check(!analyzeMonitoringDirectory(fixture / "missing", std::chrono::seconds{10}),
              "missing monitoring directory rejected");
        check(!analyzeMonitoringDirectory(fixture, std::chrono::milliseconds::zero()),
              "zero monitoring period rejected");

        const auto invalidFixture = std::filesystem::temp_directory_path() / "latency-monitoring-invalid-fixture";
        std::error_code filesystemError;
        std::filesystem::remove_all(invalidFixture, filesystemError);
        std::filesystem::create_directories(invalidFixture);
        std::filesystem::copy_file(fixture / "example-summary.csv", invalidFixture / "example-summary.csv");
        std::filesystem::copy_file(fixture / "example-server.log", invalidFixture / "example-server.log");
        check(!analyzeMonitoringDirectory(invalidFixture, std::chrono::seconds{10}), "missing process log rejected");
        std::filesystem::copy_file(fixture / "example-client.log", invalidFixture / "example-client.log");
        std::ofstream{invalidFixture / "example-summary.csv", std::ios::trunc}
            << "window_start_utc,window_end_utc,sample_kind,expected_per_batch\n"
               "2026-01-02T00:00:00Z,2026-01-02T00:01:00Z,event,3000junk\n";
        check(!analyzeMonitoringDirectory(invalidFixture, std::chrono::seconds{10}),
              "number with trailing characters rejected");
        std::filesystem::remove_all(invalidFixture, filesystemError);
    } else {
        check(false, "monitoring fixture path argument provided");
    }

    return failures ? 1 : 0;
}
