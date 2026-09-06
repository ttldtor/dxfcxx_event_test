// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/core.hpp"
#include "latency/monitoring.hpp"
#include "latency/runner.hpp"

#include <doctest/doctest.h>

#include <dxfeed_graal_cpp_api/system/System.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace latency;

/** Owns a clean temporary directory for one filesystem-oriented test case. */
class TemporaryDirectory {
    std::filesystem::path path_;

    public:
    /** Recreates the requested temporary directory. */
    explicit TemporaryDirectory(std::filesystem::path path) : path_(std::move(path)) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_);
    }

    /** Removes the temporary directory and all test artifacts below it. */
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    /** Returns the temporary directory path. */
    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }
};

const std::filesystem::path FIXTURE_DIRECTORY{LATENCY_TEST_DATA_DIR};

} // namespace

TEST_CASE("task and duration parsing") {
    const auto parsed = parseTask("SUB:Q100;S1;T5;E4");

    REQUIRE(parsed.has_value());
    CHECK(parsed->toString() == "SUB:Q100;S1;T5;E4");
    CHECK(parsed->eventCount() == 110);
    CHECK(parsed->symbolCount() == 100);
    CHECK(parsed->publishPeriod == 1s);

    const auto quotes = parsed->symbols(EventKind::QUOTE);

    REQUIRE(quotes.size() == 100);
    CHECK(quotes.front() == "SYM00");
    CHECK(quotes.back() == "SYM99");

    const auto symbols = parsed->symbols();

    REQUIRE(symbols.size() == 100);
    CHECK(symbols.front() == "SYM00");
    CHECK(symbols.back() == "SYM99");

    const auto differentlySized = parseTask("SUB:Q375;T37");

    REQUIRE(differentlySized.has_value());
    CHECK(differentlySized->symbols(EventKind::QUOTE).front() == "SYM000");
    CHECK(differentlySized->symbols(EventKind::TRADE).front() == "SYM000");
    CHECK(differentlySized->symbols(EventKind::TRADE).back() == "SYM036");
    CHECK(differentlySized->symbols().back() == "SYM374");

    const auto cadence = parseTask("SUB:Q500;T500;S500@10ms");

    REQUIRE(cadence.has_value());
    CHECK(cadence->toString() == "SUB:Q500;T500;S500@10ms");
    CHECK(cadence->publishPeriod == 10ms);
    CHECK(cadence->nominalEventsPerSecond() == 150'000);
    CHECK(cadence->batchCount(25ms) == 3);

    const auto timeAndSale = parseTask("SUB:Q375;N375@10ms#375");

    REQUIRE(timeAndSale.has_value());
    CHECK(timeAndSale->quantity(EventKind::TIME_AND_SALE) == 375);
    CHECK(timeAndSale->eventCount() == 750);
    CHECK(timeAndSale->symbols(EventKind::TIME_AND_SALE).front() == "SYM000");
    CHECK(timeAndSale->symbols(EventKind::TIME_AND_SALE).back() == "SYM374");
    CHECK(timeAndSale->toString() == "SUB:Q375;N375@10ms#375");

    const auto secondsCadence = parseTask("SUB:Q1@2s");

    REQUIRE(secondsCadence.has_value());
    CHECK(secondsCadence->toString() == "SUB:Q1@2s");

    const auto expandedUniverse = parseTask("SUB:Q375;T375;E375;S375@10ms#3750");

    REQUIRE(expandedUniverse.has_value());
    CHECK(expandedUniverse->toString() == "SUB:Q375;T375;E375;S375@10ms#3750");
    CHECK(expandedUniverse->eventCount() == 1500);
    CHECK(expandedUniverse->symbolCount() == 3750);
    CHECK(expandedUniverse->symbols().front() == "SYM0000");
    CHECK(expandedUniverse->symbols().back() == "SYM3749");
    CHECK(expandedUniverse->symbols(EventKind::QUOTE).size() == 375);

    const auto reordered = parseTask("SUB:S375;E375;T375;Q375@10ms#375");

    REQUIRE(reordered.has_value());
    REQUIRE(reordered->items.size() == 4);
    CHECK(reordered->items[0].kind == EventKind::SUMMARY);
    CHECK(reordered->items[1].kind == EventKind::TRADE_ETH);
    CHECK(reordered->items[2].kind == EventKind::TRADE);
    CHECK(reordered->items[3].kind == EventKind::QUOTE);
    CHECK(reordered->toString() == "SUB:S375;E375;T375;Q375@10ms#375");

    const auto shuffled = parseTask("SUB:Q375;T375;E375;S375@10ms#375~22805");

    REQUIRE(shuffled.has_value());
    CHECK(shuffled->shuffleSeed == 22805);
    CHECK(shuffled->toString() == "SUB:Q375;T375;E375;S375@10ms#375~22805");

    const auto regional = parseTask("SUB:Q375;T375;E375;S375@10ms#375&2~22805");

    REQUIRE(regional.has_value());
    CHECK(regional->regionalSourceCount == 2);
    CHECK(regional->eventCount() == 1500);
    CHECK(regional->symbolCount() == 375);
    CHECK(regional->marketSymbolCount() == 1125);
    CHECK(regional->toString() == "SUB:Q375;T375;E375;S375@10ms#375&2~22805");
    REQUIRE(regional->marketSymbols().size() == 1125);
    CHECK(regional->marketSymbols()[0] == "SYM000");
    CHECK(regional->marketSymbols()[374] == "SYM374");
    CHECK(regional->marketSymbols()[375] == "SYM000&A");
    CHECK(regional->marketSymbols().back() == "SYM374&B");

    CHECK_FALSE(parseTask("SUB:").has_value());
    CHECK_FALSE(parseTask("SUB:Q0").has_value());
    CHECK_FALSE(parseTask("SUB:Q1;Q2").has_value());
    CHECK_FALSE(parseTask("SUB:X1").has_value());
    CHECK_FALSE(parseTask("SUB:P1").has_value());
    CHECK_FALSE(parseTask("SUB:Q1junk").has_value());
    CHECK_FALSE(parseTask("SUB:Q1;").has_value());
    CHECK_FALSE(parseTask("SUB:Q1@").has_value());
    CHECK_FALSE(parseTask("SUB:Q1@0ms").has_value());
    CHECK_FALSE(parseTask("SUB:Q1@10ms@20ms").has_value());
    CHECK_FALSE(parseTask("SUB:Q10#").has_value());
    CHECK_FALSE(parseTask("SUB:Q10#0").has_value());
    CHECK_FALSE(parseTask("SUB:Q10#9").has_value());
    CHECK_FALSE(parseTask("SUB:Q10#20#30").has_value());
    CHECK_FALSE(parseTask("SUB:Q10#20@10ms").has_value());
    CHECK_FALSE(parseTask("SUB:Q10~").has_value());
    CHECK_FALSE(parseTask("SUB:Q10~seed").has_value());
    CHECK_FALSE(parseTask("SUB:Q10~1~2").has_value());
    CHECK_FALSE(parseTask("SUB:Q10~1@10ms").has_value());
    CHECK_FALSE(parseTask("SUB:Q10&").has_value());
    CHECK_FALSE(parseTask("SUB:Q10&0").has_value());
    CHECK_FALSE(parseTask("SUB:Q10&27").has_value());
    CHECK_FALSE(parseTask("SUB:Q10&1&2").has_value());
    CHECK_FALSE(parseTask("SUB:Q10&1@10ms").has_value());
    CHECK_FALSE(parseTask("SUB:Q10&1#20").has_value());
    CHECK_FALSE(parseTask("SUB:Q10~1&2").has_value());
    CHECK_FALSE(parseTask("SUB:Q999999999999999999999999999999").has_value());

    CHECK(parseDuration("10s") == 10s);
    CHECK(parseDuration("2m") == 2min);
    CHECK_FALSE(parseDuration("0s").has_value());

    const auto monitoringPeriod = parseMonitoringPeriod("10s");

    REQUIRE(monitoringPeriod.has_value());
    CHECK(*monitoringPeriod == 10s);

    const auto disabledMonitoring = parseMonitoringPeriod("0");

    REQUIRE(disabledMonitoring.has_value());
    CHECK_FALSE(disabledMonitoring->has_value());
    CHECK_FALSE(parseMonitoringPeriod("off").has_value());
    CHECK(monitoringPeriodPropertyValue(10s) == "10s");
    CHECK(monitoringPeriodPropertyValue(1500ms) == "1.5s");
    CHECK(monitoringPeriodPropertyValue(std::nullopt) == "0");
}

TEST_CASE("task marker symbols use a separate control channel") {
    const auto symbol = markerSymbol("SUB:Q375;T375@10ms");

    CHECK(symbol == "LATENCY_MARKER:SUB:Q375;T375@10ms");
    CHECK(isMarkerSymbol(symbol));
    CHECK_FALSE(isMarkerSymbol("SUB:Q375;T375@10ms"));
    CHECK_FALSE(parseTask(symbol).has_value());
}

TEST_CASE("statistics and benchmark profile comparison") {
    const auto stats = calculateStatistics({1, 1, 2, 2, 100});

    CHECK(stats.count == 5);
    CHECK(stats.p50 == 2);
    CHECK(stats.q1 == 1);
    CHECK(stats.q3 == 2);
    CHECK(stats.outlierThreshold == doctest::Approx{3.5});
    CHECK(stats.outlierCount == 1);

    const auto flat = calculateStatistics({5, 5, 5, 5, 6});

    CHECK(flat.iqr == 0);
    CHECK(flat.outlierCount == 1);
    CHECK(calculateStatistics({}).count == 0);
    CHECK(nanosecondsToMicroseconds(123'456) == doctest::Approx{123.456});

    const auto repeatedProfile = parseBenchmarkProfile("q50k-t50k-s50k-r03");
    const auto legacyProfile = parseBenchmarkProfile("legacy-profile");

    CHECK(repeatedProfile == (BenchmarkProfile{"q50k-t50k-s50k", 3}));
    CHECK(legacyProfile == (BenchmarkProfile{"legacy-profile", 1}));

    const auto comparison = compareRuns({9, 1, 5, 3});

    CHECK(comparison.runs == 4);
    CHECK(comparison.minimum == 1);
    CHECK(comparison.median == 4);
    CHECK(comparison.maximum == 9);
}

TEST_CASE("monitoring fixture analysis") {
    const auto analysis = analyzeMonitoringDirectory(FIXTURE_DIRECTORY, 10s);

    REQUIRE(analysis.has_value());
    REQUIRE(analysis->samples.size() == 4);
    CHECK(analysis->samples.front().subscription == 3'001);
    CHECK(analysis->samples.front().readDataRps == 3'002);
    CHECK(analysis->samples.front().readDataLagUs == -800);
    CHECK(analysis->samples.front().cpuPercent == doctest::Approx{0.1});
    CHECK_FALSE(analysis->samples.front().sticky.has_value());
    CHECK_FALSE(analysis->aggregates.empty());

    const auto cpu = std::ranges::find_if(analysis->aggregates, [](const MonitoringAggregate &aggregate) {
        return aggregate.profile == "example" && aggregate.process == "client" && aggregate.metric == "cpu_percent";
    });

    REQUIRE(cpu != analysis->aggregates.end());
    CHECK(cpu->samples == 2);
    CHECK(cpu->mean == doctest::Approx{0.15});
}

TEST_CASE("repeated benchmark comparison files") {
    TemporaryDirectory repeatedFixture{std::filesystem::temp_directory_path() / "latency-repeated-fixture"};

    {
        std::ofstream suite{repeatedFixture.path() / "suite.conf"};
        suite << R"(EXPERIMENT_TITLE=Controlled delivery experiment
EXPERIMENT_OBJECTIVE=Determine whether the changed setting affects delivery.
EXPERIMENT_VARIABLE=Endpoint setting.
EXPERIMENT_CONTROLS=Compiler, workload, duration, and host.
EXPERIMENT_SUCCESS_CRITERIA=Compare listener coverage, latency, QD drops, and resource use.
EXPERIMENT_LIMITATIONS=Does not represent a production network.
REPETITIONS=3
WARMUP=1s
DURATION=2s
WINDOW=1s
BATCH_TIMEOUT=3s
STARTUP_TIMEOUT=4s
MONITORING_PERIOD=1s
CLIENT_ROLE=stream-feed
COOLDOWN_SECONDS=0
ADDRESS=127.0.0.1:7400
LISTEN_ADDRESS=:7400
PROFILE=example|SUB:Q1
)";
    }

    for (const auto repetition : {"r01", "r02", "r03"}) {
        const auto profile = std::format("example-{}", repetition);
        const auto legacyProfile = std::format("legacy-example-{}", repetition);
        std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-summary.csv",
                                   repeatedFixture.path() / std::format("{}-summary.csv", profile));
        std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-server.log",
                                   repeatedFixture.path() / std::format("{}-server.log", profile));
        std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-client.log",
                                   repeatedFixture.path() / std::format("{}-client.log", profile));
        std::ofstream timeSeries{repeatedFixture.path() / std::format("{}-time-series.csv", profile)};
        timeSeries
            << R"(from_time_ms,requested_symbols,observed_symbols,completed_symbols,snapshot_events,snapshot_callbacks,snapshot_begin,snapshot_end,snapshot_snip,snapshot_remove,duplicate_indices,premature_live_events,live_events,clock_anomalies,first_event_delay_ms,snapshot_duration_ms,first_live_after_snapshot_ms,live_latency_samples,live_latency_mean_us,live_latency_p50_us,live_latency_p90_us,live_latency_p99_us,live_latency_p999_us,live_latency_max_us
1788307200000,1,1,1,200,2,1,1,0,1,0,0,100,0,1.5,3.0,0.5,100,100,90,150,200,250,300
)";
        std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-server.log",
                                   repeatedFixture.path() / std::format("{}-server.log", legacyProfile));
        std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-client.log",
                                   repeatedFixture.path() / std::format("{}-client.log", legacyProfile));
        std::ofstream delivery{repeatedFixture.path() / std::format("{}-delivery.csv", legacyProfile)};
        delivery
            << R"("window_start_utc","window_end_utc","sample_kind","expected_per_batch","nominal_events_per_second","callbacks","recurring_events","quote","trade","trade_eth","summary","profiles","maximum_data_count","actual_events_per_second","cpu_core_percent","cpu_host_percent","rss_mean_bytes","rss_maximum_bytes","resource_samples","contract"
"2026-01-02T00:00:00.000Z","2026-01-02T23:59:00.000Z","event",4,400,1200,1200,300,300,300,300,0,1,400,25,2.5,104857600,125829120,120,"default"
)";
    }

    const auto analysis = analyzeMonitoringDirectory(repeatedFixture.path(), 10s);

    REQUIRE(analysis.has_value());
    REQUIRE(writeBenchmarkComparison(repeatedFixture.path(), *analysis).has_value());
    CHECK(std::filesystem::file_size(repeatedFixture.path() / "latency-runs.csv") > 0);
    CHECK(std::filesystem::file_size(repeatedFixture.path() / "delivery-runs.csv") > 0);
    CHECK(std::filesystem::file_size(repeatedFixture.path() / "delivery-comparison.csv") > 0);
    CHECK(std::filesystem::file_size(repeatedFixture.path() / "time-series-runs.csv") > 0);
    CHECK(std::filesystem::file_size(repeatedFixture.path() / "time-series-comparison.csv") > 0);
    CHECK(std::filesystem::file_size(repeatedFixture.path() / "monitoring-comparison.csv") > 0);

    std::ifstream latencyRuns{repeatedFixture.path() / "latency-runs.csv"};
    const std::string latencyRunsText{std::istreambuf_iterator<char>{latencyRuns}, {}};

    CHECK(latencyRunsText.contains("\"listener_deficit\",\"listener_coverage\""));
    CHECK_FALSE(latencyRunsText.contains("\"not_delivered\""));

    std::ifstream report{repeatedFixture.path() / "REPORT.md"};
    const std::string reportText{std::istreambuf_iterator<char>{report}, {}};

    CHECK(reportText.starts_with("# Controlled delivery experiment"));
    CHECK(reportText.contains("## Experiment definition"));
    CHECK(reportText.contains("**Objective:** Determine whether the changed setting affects delivery."));
    CHECK(reportText.contains("**Changed variable:** Endpoint setting."));
    CHECK(reportText.contains("**Controls:** Compiler, workload, duration, and host."));
    CHECK(reportText.contains(
        "**Evaluation criteria:** Compare listener coverage, latency, QD drops, and resource use."));
    CHECK(reportText.contains("**Limitations:** Does not represent a production network."));
    CHECK(reportText.contains("## Results"));
    CHECK(reportText.contains("## Legacy C API delivery"));
    CHECK(reportText.contains("## TimeAndSale snapshot and live cutover"));
    CHECK(reportText.contains("| example | 3 | 1 | 200 (200–200)"));
    CHECK(reportText.contains("| legacy-example | default | 3 | 400.000 | 400.000"));
    CHECK(reportText.contains("| example | stream-feed | unknown | 0.000 ms | 3 |"));
    CHECK(reportText.contains("Listener coverage median"));
    CHECK(reportText.contains("Listener deficit median"));
    CHECK(reportText.contains("not a transport-loss"));
    CHECK(reportText.contains("QD-drop counter"));
    CHECK(reportText.contains("## Client monitoring"));
    CHECK(reportText.contains("| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |"));
    CHECK(reportText.contains("## Server monitoring"));
    CHECK(reportText.contains("| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |"));
    CHECK(reportText.contains("`Dropped = 0` rules out drops counted by the corresponding QD endpoint"));
    CHECK(reportText.contains("cannot locate TICKER supersession on the publisher or feed side"));
    CHECK(reportText.contains("must not be interpreted as zero"));
    CHECK_FALSE(reportText.contains("| n/a |"));

    report.close();
    std::filesystem::remove(repeatedFixture.path() / "suite.conf");
    REQUIRE(writeBenchmarkComparison(repeatedFixture.path(), *analysis).has_value());

    std::ifstream legacyReport{repeatedFixture.path() / "REPORT.md"};
    const std::string legacyReportText{std::istreambuf_iterator<char>{legacyReport}, {}};

    CHECK(legacyReportText.starts_with("# Repeated latency benchmark"));
    CHECK_FALSE(legacyReportText.contains("## Experiment definition"));
    CHECK(legacyReportText.contains("## Results"));
}

TEST_CASE("invalid monitoring inputs are rejected") {
    CHECK_FALSE(analyzeMonitoringDirectory(FIXTURE_DIRECTORY / "missing", 10s).has_value());
    CHECK_FALSE(analyzeMonitoringDirectory(FIXTURE_DIRECTORY, 0ms).has_value());

    TemporaryDirectory invalidFixture{std::filesystem::temp_directory_path() / "latency-monitoring-invalid-fixture"};
    std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-summary.csv",
                               invalidFixture.path() / "example-summary.csv");
    std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-server.log", invalidFixture.path() / "example-server.log");

    CHECK_FALSE(analyzeMonitoringDirectory(invalidFixture.path(), 10s).has_value());

    std::filesystem::copy_file(FIXTURE_DIRECTORY / "example-client.log", invalidFixture.path() / "example-client.log");

    {
        std::ofstream summary{invalidFixture.path() / "example-summary.csv", std::ios::trunc};
        summary << R"(window_start_utc,window_end_utc,sample_kind,expected_per_batch
2026-01-02T00:00:00Z,2026-01-02T00:01:00Z,event,3000junk
)";
    }

    CHECK_FALSE(analyzeMonitoringDirectory(invalidFixture.path(), 10s).has_value());
}

TEST_CASE("the default isolate properties file enables nanosecond timestamps") {
    INFO("working directory: " << std::filesystem::current_path().string());
    CHECK(dxfcpp::System::getProperty("dxscheme.nanoTime") == "true");
}

TEST_CASE("benchmark suite parsing and rotating execution plan") {
    std::istringstream input{R"(# benchmark suite
EXPERIMENT_TITLE=Parser experiment
EXPERIMENT_OBJECTIVE=Verify experiment metadata parsing.
EXPERIMENT_VARIABLE=Test input.
EXPERIMENT_CONTROLS=All other fixture values.
EXPERIMENT_SUCCESS_CRITERIA=Every field is preserved.
EXPERIMENT_LIMITATIONS=Parser test only.
REPETITIONS=2
WARMUP=1s
DURATION=2s
WINDOW=1s
BATCH_TIMEOUT=3s
STARTUP_TIMEOUT=4s
MONITORING_PERIOD=1s
CLIENT_ROLE=feed
LISTENER_DELAY=5us
EVENTS_BATCH_LIMIT=optimal
AGGREGATION_PERIOD=0
COOLDOWN_SECONDS=0
ADDRESS=127.0.0.1:7400
LISTEN_ADDRESS=:7400
PROFILE=first|SUB:Q1
PROFILE=second|SUB:T2|stream-feed|1|10ms|legacy
)"};
    const auto suite = parseBenchmarkSuite(input);

    REQUIRE(suite.has_value());
    CHECK(suite->experiment.title == "Parser experiment");
    CHECK(suite->experiment.objective == "Verify experiment metadata parsing.");
    CHECK(suite->experiment.variable == "Test input.");
    CHECK(suite->experiment.controls == "All other fixture values.");
    CHECK(suite->experiment.successCriteria == "Every field is preserved.");
    CHECK(suite->experiment.limitations == "Parser test only.");
    CHECK(suite->repetitions == 2);
    CHECK(suite->profiles.size() == 2);
    CHECK(suite->profiles[1].clientRole == "stream-feed");
    CHECK(suite->profiles[1].eventsBatchLimit == "1");
    CHECK(suite->profiles[1].aggregationPeriod == "10ms");
    CHECK(suite->profiles[1].clientImplementation == "legacy");

    const auto plan =
        buildBenchmarkPlan(*suite, {.clientRole = "feed", .eventsBatchLimit = "375", .aggregationPeriod = "1ms"});

    REQUIRE(plan.size() == 4);
    CHECK(plan[0].prefix == "first-r01");
    CHECK(plan[0].eventsBatchLimit == "375");
    CHECK(plan[0].aggregationPeriod == "1ms");
    CHECK(plan[1].prefix == "second-r01");
    CHECK(plan[1].clientRole == "stream-feed");
    CHECK(plan[1].eventsBatchLimit == "1");
    CHECK(plan[1].clientImplementation == "legacy");
    CHECK(plan[1].aggregationPeriod == "10ms");
    CHECK(plan[2].prefix == "second-r02");
    CHECK(plan[3].prefix == "first-r02");

    std::istringstream legacyInput{R"(REPETITIONS=1
WARMUP=1s
DURATION=2s
WINDOW=1s
BATCH_TIMEOUT=3s
STARTUP_TIMEOUT=4s
MONITORING_PERIOD=1s
CLIENT_ROLE=feed
COOLDOWN_SECONDS=0
ADDRESS=127.0.0.1:7400
LISTEN_ADDRESS=:7400
PROFILE=legacy|SUB:Q1
)"};
    const auto legacySuite = parseBenchmarkSuite(legacyInput);

    REQUIRE(legacySuite.has_value());
    CHECK(legacySuite->experiment.title.empty());
}

TEST_CASE("invalid benchmark suite settings are rejected") {
    std::istringstream input{R"(REPETITIONS=0
UNKNOWN=value
)"};

    CHECK_FALSE(parseBenchmarkSuite(input).has_value());

    std::istringstream incompleteMetadata{R"(EXPERIMENT_TITLE=Incomplete metadata
REPETITIONS=1
WARMUP=1s
DURATION=1s
WINDOW=1s
BATCH_TIMEOUT=1s
STARTUP_TIMEOUT=1s
MONITORING_PERIOD=1s
CLIENT_ROLE=feed
COOLDOWN_SECONDS=0
ADDRESS=127.0.0.1:7400
LISTEN_ADDRESS=:7400
PROFILE=example|SUB:Q1
)"};

    CHECK_FALSE(parseBenchmarkSuite(incompleteMetadata).has_value());

    std::istringstream streamFeedTimeSeries{R"(REPETITIONS=1
WARMUP=1s
DURATION=1s
WINDOW=1s
BATCH_TIMEOUT=1s
STARTUP_TIMEOUT=1s
MONITORING_PERIOD=1s
CLIENT_ROLE=stream-feed
COOLDOWN_SECONDS=0
ADDRESS=127.0.0.1:7400
LISTEN_ADDRESS=:7400
PROFILE=time-series|SUB:Q1;N1
)"};

    const auto invalidRole = parseBenchmarkSuite(streamFeedTimeSeries);

    REQUIRE_FALSE(invalidRole.has_value());
    CHECK(invalidRole.error().contains("requires the feed client role"));

    std::istringstream legacyTimeSeries{R"(REPETITIONS=1
WARMUP=1s
DURATION=1s
WINDOW=1s
BATCH_TIMEOUT=1s
STARTUP_TIMEOUT=1s
MONITORING_PERIOD=1s
CLIENT_ROLE=feed
COOLDOWN_SECONDS=0
ADDRESS=127.0.0.1:7400
LISTEN_ADDRESS=:7400
PROFILE=time-series|SUB:Q1;N1||||legacy
)"};

    const auto invalidImplementation = parseBenchmarkSuite(legacyTimeSeries);

    REQUIRE_FALSE(invalidImplementation.has_value());
    CHECK(invalidImplementation.error().contains("not supported by the legacy client"));
}
