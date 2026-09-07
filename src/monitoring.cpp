// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/monitoring.hpp"
#include "latency/runner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <string_view>
#include <tuple>

namespace latency {
namespace {
constexpr std::string_view SUMMARY_SUFFIX = "-summary.csv";
constexpr std::string_view DELIVERY_SUFFIX = "-delivery.csv";
constexpr std::string_view CLIENT_LOG_SUFFIX = "-client.log";
constexpr std::string_view TIME_SERIES_SUFFIX = "-time-series.csv";
constexpr std::string_view SNAPSHOT_OVERLAP_SUFFIX = "-snapshot-overlap.csv";
constexpr std::string_view SOURCE_TIME_METHODS_SUFFIX = "-source-time-methods.csv";
constexpr std::string_view NUMBER_PATTERN = R"([-+]?[0-9][0-9,]*(?:\.[0-9]+)?)";

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

/** Defines the latency-measurement interval associated with one run. */
struct Measurement {
    TimePoint start;
    TimePoint end;
    double nominalEventsPerSecond{};
};

/** Maps a monitoring CSV metric name to its source sample member. */
struct MetricDefinition {
    std::string_view name;
    std::optional<double> MonitoringSample::*member;
};

/** Contains one whole-run latency row used to build cross-run comparisons. */
struct LatencyRunRow {
    std::string profile;
    BenchmarkProfile identity;
    std::string sampleKind;
    double nominalEventsPerSecond{};
    double samples{};
    double expectedPerBatch{};
    std::string endpointRole{"stream-feed"};
    std::string eventsBatchLimit{"unknown"};
    double aggregationPeriodMs{};
    double published{};
    double delivered{};
    double listenerDeficit{};
    double listenerCoverage{};
    double excessEvents{};
    double fullPublications{};
    double partialPublications{};
    double emptyPublications{};
    double uncorrelatedEvents{};
    double callbacks{};
    double clockAnomalies{};
    double missingBatches{};
    double pendingBatches{};
    double minimumUs{};
    double meanUs{};
    double p50Us{};
    double p90Us{};
    double p95Us{};
    double p99Us{};
    double p999Us{};
    double maximumUs{};
    double outliers{};
    bool integrityOk{};
};

/** Contains one source-time selection method emitted by the full Graal client. */
struct SourceTimeMethodRunRow {
    std::string profile;
    BenchmarkProfile identity;
    std::string method;
    std::string monotonicScope;
    std::string timestampResolution;
    double observations{};
    double accepted{};
    double rejected{};
    double acceptanceRatio{};
    double negativeClamped{};
    double minimumUs{};
    double meanUs{};
    double p50Us{};
    double p90Us{};
    double p95Us{};
    double p99Us{};
    double p999Us{};
    double maximumUs{};
    double outliers{};
};

/** Contains one whole-run callback-delivery row emitted by a delivery-only client. */
struct DeliveryRunRow {
    std::string profile;
    BenchmarkProfile identity;
    double nominalEventsPerSecond{};
    double callbacks{};
    double recurringEvents{};
    double quotes{};
    double trades{};
    double tradeEths{};
    double summaries{};
    double profiles{};
    double maximumDataCount{};
    double actualEventsPerSecond{};
    double cpuCorePercent{std::numeric_limits<double>::quiet_NaN()};
    double cpuHostPercent{std::numeric_limits<double>::quiet_NaN()};
    double rssMeanBytes{std::numeric_limits<double>::quiet_NaN()};
    double rssMaximumBytes{std::numeric_limits<double>::quiet_NaN()};
    double resourceSamples{std::numeric_limits<double>::quiet_NaN()};
    std::string implementation{"legacy"};
    std::string contract;
};

/** Contains process CPU and resident-memory measurements emitted by one benchmark client. */
struct ClientResourceRunRow {
    std::string profile;
    BenchmarkProfile identity;
    std::string clientWorkload;
    double nominalEventsPerSecond{};
    double cpuCorePercent{};
    double cpuHostPercent{};
    double rssMeanBytes{};
    double rssMaximumBytes{};
    double samples{};
};

/** Contains one TimeAndSale HISTORY snapshot and live-cutover result. */
struct TimeSeriesRunRow {
    std::string profile;
    BenchmarkProfile identity;
    double requestedSymbols{};
    double observedSymbols{};
    double completedSymbols{};
    double snapshotEvents{};
    double snapshotCallbacks{};
    double snapshotBegins{};
    double snapshotEnds{};
    double snapshotSnips{};
    double snapshotRemovals{};
    double duplicateIndices{};
    double prematureLiveEvents{};
    double liveEvents{};
    double clockAnomalies{};
    double firstEventDelayMs{};
    double snapshotDurationMs{};
    double firstLiveRelativeToGlobalCompletionMs{};
    double liveLatencySamples{};
    double liveLatencyMeanUs{};
    double liveLatencyP50Us{};
    double liveLatencyP90Us{};
    double liveLatencyP99Us{};
    double liveLatencyP999Us{};
    double liveLatencyMaximumUs{};
    bool unsubscribedAfterSnapshot{};
    double cpuCorePercent{std::numeric_limits<double>::quiet_NaN()};
    double cpuHostPercent{std::numeric_limits<double>::quiet_NaN()};
    double rssMeanBytes{std::numeric_limits<double>::quiet_NaN()};
    double rssMaximumBytes{std::numeric_limits<double>::quiet_NaN()};
    double resourceSamples{std::numeric_limits<double>::quiet_NaN()};
    bool integrityOk{};
};

/** Contains one before/during/after ticker measurement around a TimeAndSale snapshot. */
struct SnapshotOverlapRunRow {
    std::string profile;
    BenchmarkProfile identity;
    std::string phase;
    std::string sampleKind;
    double durationMs{};
    double samples{};
    double published{};
    double delivered{};
    double listenerDeficit{};
    double listenerCoverage{};
    double excessEvents{};
    double callbacks{};
    double clockAnomalies{};
    double missingBatches{};
    double pendingBatches{};
    double minimumUs{};
    double meanUs{};
    double p50Us{};
    double p90Us{};
    double p95Us{};
    double p99Us{};
    double p999Us{};
    double maximumUs{};
    double cpuCorePercent{};
    double cpuHostPercent{};
    double rssMeanBytes{};
    double rssMaximumBytes{};
    double resourceSamples{};
};

/** Reads optional experiment metadata from the suite configuration preserved with a benchmark run. */
std::expected<std::optional<BenchmarkExperiment>, std::string>
readExperimentMetadata(const std::filesystem::path &runDirectory) {
    const auto suitePath = runDirectory / "suite.conf";

    if (!std::filesystem::exists(suitePath)) {
        return std::nullopt;
    }

    const auto suite = readBenchmarkSuite(suitePath);

    if (!suite) {
        return std::unexpected{std::format("Unable to read experiment metadata: {}", suite.error())};
    }

    if (suite->experiment.title.empty()) {
        return std::nullopt;
    }

    return suite->experiment;
}

/** Writes the report title and optional experiment definition. */
void writeExperimentDefinition(std::ostream &output, const std::optional<BenchmarkExperiment> &experiment) {
    if (!experiment) {
        output << "# Repeated latency benchmark\n\n";

        return;
    }

    output << std::format(R"(# {}

## Experiment definition

- **Objective:** {}
- **Changed variable:** {}
- **Controls:** {}
- **Evaluation criteria:** {}
- **Limitations:** {}

)",
                          experiment->title, experiment->objective, experiment->variable, experiment->controls,
                          experiment->successCriteria, experiment->limitations);
}

/** Contains one minimum/median/maximum comparison row. */
struct ComparisonRow {
    std::string scenario;
    std::string category;
    std::string metric;
    RunComparison comparison;
};

constexpr std::array METRICS{
    MetricDefinition{"subscription", &MonitoringSample::subscription},
    MetricDefinition{"sticky", &MonitoringSample::sticky},
    MetricDefinition{"storage", &MonitoringSample::storage},
    MetricDefinition{"buffer", &MonitoringSample::buffer},
    MetricDefinition{"dropped", &MonitoringSample::dropped},
    MetricDefinition{"read_bps", &MonitoringSample::readBps},
    MetricDefinition{"read_subscription_rps", &MonitoringSample::readSubscriptionRps},
    MetricDefinition{"read_data_rps", &MonitoringSample::readDataRps},
    MetricDefinition{"read_data_lag_us", &MonitoringSample::readDataLagUs},
    MetricDefinition{"write_bps", &MonitoringSample::writeBps},
    MetricDefinition{"write_subscription_rps", &MonitoringSample::writeSubscriptionRps},
    MetricDefinition{"write_data_rps", &MonitoringSample::writeDataRps},
    MetricDefinition{"write_data_lag_us", &MonitoringSample::writeDataLagUs},
    MetricDefinition{"rtt_us", &MonitoringSample::rttUs},
    MetricDefinition{"cpu_percent", &MonitoringSample::cpuPercent},
};

/** Parses one RFC 4180-style CSV row. */
std::vector<std::string> parseCsvRow(std::string_view line) {
    std::vector<std::string> result;
    std::string value;
    bool quoted = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const auto character = line[i];

        if (quoted) {
            if (character == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                value.push_back('"');
                ++i;
            } else if (character == '"') {
                quoted = false;
            } else {
                value.push_back(character);
            }
        } else if (character == '"') {
            quoted = true;
        } else if (character == ',') {
            result.push_back(std::move(value));
            value.clear();
        } else if (character != '\r') {
            value.push_back(character);
        }
    }

    result.push_back(std::move(value));

    return result;
}

/** Parses one locale-independent decimal number. */
std::expected<double, std::string> parseNumber(std::string text) {
    std::erase(text, ',');
    std::istringstream input{text};

    input.imbue(std::locale::classic());

    double value{};

    input >> std::noskipws >> value;

    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return std::unexpected(std::format("invalid number: {}", text));
    }

    return value;
}

/** Extracts and parses the first captured number matching a regular expression. */
std::optional<double> findNumber(const std::string &text, const std::string &pattern) {
    std::smatch match;

    if (!std::regex_search(text, match, std::regex{pattern})) {
        return std::nullopt;
    }

    return parseNumber(match[1].str()).value_or(std::numeric_limits<double>::quiet_NaN());
}

/** Converts a UTC calendar value to Unix time on the current platform. */
std::time_t utcTime(std::tm *value) {
#ifdef _WIN32
    const auto result = _mkgmtime(value);
#else
    const auto result = timegm(value);
#endif

    return result;
}

/** Parses an ISO-8601 UTC timestamp with optional fractional seconds. */
std::expected<TimePoint, std::string> parseUtcTimestamp(std::string_view text) {
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::unexpected(std::format("invalid UTC timestamp: {}", text));
    }

    std::tm calendar{};

    try {
        calendar.tm_year = std::stoi(std::string{text.substr(0, 4)}) - 1900;
        calendar.tm_mon = std::stoi(std::string{text.substr(5, 2)}) - 1;
        calendar.tm_mday = std::stoi(std::string{text.substr(8, 2)});
        calendar.tm_hour = std::stoi(std::string{text.substr(11, 2)});
        calendar.tm_min = std::stoi(std::string{text.substr(14, 2)});
        calendar.tm_sec = std::stoi(std::string{text.substr(17, 2)});
    } catch (const std::exception &) {
        return std::unexpected(std::format("invalid UTC timestamp: {}", text));
    }

    const auto seconds = utcTime(&calendar);

    if (seconds == static_cast<std::time_t>(-1)) {
        return std::unexpected(std::format("invalid UTC timestamp: {}", text));
    }

    std::chrono::nanoseconds fraction{};
    const auto dot = text.find('.', 19);

    if (dot != std::string_view::npos) {
        const auto end = text.find_first_of("Z+-", dot);
        auto digits = std::string{text.substr(dot + 1, end - dot - 1)};

        if (digits.size() > 9) {
            digits.resize(9);
        }

        while (digits.size() < 9) {
            digits.push_back('0');
        }

        auto parsed = parseNumber(digits);

        if (!parsed) {
            return std::unexpected(std::format("invalid UTC timestamp: {}", text));
        }

        fraction = std::chrono::nanoseconds{static_cast<std::int64_t>(*parsed)};
    }

    return std::chrono::time_point_cast<Clock::duration>(Clock::from_time_t(seconds) + fraction);
}

/** Parses the local timestamp format emitted by QD monitoring logs. */
std::expected<TimePoint, std::string> parseLocalLogTimestamp(const std::string &date, const std::string &time) {
    if (date.size() != 6 || time.size() != 10) {
        return std::unexpected(std::format("invalid QD timestamp: {} {}", date, time));
    }

    std::tm calendar{};

    try {
        const auto year = std::stoi(date.substr(0, 2));
        calendar.tm_year = (year >= 70 ? 1900 + year : 2000 + year) - 1900;
        calendar.tm_mon = std::stoi(date.substr(2, 2)) - 1;
        calendar.tm_mday = std::stoi(date.substr(4, 2));
        calendar.tm_hour = std::stoi(time.substr(0, 2));
        calendar.tm_min = std::stoi(time.substr(2, 2));
        calendar.tm_sec = std::stoi(time.substr(4, 2));
        calendar.tm_isdst = -1;
    } catch (const std::exception &) {
        return std::unexpected(std::format("invalid QD timestamp: {} {}", date, time));
    }

    const auto seconds = std::mktime(&calendar);

    if (seconds == static_cast<std::time_t>(-1)) {
        return std::unexpected(std::format("invalid QD timestamp: {} {}", date, time));
    }

    const auto milliseconds = std::chrono::milliseconds{std::stoi(time.substr(7, 3))};

    return Clock::from_time_t(seconds) + milliseconds;
}

/** Formats a time point as an ISO-8601 UTC timestamp with millisecond precision. */
std::string formatUtc(TimePoint value) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    auto fraction = milliseconds - seconds;

    if (fraction.count() < 0) {
        fraction += std::chrono::seconds{1};
        seconds -= std::chrono::seconds{1};
    }

    const auto time = Clock::to_time_t(TimePoint{seconds});
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << fraction.count()
           << 'Z';

    return output.str();
}

/** Reads the measurement boundaries and nominal rate from a client result CSV. */
std::expected<Measurement, std::string> readMeasurement(const std::filesystem::path &path) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read client result: {}", path.string()));
    }

    std::string line;

    if (!std::getline(input, line)) {
        return std::unexpected(std::format("empty client result: {}", path.string()));
    }

    const auto headings = parseCsvRow(line);
    const auto indexOf = [&](std::string_view name) -> std::optional<std::size_t> {
        const auto found = std::ranges::find(headings, name);

        if (found == headings.end()) {
            return std::nullopt;
        }

        return static_cast<std::size_t>(found - headings.begin());
    };
    const auto startIndex = indexOf("window_start_utc");
    const auto endIndex = indexOf("window_end_utc");
    const auto kindIndex = indexOf("sample_kind");
    const auto expectedIndex = indexOf("expected_per_batch");
    const auto nominalIndex = indexOf("nominal_events_per_second");

    if (!startIndex || !endIndex || !kindIndex || !expectedIndex) {
        return std::unexpected(std::format("client result has no required columns: {}", path.string()));
    }

    std::optional<Measurement> measurement;

    while (std::getline(input, line)) {
        const auto columns = parseCsvRow(line);
        auto maximumIndex = std::max({*startIndex, *endIndex, *kindIndex, *expectedIndex});

        if (nominalIndex) {
            maximumIndex = std::max(maximumIndex, *nominalIndex);
        }

        if (columns.size() <= maximumIndex || columns[*kindIndex] != "event") {
            continue;
        }

        auto start = parseUtcTimestamp(columns[*startIndex]);
        auto end = parseUtcTimestamp(columns[*endIndex]);
        auto expected = parseNumber(columns[*expectedIndex]);
        auto nominal = nominalIndex ? parseNumber(columns[*nominalIndex]) : expected;

        if (!start || !end || !expected || !nominal) {
            return std::unexpected(std::format("invalid event row in client result: {}", path.string()));
        }

        if (!measurement) {
            measurement = Measurement{*start, *end, *nominal};
        } else {
            measurement->end = *end;
        }
    }

    if (!measurement) {
        return std::unexpected(std::format("no event rows found in client result: {}", path.string()));
    }

    return *measurement;
}

/** Parses QD monitoring records associated with one benchmark process. */
std::expected<std::vector<MonitoringSample>, std::string>
readMonitoringLog(const std::filesystem::path &path, const std::string &profile, const std::string &process,
                  const Measurement &measurement, std::chrono::milliseconds monitoringPeriod) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("missing monitoring log: {}", path.string()));
    }

    const std::regex linePattern{R"(^[A-Z]\s+([0-9]{6})\s+([0-9]{6}\.[0-9]{3}).*?\{([^}]+)\}\s+(Subscription:.*)$)"};
    const std::regex readPattern{"(?:^|; )Read: (" + std::string{NUMBER_PATTERN} + ") Bps(?: \\(([^)]*)\\))?"};
    const std::regex writePattern{"(?:^|; )Write: (" + std::string{NUMBER_PATTERN} + ") Bps(?: \\(([^)]*)\\))?"};
    std::vector<MonitoringSample> result;
    std::optional<TimePoint> previousEnd;
    std::string line;

    while (std::getline(input, line)) {
        std::smatch match;

        if (!std::regex_search(line, match, linePattern)) {
            continue;
        }

        auto intervalEnd = parseLocalLogTimestamp(match[1].str(), match[2].str());

        if (!intervalEnd) {
            return std::unexpected(std::format("{} in {}", intervalEnd.error(), path.string()));
        }

        const auto intervalStart = previousEnd.value_or(*intervalEnd - monitoringPeriod);
        previousEnd = *intervalEnd;
        const auto stats = match[4].str();
        std::smatch read;
        std::smatch write;
        const auto hasRead = std::regex_search(stats, read, readPattern);
        const auto hasWrite = std::regex_search(stats, write, writePattern);
        const auto readDetails = hasRead ? read[2].str() : std::string{};
        const auto writeDetails = hasWrite ? write[2].str() : std::string{};
        MonitoringSample sample;
        sample.profile = profile;
        sample.process = process;
        sample.endpoint = match[3].str();
        sample.intervalStartUtc = formatUtc(intervalStart);
        sample.intervalEndUtc = formatUtc(*intervalEnd);
        sample.inMeasurement = intervalStart >= measurement.start && *intervalEnd <= measurement.end;
        sample.nominalEventsPerSecond = measurement.nominalEventsPerSecond;
        sample.subscription = findNumber(stats, "Subscription: (" + std::string{NUMBER_PATTERN} + ")");
        sample.sticky = findNumber(stats, "Sticky: (" + std::string{NUMBER_PATTERN} + ")");
        sample.storage = findNumber(stats, "Storage: (" + std::string{NUMBER_PATTERN} + ")");
        sample.buffer = findNumber(stats, "Buffer: (" + std::string{NUMBER_PATTERN} + ")");
        sample.dropped = findNumber(stats, "Dropped: (" + std::string{NUMBER_PATTERN} + ")");
        sample.readBps = hasRead ? parseNumber(read[1].str()).value_or(0) : std::optional<double>{};
        sample.readSubscriptionRps = findNumber(readDetails, "sub (" + std::string{NUMBER_PATTERN} + ") rps");
        sample.readDataRps = findNumber(readDetails, "data (" + std::string{NUMBER_PATTERN} + ") rps");
        sample.readDataLagUs = findNumber(readDetails, "lag (" + std::string{NUMBER_PATTERN} + ") us");
        sample.writeBps = hasWrite ? parseNumber(write[1].str()).value_or(0) : std::optional<double>{};
        sample.writeSubscriptionRps = findNumber(writeDetails, "sub (" + std::string{NUMBER_PATTERN} + ") rps");
        sample.writeDataRps = findNumber(writeDetails, "data (" + std::string{NUMBER_PATTERN} + ") rps");
        sample.writeDataLagUs = findNumber(writeDetails, "lag (" + std::string{NUMBER_PATTERN} + ") us");
        sample.rttUs = findNumber(stats, "(?:^|; )rtt (" + std::string{NUMBER_PATTERN} + ") us");
        sample.cpuPercent = findNumber(stats, "CPU: (" + std::string{NUMBER_PATTERN} + ")%");
        result.push_back(std::move(sample));
    }

    return result;
}

/** Writes a quoted and escaped string value to a CSV stream. */
void writeCsvValue(std::ostream &output, std::string_view value) {
    output << '"';

    for (const auto character : value) {
        if (character == '"') {
            output << '"';
        }

        output << character;
    }

    output << '"';
}

/** Writes a floating-point value to a CSV stream without losing precision. */
void writeCsvValue(std::ostream &output, double value) {
    std::ostringstream formatted;
    formatted << std::setprecision(17) << value;
    writeCsvValue(output, formatted.str());
}

/** Writes an optional floating-point value when it is present. */
void writeCsvValue(std::ostream &output, const std::optional<double> &value) {
    if (value) {
        writeCsvValue(output, *value);
    }
}

/** Writes one optionally comma-prefixed value to a CSV row. */
template <typename Value> void writeColumn(std::ostream &output, const Value &value, bool first = false) {
    if (!first) {
        output << ',';
    }

    writeCsvValue(output, value);
}

/** Opens an output file for replacement and reports failures without throwing. */
std::expected<void, std::string> openOutput(std::ofstream &output, const std::filesystem::path &path) {
    output.open(path, std::ios::trunc);

    if (!output) {
        return std::unexpected(std::format("cannot write output: {}", path.string()));
    }

    return {};
}

/** Reads whole-run rows from one latency summary. */
std::expected<std::vector<LatencyRunRow>, std::string> readLatencyTotals(const std::filesystem::path &path,
                                                                         const std::string &profile) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read latency summary: {}", path.string()));
    }

    std::string line;

    if (!std::getline(input, line)) {
        return std::unexpected(std::format("empty latency summary: {}", path.string()));
    }

    const auto headings = parseCsvRow(line);
    const auto indexOf = [&](const std::string_view name) -> std::optional<std::size_t> {
        const auto found = std::ranges::find(headings, name);

        return found == headings.end() ? std::nullopt
                                       : std::optional{static_cast<std::size_t>(found - headings.begin())};
    };
    const auto kindIndex = indexOf("sample_kind");
    const auto samplesIndex = indexOf("samples");
    const auto expectedIndex = indexOf("expected_per_batch");
    const auto nominalIndex = indexOf("nominal_events_per_second");
    const auto roleIndex = indexOf("endpoint_role");
    const auto eventsBatchLimitIndex = indexOf("events_batch_limit");
    const auto aggregationPeriodIndex = indexOf("aggregation_period_ms");
    const auto publishedIndex = indexOf("published");
    const auto deliveredIndex = indexOf("delivered");
    const auto listenerDeficitIndex = indexOf("listener_deficit").or_else([&] {
        return indexOf("not_delivered");
    });
    const auto listenerCoverageIndex = indexOf("listener_coverage").or_else([&] {
        return indexOf("delivery_ratio");
    });
    const auto excessIndex = indexOf("excess_events");
    const auto fullIndex = indexOf("full_publications");
    const auto partialIndex = indexOf("partial_publications");
    const auto emptyIndex = indexOf("empty_publications");
    const auto uncorrelatedIndex = indexOf("uncorrelated_events");
    const auto callbacksIndex = indexOf("callbacks");
    const auto clockIndex = indexOf("clock_anomalies");
    const auto missingIndex = indexOf("missing_batches");
    const auto pendingIndex = indexOf("pending_batches");
    const auto minIndex = indexOf("min_us");
    const auto meanIndex = indexOf("mean_us");
    const auto p50Index = indexOf("p50_us");
    const auto p90Index = indexOf("p90_us");
    const auto p95Index = indexOf("p95_us");
    const auto p99Index = indexOf("p99_us");
    const auto p999Index = indexOf("p999_us");
    const auto maxIndex = indexOf("max_us");
    const auto outliersIndex = indexOf("outliers");
    const std::array required{kindIndex,    samplesIndex, expectedIndex, callbacksIndex, clockIndex,
                              missingIndex, minIndex,     meanIndex,     p50Index,       p90Index,
                              p95Index,     p99Index,     p999Index,     maxIndex,       outliersIndex};

    if (std::ranges::any_of(required, [](const auto &index) {
            return !index;
        })) {
        return std::unexpected(std::format("latency summary has no comparison columns: {}", path.string()));
    }

    std::vector<LatencyRunRow> rows;
    const auto identity = parseBenchmarkProfile(profile);

    while (std::getline(input, line)) {
        const auto columns = parseCsvRow(line);

        if (columns.size() <= *kindIndex || !columns[*kindIndex].ends_with("-total")) {
            continue;
        }

        const auto number = [&](std::optional<std::size_t> index) -> std::expected<double, std::string> {
            if (!index || columns.size() <= *index) {
                return std::unexpected(std::format("missing value in latency summary: {}", path.string()));
            }

            return parseNumber(columns[*index]);
        };
        auto values =
            std::array{number(samplesIndex), number(expectedIndex), number(callbacksIndex), number(clockIndex),
                       number(missingIndex), number(minIndex),      number(meanIndex),      number(p50Index),
                       number(p90Index),     number(p95Index),      number(p99Index),       number(p999Index),
                       number(maxIndex),     number(outliersIndex)};

        for (const auto &value : values) {
            if (!value) {
                return std::unexpected(value.error());
            }
        }

        double pending = 0;

        if (pendingIndex) {
            auto value = number(pendingIndex);

            if (!value) {
                return std::unexpected(value.error());
            }

            pending = *value;
        }

        double nominal = *values[1];

        if (nominalIndex) {
            auto value = number(nominalIndex);

            if (!value) {
                return std::unexpected(value.error());
            }

            nominal = *value;
        }

        LatencyRunRow row{profile, identity, columns[*kindIndex], nominal, *values[0], *values[1]};
        row.callbacks = *values[2];
        row.clockAnomalies = *values[3];
        row.missingBatches = *values[4];
        row.pendingBatches = pending;
        row.minimumUs = *values[5];
        row.meanUs = *values[6];
        row.p50Us = *values[7];
        row.p90Us = *values[8];
        row.p95Us = *values[9];
        row.p99Us = *values[10];
        row.p999Us = *values[11];
        row.maximumUs = *values[12];
        row.outliers = *values[13];

        if (roleIndex && columns.size() > *roleIndex) {
            row.endpointRole = columns[*roleIndex];
        }

        if (eventsBatchLimitIndex && columns.size() > *eventsBatchLimitIndex) {
            row.eventsBatchLimit = columns[*eventsBatchLimitIndex];
        }

        const auto optionalNumber = [&](std::optional<std::size_t> index, double fallback = 0.0) {
            if (!index || columns.size() <= *index) {
                return fallback;
            }

            const auto value = parseNumber(columns[*index]);

            return value.value_or(fallback);
        };
        row.published = optionalNumber(publishedIndex, row.samples);
        row.delivered = optionalNumber(deliveredIndex, row.samples);
        row.listenerDeficit = optionalNumber(listenerDeficitIndex);
        row.listenerCoverage =
            optionalNumber(listenerCoverageIndex, row.published ? row.delivered / row.published : 0.0);
        row.aggregationPeriodMs = optionalNumber(aggregationPeriodIndex);
        row.excessEvents = optionalNumber(excessIndex);
        row.fullPublications = optionalNumber(fullIndex);
        row.partialPublications = optionalNumber(partialIndex);
        row.emptyPublications = optionalNumber(emptyIndex);
        row.uncorrelatedEvents = optionalNumber(uncorrelatedIndex);
        rows.push_back(std::move(row));
    }

    const auto batch = std::ranges::find(rows, "batch-total", &LatencyRunRow::sampleKind);
    const auto batches = batch == rows.end() ? 0.0 : batch->samples;

    for (auto &row : rows) {
        if (row.sampleKind == "batch-total") {
            row.nominalEventsPerSecond = 0;
        }

        const auto exactDelivery = row.sampleKind == "batch-total" || row.endpointRole == "feed" ||
                                   (row.listenerDeficit == 0 && row.excessEvents == 0);
        row.integrityOk = batches > 0 && row.clockAnomalies == 0 && row.missingBatches == 0 &&
                          row.pendingBatches == 0 && exactDelivery;
    }

    return rows;
}

/** Reads all source-time method rows produced by one full Graal client execution. */
std::expected<std::vector<SourceTimeMethodRunRow>, std::string> readSourceTimeMethods(const std::filesystem::path &path,
                                                                                      const std::string &profile) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read source-time methods: {}", path.string()));
    }

    std::string line;

    if (!std::getline(input, line)) {
        return std::unexpected(std::format("empty source-time methods file: {}", path.string()));
    }

    const auto headings = parseCsvRow(line);
    const auto indexOf = [&](std::string_view name) -> std::optional<std::size_t> {
        const auto found = std::ranges::find(headings, name);

        return found == headings.end() ? std::nullopt
                                       : std::optional{static_cast<std::size_t>(found - headings.begin())};
    };
    const auto methodIndex = indexOf("method");
    const auto scopeIndex = indexOf("monotonic_scope");
    const auto resolutionIndex = indexOf("timestamp_resolution");
    const auto observationsIndex = indexOf("observations");
    const auto acceptedIndex = indexOf("accepted");
    const auto rejectedIndex = indexOf("rejected");
    const auto acceptanceIndex = indexOf("acceptance_ratio");
    const auto negativeIndex = indexOf("negative_clamped");
    const auto minimumIndex = indexOf("min_us");
    const auto meanIndex = indexOf("mean_us");
    const auto p50Index = indexOf("p50_us");
    const auto p90Index = indexOf("p90_us");
    const auto p95Index = indexOf("p95_us");
    const auto p99Index = indexOf("p99_us");
    const auto p999Index = indexOf("p999_us");
    const auto maximumIndex = indexOf("max_us");
    const auto outliersIndex = indexOf("outliers");
    const std::array required{methodIndex,   scopeIndex,      resolutionIndex, observationsIndex, acceptedIndex,
                              rejectedIndex, acceptanceIndex, negativeIndex,   minimumIndex,      meanIndex,
                              p50Index,      p90Index,        p95Index,        p99Index,          p999Index,
                              maximumIndex,  outliersIndex};

    if (std::ranges::any_of(required, [](const auto &index) {
            return !index;
        })) {
        return std::unexpected(std::format("source-time methods file has no comparison columns: {}", path.string()));
    }

    const auto identity = parseBenchmarkProfile(profile);
    std::vector<SourceTimeMethodRunRow> rows;

    while (std::getline(input, line)) {
        const auto columns = parseCsvRow(line);

        if (columns.empty()) {
            continue;
        }

        const auto number = [&](std::size_t index) -> std::expected<double, std::string> {
            if (columns.size() <= index) {
                return std::unexpected(std::format("missing source-time value in: {}", path.string()));
            }

            return parseNumber(columns[index]);
        };
        const std::array numericIndices{*observationsIndex, *acceptedIndex, *rejectedIndex, *acceptanceIndex,
                                        *negativeIndex,     *minimumIndex,  *meanIndex,     *p50Index,
                                        *p90Index,          *p95Index,      *p99Index,      *p999Index,
                                        *maximumIndex,      *outliersIndex};
        std::array<double, numericIndices.size()> values{};

        for (std::size_t i = 0; i < numericIndices.size(); ++i) {
            auto value = number(numericIndices[i]);

            if (!value) {
                return std::unexpected(value.error());
            }

            values[i] = *value;
        }

        if (columns.size() <= std::max({*methodIndex, *scopeIndex, *resolutionIndex})) {
            return std::unexpected(std::format("missing source-time label in: {}", path.string()));
        }

        rows.push_back({profile, identity, columns[*methodIndex], columns[*scopeIndex], columns[*resolutionIndex],
                        values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
                        values[8], values[9], values[10], values[11], values[12], values[13]});
    }

    if (rows.empty()) {
        return std::unexpected(std::format("source-time methods file has no data rows: {}", path.string()));
    }

    return rows;
}

/** Reads the whole-run delivery counters produced by one delivery-only client execution. */
std::expected<DeliveryRunRow, std::string> readDelivery(const std::filesystem::path &path, const std::string &profile) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read delivery result: {}", path.string()));
    }

    std::string headingsLine;
    std::string valuesLine;

    if (!std::getline(input, headingsLine) || !std::getline(input, valuesLine)) {
        return std::unexpected(std::format("incomplete delivery result: {}", path.string()));
    }

    const auto headings = parseCsvRow(headingsLine);
    const auto values = parseCsvRow(valuesLine);
    const auto indexOf = [&](std::string_view name) -> std::expected<std::size_t, std::string> {
        const auto found = std::ranges::find(headings, name);

        if (found == headings.end()) {
            return std::unexpected(std::format("delivery result has no {} column: {}", name, path.string()));
        }

        const auto index = static_cast<std::size_t>(found - headings.begin());

        if (index >= values.size()) {
            return std::unexpected(std::format("delivery result has no {} value: {}", name, path.string()));
        }

        return index;
    };
    const auto number = [&](std::string_view name) -> std::expected<double, std::string> {
        const auto index = indexOf(name);

        if (!index) {
            return std::unexpected(index.error());
        }

        return parseNumber(values[*index]);
    };
    const auto optionalNumber = [&](std::string_view name) -> std::expected<double, std::string> {
        const auto found = std::ranges::find(headings, name);

        if (found == headings.end()) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const auto index = static_cast<std::size_t>(found - headings.begin());

        if (index >= values.size()) {
            return std::unexpected(std::format("delivery result has no {} value: {}", name, path.string()));
        }

        return parseNumber(values[index]);
    };
    const auto nominal = number("nominal_events_per_second");
    const auto callbacks = number("callbacks");
    const auto recurring = number("recurring_events");
    const auto quotes = number("quote");
    const auto trades = number("trade");
    const auto tradeEths = number("trade_eth");
    const auto summaries = number("summary");
    const auto profiles = number("profiles");
    const auto maximumDataCount = number("maximum_data_count");
    const auto actualRate = number("actual_events_per_second");
    const auto cpuCorePercent = optionalNumber("cpu_core_percent");
    const auto cpuHostPercent = optionalNumber("cpu_host_percent");
    const auto rssMeanBytes = optionalNumber("rss_mean_bytes");
    const auto rssMaximumBytes = optionalNumber("rss_maximum_bytes");
    const auto resourceSamples = optionalNumber("resource_samples");
    const auto implementationFound = std::ranges::find(headings, "implementation");
    const auto implementationIndex = static_cast<std::size_t>(implementationFound - headings.begin());

    if (implementationFound != headings.end() && implementationIndex >= values.size()) {
        return std::unexpected(std::format("delivery result has no implementation value: {}", path.string()));
    }

    const auto implementation =
        implementationFound == headings.end() ? std::string{"legacy"} : values[implementationIndex];
    const auto contractIndex = indexOf("contract");

    for (const auto *value :
         {&nominal, &callbacks, &recurring, &quotes, &trades, &tradeEths, &summaries, &profiles, &maximumDataCount,
          &actualRate, &cpuCorePercent, &cpuHostPercent, &rssMeanBytes, &rssMaximumBytes, &resourceSamples}) {
        if (!*value) {
            return std::unexpected(value->error());
        }
    }

    if (!contractIndex) {
        return std::unexpected(contractIndex.error());
    }

    return DeliveryRunRow{profile,
                          parseBenchmarkProfile(profile),
                          *nominal,
                          *callbacks,
                          *recurring,
                          *quotes,
                          *trades,
                          *tradeEths,
                          *summaries,
                          *profiles,
                          *maximumDataCount,
                          *actualRate,
                          *cpuCorePercent,
                          *cpuHostPercent,
                          *rssMeanBytes,
                          *rssMaximumBytes,
                          *resourceSamples,
                          implementation,
                          values[*contractIndex]};
}

/** Reads optional process resource statistics from one benchmark client log. */
std::expected<std::optional<ClientResourceRunRow>, std::string> readClientResources(const std::filesystem::path &path,
                                                                                    const std::string &profile) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read client log: {}", path.string()));
    }

    const std::string contents{std::istreambuf_iterator<char>{input}, {}};
    const std::regex resourcePattern{
        R"((Client resources|Graal delivery resources|Legacy resources): cpu-core=([-+]?[0-9]+(?:\.[0-9]+)?)% cpu-host=([-+]?[0-9]+(?:\.[0-9]+)?)% rss-mean=([-+]?[0-9]+(?:\.[0-9]+)?) MiB rss-maximum=([-+]?[0-9]+(?:\.[0-9]+)?) MiB samples=([0-9]+))"};
    std::smatch match;

    if (!std::regex_search(contents, match, resourcePattern)) {
        return std::nullopt;
    }

    std::array<std::expected<double, std::string>, 5> values{parseNumber(match[2].str()), parseNumber(match[3].str()),
                                                             parseNumber(match[4].str()), parseNumber(match[5].str()),
                                                             parseNumber(match[6].str())};

    for (const auto &value : values) {
        if (!value) {
            return std::unexpected(
                std::format("invalid client resource statistics in {}: {}", path.string(), value.error()));
        }
    }

    const auto clientWorkload = match[1].str() == "Client resources"           ? std::string{"graal/full"}
                                : match[1].str() == "Graal delivery resources" ? std::string{"graal/delivery-only"}
                                                                               : std::string{"legacy/delivery-only"};

    return ClientResourceRunRow{
        profile,    parseBenchmarkProfile(profile), clientWorkload,           0,         *values[0],
        *values[1], *values[2] * 1'048'576.0,       *values[3] * 1'048'576.0, *values[4]};
}

/** Reads the TimeAndSale snapshot and live-cutover counters produced by one client execution. */
std::expected<TimeSeriesRunRow, std::string> readTimeSeries(const std::filesystem::path &path,
                                                            const std::string &profile) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read time-series result: {}", path.string()));
    }

    std::string headingsLine;
    std::string valuesLine;

    if (!std::getline(input, headingsLine) || !std::getline(input, valuesLine)) {
        return std::unexpected(std::format("incomplete time-series result: {}", path.string()));
    }

    const auto headings = parseCsvRow(headingsLine);
    const auto columns = parseCsvRow(valuesLine);
    const auto number = [&](std::string_view name) -> std::expected<double, std::string> {
        const auto found = std::ranges::find(headings, name);

        if (found == headings.end()) {
            return std::unexpected(std::format("time-series result has no {} column: {}", name, path.string()));
        }

        const auto index = static_cast<std::size_t>(found - headings.begin());

        if (index >= columns.size()) {
            return std::unexpected(std::format("time-series result has no {} value: {}", name, path.string()));
        }

        return parseNumber(columns[index]);
    };
    const auto optionalNumber = [&](std::string_view name) -> std::expected<double, std::string> {
        const auto found = std::ranges::find(headings, name);

        if (found == headings.end()) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const auto index = static_cast<std::size_t>(found - headings.begin());

        if (index >= columns.size()) {
            return std::unexpected(std::format("time-series result has no {} value: {}", name, path.string()));
        }

        return parseNumber(columns[index]);
    };
    constexpr std::array names{"requested_symbols",     "observed_symbols",
                               "completed_symbols",     "snapshot_events",
                               "snapshot_callbacks",    "snapshot_begin",
                               "snapshot_end",          "snapshot_snip",
                               "snapshot_remove",       "duplicate_indices",
                               "premature_live_events", "live_events",
                               "clock_anomalies",       "first_event_delay_ms",
                               "snapshot_duration_ms",  "first_live_relative_to_global_completion_ms",
                               "live_latency_samples",  "live_latency_mean_us",
                               "live_latency_p50_us",   "live_latency_p90_us",
                               "live_latency_p99_us",   "live_latency_p999_us",
                               "live_latency_max_us"};
    std::array<double, names.size()> values{};

    for (std::size_t i = 0; i < names.size(); ++i) {
        auto value = number(names[i]);

        if (!value && i == 15) {
            value = number("first_live_after_snapshot_ms");
        }

        if (!value) {
            return std::unexpected(value.error());
        }

        values[i] = *value;
    }

    TimeSeriesRunRow row{profile, parseBenchmarkProfile(profile)};
    row.requestedSymbols = values[0];
    row.observedSymbols = values[1];
    row.completedSymbols = values[2];
    row.snapshotEvents = values[3];
    row.snapshotCallbacks = values[4];
    row.snapshotBegins = values[5];
    row.snapshotEnds = values[6];
    row.snapshotSnips = values[7];
    row.snapshotRemovals = values[8];
    row.duplicateIndices = values[9];
    row.prematureLiveEvents = values[10];
    row.liveEvents = values[11];
    row.clockAnomalies = values[12];
    row.firstEventDelayMs = values[13];
    row.snapshotDurationMs = values[14];
    row.firstLiveRelativeToGlobalCompletionMs = values[15];
    row.liveLatencySamples = values[16];
    row.liveLatencyMeanUs = values[17];
    row.liveLatencyP50Us = values[18];
    row.liveLatencyP90Us = values[19];
    row.liveLatencyP99Us = values[20];
    row.liveLatencyP999Us = values[21];
    row.liveLatencyMaximumUs = values[22];
    const auto unsubscribedAfterSnapshot = optionalNumber("unsubscribed_after_snapshot");
    const auto cpuCorePercent = optionalNumber("cpu_core_percent");
    const auto cpuHostPercent = optionalNumber("cpu_host_percent");
    const auto rssMeanBytes = optionalNumber("rss_mean_bytes");
    const auto rssMaximumBytes = optionalNumber("rss_maximum_bytes");
    const auto resourceSamples = optionalNumber("resource_samples");

    for (const auto *value : {&unsubscribedAfterSnapshot, &cpuCorePercent, &cpuHostPercent, &rssMeanBytes,
                              &rssMaximumBytes, &resourceSamples}) {
        if (!*value) {
            return std::unexpected(value->error());
        }
    }

    row.unsubscribedAfterSnapshot = std::isfinite(*unsubscribedAfterSnapshot) && *unsubscribedAfterSnapshot != 0;
    row.cpuCorePercent = *cpuCorePercent;
    row.cpuHostPercent = *cpuHostPercent;
    row.rssMeanBytes = *rssMeanBytes;
    row.rssMaximumBytes = *rssMaximumBytes;
    row.resourceSamples = *resourceSamples;
    row.integrityOk = row.requestedSymbols > 0 && row.completedSymbols == row.requestedSymbols &&
                      row.snapshotBegins == row.requestedSymbols &&
                      row.snapshotEnds + row.snapshotSnips == row.requestedSymbols && row.duplicateIndices == 0 &&
                      row.prematureLiveEvents == 0 && row.clockAnomalies == 0 &&
                      (row.unsubscribedAfterSnapshot || row.liveLatencySamples > 0);

    return row;
}

/** Reads before/during/after ticker measurements produced by an in-measurement HISTORY subscription. */
std::expected<std::vector<SnapshotOverlapRunRow>, std::string> readSnapshotOverlap(const std::filesystem::path &path,
                                                                                   const std::string &profile) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read snapshot-overlap result: {}", path.string()));
    }

    std::string line;

    if (!std::getline(input, line)) {
        return std::unexpected(std::format("empty snapshot-overlap result: {}", path.string()));
    }

    const auto headings = parseCsvRow(line);
    const auto indexOf = [&](std::string_view name) -> std::expected<std::size_t, std::string> {
        const auto found = std::ranges::find(headings, name);

        if (found == headings.end()) {
            return std::unexpected(std::format("snapshot-overlap result has no {} column: {}", name, path.string()));
        }

        return static_cast<std::size_t>(found - headings.begin());
    };
    constexpr std::array numericNames{"duration_ms",
                                      "samples",
                                      "published",
                                      "delivered",
                                      "listener_deficit",
                                      "listener_coverage",
                                      "excess_events",
                                      "callbacks",
                                      "clock_anomalies",
                                      "missing_batches",
                                      "pending_batches",
                                      "min_us",
                                      "mean_us",
                                      "p50_us",
                                      "p90_us",
                                      "p95_us",
                                      "p99_us",
                                      "p999_us",
                                      "max_us",
                                      "cpu_core_percent",
                                      "cpu_host_percent",
                                      "rss_mean_bytes",
                                      "rss_maximum_bytes",
                                      "resource_samples"};
    const auto phaseIndex = indexOf("phase");
    const auto kindIndex = indexOf("sample_kind");
    std::array<std::size_t, numericNames.size()> numericIndices{};

    if (!phaseIndex || !kindIndex) {
        return std::unexpected(!phaseIndex ? phaseIndex.error() : kindIndex.error());
    }

    for (std::size_t i = 0; i < numericNames.size(); ++i) {
        const auto index = indexOf(numericNames[i]);

        if (!index) {
            return std::unexpected(index.error());
        }

        numericIndices[i] = *index;
    }

    std::vector<SnapshotOverlapRunRow> result;
    const auto identity = parseBenchmarkProfile(profile);

    while (std::getline(input, line)) {
        const auto columns = parseCsvRow(line);
        const auto maximumIndex = std::max({*phaseIndex, *kindIndex, *std::ranges::max_element(numericIndices)});

        if (columns.size() <= maximumIndex) {
            return std::unexpected(std::format("incomplete snapshot-overlap row: {}", path.string()));
        }

        std::array<double, numericNames.size()> values{};

        for (std::size_t i = 0; i < numericIndices.size(); ++i) {
            const auto value = parseNumber(columns[numericIndices[i]]);

            if (!value) {
                return std::unexpected(
                    std::format("invalid {} in snapshot-overlap result: {}", numericNames[i], path.string()));
            }

            values[i] = *value;
        }

        SnapshotOverlapRunRow row{profile, identity, columns[*phaseIndex], columns[*kindIndex]};
        row.durationMs = values[0];
        row.samples = values[1];
        row.published = values[2];
        row.delivered = values[3];
        row.listenerDeficit = values[4];
        row.listenerCoverage = values[5];
        row.excessEvents = values[6];
        row.callbacks = values[7];
        row.clockAnomalies = values[8];
        row.missingBatches = values[9];
        row.pendingBatches = values[10];
        row.minimumUs = values[11];
        row.meanUs = values[12];
        row.p50Us = values[13];
        row.p90Us = values[14];
        row.p95Us = values[15];
        row.p99Us = values[16];
        row.p999Us = values[17];
        row.maximumUs = values[18];
        row.cpuCorePercent = values[19];
        row.cpuHostPercent = values[20];
        row.rssMeanBytes = values[21];
        row.rssMaximumBytes = values[22];
        row.resourceSamples = values[23];
        result.push_back(std::move(row));
    }

    if (result.empty()) {
        return std::unexpected(std::format("snapshot-overlap result has no data rows: {}", path.string()));
    }

    return result;
}

/** Writes the common header used by comparison CSV files. */
void writeComparisonHeader(std::ostream &output) {
    output << "\"scenario\",\"category\",\"metric\",\"runs\",\"minimum\",\"median\",\"maximum\"\n";
}

/** Writes one comparison row to a CSV stream. */
void writeComparisonRow(std::ostream &output, const ComparisonRow &row) {
    writeColumn(output, row.scenario, true);
    writeColumn(output, row.category);
    writeColumn(output, row.metric);
    writeColumn(output, static_cast<double>(row.comparison.runs));
    writeColumn(output, row.comparison.minimum);
    writeColumn(output, row.comparison.median);
    writeColumn(output, row.comparison.maximum);
    output << '\n';
}
} // namespace

BenchmarkProfile parseBenchmarkProfile(std::string_view profile) {
    static const std::regex repetitionPattern{R"(^(.+)-r([0-9]+)$)"};
    std::match_results<std::string_view::const_iterator> match;

    if (!std::regex_match(profile.begin(), profile.end(), match, repetitionPattern)) {
        return {std::string{profile}, 1};
    }

    try {
        const auto repetition = std::stoull(std::string{match[2].first, match[2].second});

        return {std::string{match[1].first, match[1].second}, static_cast<std::size_t>(repetition)};
    } catch (const std::exception &) {
        return {std::string{profile}, 1};
    }
}

RunComparison compareRuns(std::vector<double> values) {
    if (values.empty()) {
        return {};
    }

    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    const auto median = values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;

    return {values.size(), values.front(), median, values.back()};
}

std::expected<MonitoringAnalysis, std::string> analyzeMonitoringDirectory(const std::filesystem::path &runDirectory,
                                                                          std::chrono::milliseconds monitoringPeriod) {
    if (monitoringPeriod <= std::chrono::milliseconds::zero()) {
        return std::unexpected("monitoring period must be positive");
    }

    std::error_code error;

    if (!std::filesystem::is_directory(runDirectory, error)) {
        return std::unexpected(std::format("benchmark directory does not exist: {}", runDirectory.string()));
    }

    std::vector<std::filesystem::path> results;

    for (const auto &entry : std::filesystem::directory_iterator{runDirectory}) {
        const auto filename = entry.path().filename().string();

        if (entry.is_regular_file() && ((filename.ends_with(SUMMARY_SUFFIX) && filename != "monitoring-summary.csv") ||
                                        filename.ends_with(DELIVERY_SUFFIX))) {
            results.push_back(entry.path());
        }
    }

    std::ranges::sort(results);

    if (results.empty()) {
        return std::unexpected(std::format("no client result files found in: {}", runDirectory.string()));
    }

    MonitoringAnalysis analysis;

    for (const auto &result : results) {
        const auto filename = result.filename().string();
        const auto suffix = filename.ends_with(DELIVERY_SUFFIX) ? DELIVERY_SUFFIX : SUMMARY_SUFFIX;
        const auto profile = filename.substr(0, filename.size() - suffix.size());
        auto measurement = readMeasurement(result);

        if (!measurement) {
            return std::unexpected(measurement.error());
        }

        for (const auto &process : {std::string{"server"}, std::string{"client"}}) {
            auto samples = readMonitoringLog(runDirectory / (profile + "-" + process + ".log"), profile, process,
                                             *measurement, monitoringPeriod);

            if (!samples) {
                return std::unexpected(samples.error());
            }

            analysis.samples.insert(analysis.samples.end(), std::make_move_iterator(samples->begin()),
                                    std::make_move_iterator(samples->end()));
        }
    }

    if (analysis.samples.empty()) {
        return std::unexpected("no QD monitoring records were found");
    }

    std::ranges::sort(analysis.samples, {}, [](const MonitoringSample &sample) {
        return std::tuple{sample.profile, sample.process, sample.intervalEndUtc};
    });
    std::map<std::pair<std::string, std::string>, std::vector<const MonitoringSample *>> groups;

    for (const auto &sample : analysis.samples) {
        if (sample.inMeasurement) {
            groups[{sample.profile, sample.process}].push_back(&sample);
        }
    }

    for (const auto &[key, samples] : groups) {
        for (const auto &metric : METRICS) {
            std::vector<double> values;

            for (const auto *sample : samples) {
                if (const auto &value = sample->*(metric.member); value && !std::isnan(*value)) {
                    values.push_back(*value);
                }
            }

            if (values.empty()) {
                continue;
            }

            const auto [minimum, maximum] = std::ranges::minmax(values);
            const auto sum = std::accumulate(values.begin(), values.end(), 0.0);
            analysis.aggregates.push_back(MonitoringAggregate{
                key.first, key.second, samples.front()->nominalEventsPerSecond, std::string{metric.name}, values.size(),
                minimum, sum / static_cast<double>(values.size()), maximum, sum});
        }
    }

    return analysis;
}

std::expected<void, std::string> writeMonitoringAnalysis(const std::filesystem::path &runDirectory,
                                                         const MonitoringAnalysis &analysis) {
    std::ofstream samples;
    const auto samplesPath = runDirectory / "monitoring.csv";

    if (auto opened = openOutput(samples, samplesPath); !opened) {
        return opened;
    }

    samples << "\"profile\",\"process\",\"endpoint\",\"interval_start_utc\",\"interval_end_utc\","
               "\"in_measurement\",\"nominal_events_per_second\",\"subscription\",\"sticky\",\"storage\","
               "\"buffer\",\"dropped\",\"read_bps\",\"read_subscription_rps\",\"read_data_rps\","
               "\"read_data_lag_us\",\"write_bps\",\"write_subscription_rps\",\"write_data_rps\","
               "\"write_data_lag_us\",\"rtt_us\",\"cpu_percent\"\n";

    for (const auto &sample : analysis.samples) {
        writeColumn(samples, sample.profile, true);
        writeColumn(samples, sample.process);
        writeColumn(samples, sample.endpoint);
        writeColumn(samples, sample.intervalStartUtc);
        writeColumn(samples, sample.intervalEndUtc);
        writeColumn(samples, sample.inMeasurement ? std::string_view{"True"} : std::string_view{"False"});
        writeColumn(samples, sample.nominalEventsPerSecond);
        writeColumn(samples, sample.subscription);
        writeColumn(samples, sample.sticky);
        writeColumn(samples, sample.storage);
        writeColumn(samples, sample.buffer);
        writeColumn(samples, sample.dropped);
        writeColumn(samples, sample.readBps);
        writeColumn(samples, sample.readSubscriptionRps);
        writeColumn(samples, sample.readDataRps);
        writeColumn(samples, sample.readDataLagUs);
        writeColumn(samples, sample.writeBps);
        writeColumn(samples, sample.writeSubscriptionRps);
        writeColumn(samples, sample.writeDataRps);
        writeColumn(samples, sample.writeDataLagUs);
        writeColumn(samples, sample.rttUs);
        writeColumn(samples, sample.cpuPercent);
        samples << '\n';
    }

    std::ofstream aggregates;
    const auto aggregatesPath = runDirectory / "monitoring-summary.csv";

    if (auto opened = openOutput(aggregates, aggregatesPath); !opened) {
        return opened;
    }

    aggregates << "\"profile\",\"process\",\"nominal_events_per_second\",\"metric\",\"samples\","
                  "\"minimum\",\"mean\",\"maximum\",\"sum\"\n";

    for (const auto &aggregate : analysis.aggregates) {
        writeColumn(aggregates, aggregate.profile, true);
        writeColumn(aggregates, aggregate.process);
        writeColumn(aggregates, aggregate.nominalEventsPerSecond);
        writeColumn(aggregates, aggregate.metric);
        writeColumn(aggregates, static_cast<double>(aggregate.samples));
        writeColumn(aggregates, aggregate.minimum);
        writeColumn(aggregates, aggregate.mean);
        writeColumn(aggregates, aggregate.maximum);
        writeColumn(aggregates, aggregate.sum);
        aggregates << '\n';
    }

    return {};
}

std::expected<void, std::string> writeBenchmarkComparison(const std::filesystem::path &runDirectory,
                                                          const MonitoringAnalysis &analysis) {
    std::vector<LatencyRunRow> latencyRows;
    std::vector<DeliveryRunRow> deliveryRows;
    std::vector<ClientResourceRunRow> clientResourceRows;
    std::vector<SourceTimeMethodRunRow> sourceTimeMethodRows;
    std::vector<TimeSeriesRunRow> timeSeriesRows;
    std::vector<SnapshotOverlapRunRow> snapshotOverlapRows;

    for (const auto &entry : std::filesystem::directory_iterator{runDirectory}) {
        const auto filename = entry.path().filename().string();

        if (entry.is_regular_file() && filename.ends_with(SOURCE_TIME_METHODS_SUFFIX)) {
            const auto profile = filename.substr(0, filename.size() - SOURCE_TIME_METHODS_SUFFIX.size());
            auto rows = readSourceTimeMethods(entry.path(), profile);

            if (!rows) {
                return std::unexpected(rows.error());
            }

            sourceTimeMethodRows.insert(sourceTimeMethodRows.end(), std::make_move_iterator(rows->begin()),
                                        std::make_move_iterator(rows->end()));

            continue;
        }

        if (entry.is_regular_file() && filename.ends_with(DELIVERY_SUFFIX)) {
            const auto profile = filename.substr(0, filename.size() - DELIVERY_SUFFIX.size());
            auto row = readDelivery(entry.path(), profile);

            if (!row) {
                return std::unexpected(row.error());
            }

            deliveryRows.push_back(std::move(*row));

            continue;
        }

        if (entry.is_regular_file() && filename.ends_with(TIME_SERIES_SUFFIX)) {
            const auto profile = filename.substr(0, filename.size() - TIME_SERIES_SUFFIX.size());
            auto row = readTimeSeries(entry.path(), profile);

            if (!row) {
                return std::unexpected(row.error());
            }

            timeSeriesRows.push_back(std::move(*row));

            continue;
        }

        if (entry.is_regular_file() && filename.ends_with(SNAPSHOT_OVERLAP_SUFFIX)) {
            const auto profile = filename.substr(0, filename.size() - SNAPSHOT_OVERLAP_SUFFIX.size());
            auto rows = readSnapshotOverlap(entry.path(), profile);

            if (!rows) {
                return std::unexpected(rows.error());
            }

            snapshotOverlapRows.insert(snapshotOverlapRows.end(), std::make_move_iterator(rows->begin()),
                                       std::make_move_iterator(rows->end()));

            continue;
        }

        if (!entry.is_regular_file() || !filename.ends_with(SUMMARY_SUFFIX) || filename == "monitoring-summary.csv") {
            continue;
        }

        const auto profile = filename.substr(0, filename.size() - SUMMARY_SUFFIX.size());
        auto rows = readLatencyTotals(entry.path(), profile);

        if (!rows) {
            return std::unexpected(rows.error());
        }

        const auto event = std::ranges::find(*rows, "event-total", &LatencyRunRow::sampleKind);
        const auto nominal = event == rows->end() ? 0 : event->nominalEventsPerSecond;

        for (auto &row : *rows) {
            row.nominalEventsPerSecond = nominal;
            latencyRows.push_back(std::move(row));
        }
    }

    for (const auto &entry : std::filesystem::directory_iterator{runDirectory}) {
        const auto filename = entry.path().filename().string();

        if (!entry.is_regular_file() || !filename.ends_with(CLIENT_LOG_SUFFIX)) {
            continue;
        }

        const auto profile = filename.substr(0, filename.size() - CLIENT_LOG_SUFFIX.size());
        auto row = readClientResources(entry.path(), profile);

        if (!row) {
            return std::unexpected(row.error());
        }

        if (!*row) {
            continue;
        }

        const auto delivery = std::ranges::find(deliveryRows, profile, &DeliveryRunRow::profile);
        const auto latency = std::ranges::find_if(latencyRows, [&](const LatencyRunRow &candidate) {
            return candidate.profile == profile && candidate.nominalEventsPerSecond > 0;
        });

        if (delivery != deliveryRows.end()) {
            (*row)->nominalEventsPerSecond = delivery->nominalEventsPerSecond;
        } else if (latency != latencyRows.end()) {
            (*row)->nominalEventsPerSecond = latency->nominalEventsPerSecond;
        }

        clientResourceRows.push_back(std::move(**row));
    }

    std::ranges::sort(latencyRows, {}, [](const LatencyRunRow &row) {
        return std::tuple{row.nominalEventsPerSecond, row.identity.scenario, row.identity.repetition, row.sampleKind};
    });
    std::ranges::sort(deliveryRows, {}, [](const DeliveryRunRow &row) {
        return std::tuple{row.nominalEventsPerSecond, row.identity.scenario, row.identity.repetition};
    });
    std::ranges::sort(clientResourceRows, {}, [](const ClientResourceRunRow &row) {
        return std::tuple{row.nominalEventsPerSecond, row.identity.scenario, row.identity.repetition};
    });
    std::ranges::sort(sourceTimeMethodRows, {}, [](const SourceTimeMethodRunRow &row) {
        return std::tuple{row.identity.scenario, row.identity.repetition, row.method};
    });
    std::ranges::sort(timeSeriesRows, {}, [](const TimeSeriesRunRow &row) {
        return std::tuple{row.identity.scenario, row.identity.repetition};
    });
    std::ranges::sort(snapshotOverlapRows, {}, [](const SnapshotOverlapRunRow &row) {
        return std::tuple{row.identity.scenario, row.identity.repetition, row.phase, row.sampleKind};
    });

    std::ofstream deliveryRuns;

    if (auto opened = openOutput(deliveryRuns, runDirectory / "delivery-runs.csv"); !opened) {
        return opened;
    }

    deliveryRuns << "\"profile\",\"scenario\",\"repetition\",\"nominal_events_per_second\",\"callbacks\","
                    "\"recurring_events\",\"quote\",\"trade\",\"trade_eth\",\"summary\",\"profiles\","
                    "\"maximum_data_count\",\"actual_events_per_second\",\"cpu_core_percent\","
                    "\"cpu_host_percent\",\"rss_mean_bytes\",\"rss_maximum_bytes\",\"resource_samples\","
                    "\"implementation\",\"contract\"\n";

    for (const auto &row : deliveryRows) {
        writeColumn(deliveryRuns, row.profile, true);
        writeColumn(deliveryRuns, row.identity.scenario);
        writeColumn(deliveryRuns, static_cast<double>(row.identity.repetition));
        writeColumn(deliveryRuns, row.nominalEventsPerSecond);
        writeColumn(deliveryRuns, row.callbacks);
        writeColumn(deliveryRuns, row.recurringEvents);
        writeColumn(deliveryRuns, row.quotes);
        writeColumn(deliveryRuns, row.trades);
        writeColumn(deliveryRuns, row.tradeEths);
        writeColumn(deliveryRuns, row.summaries);
        writeColumn(deliveryRuns, row.profiles);
        writeColumn(deliveryRuns, row.maximumDataCount);
        writeColumn(deliveryRuns, row.actualEventsPerSecond);
        writeColumn(deliveryRuns, row.cpuCorePercent);
        writeColumn(deliveryRuns, row.cpuHostPercent);
        writeColumn(deliveryRuns, row.rssMeanBytes);
        writeColumn(deliveryRuns, row.rssMaximumBytes);
        writeColumn(deliveryRuns, row.resourceSamples);
        writeColumn(deliveryRuns, row.implementation);
        writeColumn(deliveryRuns, row.contract);
        deliveryRuns << '\n';
    }

    std::map<std::string, std::vector<const DeliveryRunRow *>> deliveryScenarios;

    for (const auto &row : deliveryRows) {
        deliveryScenarios[row.identity.scenario].push_back(&row);
    }

    std::ofstream deliveryComparison;

    if (auto opened = openOutput(deliveryComparison, runDirectory / "delivery-comparison.csv"); !opened) {
        return opened;
    }

    writeComparisonHeader(deliveryComparison);

    for (const auto &[scenario, rows] : deliveryScenarios) {
        for (const auto [name, member] : {std::pair{"actual_events_per_second", &DeliveryRunRow::actualEventsPerSecond},
                                          std::pair{"callbacks", &DeliveryRunRow::callbacks},
                                          std::pair{"maximum_data_count", &DeliveryRunRow::maximumDataCount},
                                          std::pair{"cpu_core_percent", &DeliveryRunRow::cpuCorePercent},
                                          std::pair{"cpu_host_percent", &DeliveryRunRow::cpuHostPercent},
                                          std::pair{"rss_mean_bytes", &DeliveryRunRow::rssMeanBytes},
                                          std::pair{"rss_maximum_bytes", &DeliveryRunRow::rssMaximumBytes}}) {
            std::vector<double> values;

            for (const auto *row : rows) {
                if (std::isfinite(row->*member)) {
                    values.push_back(row->*member);
                }
            }

            if (values.empty()) {
                continue;
            }

            writeComparisonRow(deliveryComparison, {scenario, std::format("{}-delivery", rows.front()->implementation),
                                                    name, compareRuns(std::move(values))});
        }
    }

    std::ofstream clientResourceRuns;

    if (auto opened = openOutput(clientResourceRuns, runDirectory / "client-resource-runs.csv"); !opened) {
        return opened;
    }

    clientResourceRuns << "\"profile\",\"scenario\",\"repetition\",\"client_workload\","
                          "\"nominal_events_per_second\",\"cpu_core_percent\",\"cpu_host_percent\","
                          "\"rss_mean_bytes\",\"rss_maximum_bytes\",\"samples\"\n";

    for (const auto &row : clientResourceRows) {
        writeColumn(clientResourceRuns, row.profile, true);
        writeColumn(clientResourceRuns, row.identity.scenario);
        writeColumn(clientResourceRuns, static_cast<double>(row.identity.repetition));
        writeColumn(clientResourceRuns, row.clientWorkload);
        writeColumn(clientResourceRuns, row.nominalEventsPerSecond);
        writeColumn(clientResourceRuns, row.cpuCorePercent);
        writeColumn(clientResourceRuns, row.cpuHostPercent);
        writeColumn(clientResourceRuns, row.rssMeanBytes);
        writeColumn(clientResourceRuns, row.rssMaximumBytes);
        writeColumn(clientResourceRuns, row.samples);
        clientResourceRuns << '\n';
    }

    std::map<std::string, std::vector<const ClientResourceRunRow *>> clientResourceScenarios;

    for (const auto &row : clientResourceRows) {
        clientResourceScenarios[row.identity.scenario].push_back(&row);
    }

    std::ofstream clientResourceComparison;

    if (auto opened = openOutput(clientResourceComparison, runDirectory / "client-resource-comparison.csv"); !opened) {
        return opened;
    }

    writeComparisonHeader(clientResourceComparison);

    for (const auto &[scenario, rows] : clientResourceScenarios) {
        for (const auto [name, member] : {
                 std::pair{"cpu_core_percent", &ClientResourceRunRow::cpuCorePercent},
                 std::pair{"cpu_host_percent", &ClientResourceRunRow::cpuHostPercent},
                 std::pair{"rss_mean_bytes", &ClientResourceRunRow::rssMeanBytes},
                 std::pair{"rss_maximum_bytes", &ClientResourceRunRow::rssMaximumBytes},
             }) {
            std::vector<double> values;

            for (const auto *row : rows) {
                values.push_back(row->*member);
            }

            writeComparisonRow(clientResourceComparison,
                               {scenario, rows.front()->clientWorkload, name, compareRuns(std::move(values))});
        }
    }

    std::ofstream timeSeriesRuns;

    if (auto opened = openOutput(timeSeriesRuns, runDirectory / "time-series-runs.csv"); !opened) {
        return opened;
    }

    timeSeriesRuns << "\"profile\",\"scenario\",\"repetition\",\"requested_symbols\",\"observed_symbols\","
                      "\"completed_symbols\",\"snapshot_events\",\"snapshot_callbacks\",\"snapshot_begin\","
                      "\"snapshot_end\",\"snapshot_snip\",\"snapshot_remove\",\"duplicate_indices\","
                      "\"premature_live_events\",\"live_events\",\"clock_anomalies\",\"first_event_delay_ms\","
                      "\"snapshot_duration_ms\",\"first_live_relative_to_global_completion_ms\","
                      "\"live_latency_samples\","
                      "\"live_latency_mean_us\",\"live_latency_p50_us\",\"live_latency_p90_us\","
                      "\"live_latency_p99_us\",\"live_latency_p999_us\",\"live_latency_max_us\","
                      "\"cpu_core_percent\",\"cpu_host_percent\",\"rss_mean_bytes\",\"rss_maximum_bytes\","
                      "\"resource_samples\",\"unsubscribed_after_snapshot\",\"integrity_ok\"\n";

    for (const auto &row : timeSeriesRows) {
        writeColumn(timeSeriesRuns, row.profile, true);
        writeColumn(timeSeriesRuns, row.identity.scenario);
        writeColumn(timeSeriesRuns, static_cast<double>(row.identity.repetition));

        for (const auto value : {row.requestedSymbols,     row.observedSymbols,
                                 row.completedSymbols,     row.snapshotEvents,
                                 row.snapshotCallbacks,    row.snapshotBegins,
                                 row.snapshotEnds,         row.snapshotSnips,
                                 row.snapshotRemovals,     row.duplicateIndices,
                                 row.prematureLiveEvents,  row.liveEvents,
                                 row.clockAnomalies,       row.firstEventDelayMs,
                                 row.snapshotDurationMs,   row.firstLiveRelativeToGlobalCompletionMs,
                                 row.liveLatencySamples,   row.liveLatencyMeanUs,
                                 row.liveLatencyP50Us,     row.liveLatencyP90Us,
                                 row.liveLatencyP99Us,     row.liveLatencyP999Us,
                                 row.liveLatencyMaximumUs, row.cpuCorePercent,
                                 row.cpuHostPercent,       row.rssMeanBytes,
                                 row.rssMaximumBytes,      row.resourceSamples}) {
            writeColumn(timeSeriesRuns, value);
        }

        writeColumn(timeSeriesRuns,
                    row.unsubscribedAfterSnapshot ? std::string_view{"True"} : std::string_view{"False"});
        writeColumn(timeSeriesRuns, row.integrityOk ? std::string_view{"True"} : std::string_view{"False"});
        timeSeriesRuns << '\n';
    }

    std::map<std::string, std::vector<const TimeSeriesRunRow *>> timeSeriesScenarios;

    for (const auto &row : timeSeriesRows) {
        timeSeriesScenarios[row.identity.scenario].push_back(&row);
    }

    std::ofstream timeSeriesComparison;

    if (auto opened = openOutput(timeSeriesComparison, runDirectory / "time-series-comparison.csv"); !opened) {
        return opened;
    }

    writeComparisonHeader(timeSeriesComparison);

    constexpr std::array timeSeriesMetrics{std::pair{"snapshot_events", &TimeSeriesRunRow::snapshotEvents},
                                           std::pair{"snapshot_callbacks", &TimeSeriesRunRow::snapshotCallbacks},
                                           std::pair{"snapshot_duration_ms", &TimeSeriesRunRow::snapshotDurationMs},
                                           std::pair{"first_live_relative_to_global_completion_ms",
                                                     &TimeSeriesRunRow::firstLiveRelativeToGlobalCompletionMs},
                                           std::pair{"live_events", &TimeSeriesRunRow::liveEvents},
                                           std::pair{"live_latency_p50_us", &TimeSeriesRunRow::liveLatencyP50Us},
                                           std::pair{"live_latency_p99_us", &TimeSeriesRunRow::liveLatencyP99Us},
                                           std::pair{"live_latency_p999_us", &TimeSeriesRunRow::liveLatencyP999Us},
                                           std::pair{"live_latency_max_us", &TimeSeriesRunRow::liveLatencyMaximumUs},
                                           std::pair{"cpu_core_percent", &TimeSeriesRunRow::cpuCorePercent},
                                           std::pair{"cpu_host_percent", &TimeSeriesRunRow::cpuHostPercent},
                                           std::pair{"rss_mean_bytes", &TimeSeriesRunRow::rssMeanBytes},
                                           std::pair{"rss_maximum_bytes", &TimeSeriesRunRow::rssMaximumBytes}};

    for (const auto &[scenario, rows] : timeSeriesScenarios) {
        for (const auto &[name, member] : timeSeriesMetrics) {
            std::vector<double> values;

            for (const auto *row : rows) {
                if (std::isfinite(row->*member)) {
                    values.push_back(row->*member);
                }
            }

            if (values.empty()) {
                continue;
            }

            writeComparisonRow(timeSeriesComparison, {scenario, "time-series", name, compareRuns(std::move(values))});
        }
    }

    std::ofstream snapshotOverlapRuns;

    if (auto opened = openOutput(snapshotOverlapRuns, runDirectory / "snapshot-overlap-runs.csv"); !opened) {
        return opened;
    }

    snapshotOverlapRuns << "\"profile\",\"scenario\",\"repetition\",\"phase\",\"sample_kind\",\"duration_ms\","
                           "\"samples\",\"published\",\"delivered\",\"listener_deficit\",\"listener_coverage\","
                           "\"excess_events\",\"callbacks\",\"clock_anomalies\",\"missing_batches\","
                           "\"pending_batches\",\"min_us\",\"mean_us\",\"p50_us\",\"p90_us\",\"p95_us\","
                           "\"p99_us\",\"p999_us\",\"max_us\",\"cpu_core_percent\",\"cpu_host_percent\","
                           "\"rss_mean_bytes\",\"rss_maximum_bytes\",\"resource_samples\"\n";

    for (const auto &row : snapshotOverlapRows) {
        writeColumn(snapshotOverlapRuns, row.profile, true);
        writeColumn(snapshotOverlapRuns, row.identity.scenario);
        writeColumn(snapshotOverlapRuns, static_cast<double>(row.identity.repetition));
        writeColumn(snapshotOverlapRuns, row.phase);
        writeColumn(snapshotOverlapRuns, row.sampleKind);

        for (const auto value : {row.durationMs,     row.samples,         row.published,
                                 row.delivered,      row.listenerDeficit, row.listenerCoverage,
                                 row.excessEvents,   row.callbacks,       row.clockAnomalies,
                                 row.missingBatches, row.pendingBatches,  row.minimumUs,
                                 row.meanUs,         row.p50Us,           row.p90Us,
                                 row.p95Us,          row.p99Us,           row.p999Us,
                                 row.maximumUs,      row.cpuCorePercent,  row.cpuHostPercent,
                                 row.rssMeanBytes,   row.rssMaximumBytes, row.resourceSamples}) {
            writeColumn(snapshotOverlapRuns, value);
        }

        snapshotOverlapRuns << '\n';
    }

    /** Maps a snapshot-overlap comparison name to its run-row member. */
    struct SnapshotOverlapMetric {
        std::string_view name;
        double SnapshotOverlapRunRow::*member;
    };

    constexpr std::array snapshotOverlapMetrics{
        SnapshotOverlapMetric{"duration_ms", &SnapshotOverlapRunRow::durationMs},
        SnapshotOverlapMetric{"samples", &SnapshotOverlapRunRow::samples},
        SnapshotOverlapMetric{"listener_coverage", &SnapshotOverlapRunRow::listenerCoverage},
        SnapshotOverlapMetric{"listener_deficit", &SnapshotOverlapRunRow::listenerDeficit},
        SnapshotOverlapMetric{"mean_us", &SnapshotOverlapRunRow::meanUs},
        SnapshotOverlapMetric{"p50_us", &SnapshotOverlapRunRow::p50Us},
        SnapshotOverlapMetric{"p90_us", &SnapshotOverlapRunRow::p90Us},
        SnapshotOverlapMetric{"p99_us", &SnapshotOverlapRunRow::p99Us},
        SnapshotOverlapMetric{"p999_us", &SnapshotOverlapRunRow::p999Us},
        SnapshotOverlapMetric{"max_us", &SnapshotOverlapRunRow::maximumUs},
        SnapshotOverlapMetric{"cpu_core_percent", &SnapshotOverlapRunRow::cpuCorePercent},
        SnapshotOverlapMetric{"cpu_host_percent", &SnapshotOverlapRunRow::cpuHostPercent},
        SnapshotOverlapMetric{"rss_mean_bytes", &SnapshotOverlapRunRow::rssMeanBytes},
        SnapshotOverlapMetric{"rss_maximum_bytes", &SnapshotOverlapRunRow::rssMaximumBytes}};
    std::map<std::tuple<std::string, std::string, std::string>, std::vector<const SnapshotOverlapRunRow *>>
        snapshotOverlapGroups;

    for (const auto &row : snapshotOverlapRows) {
        snapshotOverlapGroups[{row.identity.scenario, row.phase, row.sampleKind}].push_back(&row);
    }

    std::ofstream snapshotOverlapComparison;

    if (auto opened = openOutput(snapshotOverlapComparison, runDirectory / "snapshot-overlap-comparison.csv");
        !opened) {
        return opened;
    }

    writeComparisonHeader(snapshotOverlapComparison);

    for (const auto &[key, rows] : snapshotOverlapGroups) {
        for (const auto &metric : snapshotOverlapMetrics) {
            std::vector<double> values;

            for (const auto *row : rows) {
                values.push_back(row->*(metric.member));
            }

            writeComparisonRow(snapshotOverlapComparison,
                               {std::get<0>(key), std::format("{}:{}", std::get<1>(key), std::get<2>(key)),
                                std::string{metric.name}, compareRuns(std::move(values))});
        }
    }

    std::ofstream runs;

    if (auto opened = openOutput(runs, runDirectory / "latency-runs.csv"); !opened) {
        return opened;
    }

    runs << "\"profile\",\"scenario\",\"repetition\",\"nominal_events_per_second\",\"sample_kind\","
            "\"samples\",\"expected_per_batch\",\"endpoint_role\",\"events_batch_limit\","
            "\"aggregation_period_ms\",\"published\",\"delivered\","
            "\"listener_deficit\",\"listener_coverage\",\"excess_events\",\"full_publications\","
            "\"partial_publications\",\"empty_publications\",\"uncorrelated_events\",\"callbacks\","
            "\"clock_anomalies\",\"missing_batches\","
            "\"pending_batches\",\"integrity_ok\",\"min_us\",\"mean_us\",\"p50_us\",\"p90_us\","
            "\"p95_us\",\"p99_us\",\"p999_us\",\"max_us\",\"outliers\"\n";

    for (const auto &row : latencyRows) {
        writeColumn(runs, row.profile, true);
        writeColumn(runs, row.identity.scenario);
        writeColumn(runs, static_cast<double>(row.identity.repetition));
        writeColumn(runs, row.nominalEventsPerSecond);
        writeColumn(runs, row.sampleKind);
        writeColumn(runs, row.samples);
        writeColumn(runs, row.expectedPerBatch);
        writeColumn(runs, row.endpointRole);
        writeColumn(runs, row.eventsBatchLimit);
        writeColumn(runs, row.aggregationPeriodMs);
        writeColumn(runs, row.published);
        writeColumn(runs, row.delivered);
        writeColumn(runs, row.listenerDeficit);
        writeColumn(runs, row.listenerCoverage);
        writeColumn(runs, row.excessEvents);
        writeColumn(runs, row.fullPublications);
        writeColumn(runs, row.partialPublications);
        writeColumn(runs, row.emptyPublications);
        writeColumn(runs, row.uncorrelatedEvents);
        writeColumn(runs, row.callbacks);
        writeColumn(runs, row.clockAnomalies);
        writeColumn(runs, row.missingBatches);
        writeColumn(runs, row.pendingBatches);
        writeColumn(runs, row.integrityOk ? std::string_view{"True"} : std::string_view{"False"});
        writeColumn(runs, row.minimumUs);
        writeColumn(runs, row.meanUs);
        writeColumn(runs, row.p50Us);
        writeColumn(runs, row.p90Us);
        writeColumn(runs, row.p95Us);
        writeColumn(runs, row.p99Us);
        writeColumn(runs, row.p999Us);
        writeColumn(runs, row.maximumUs);
        writeColumn(runs, row.outliers);
        runs << '\n';
    }

    /** Maps a report metric name to its source whole-run latency member. */
    struct LatencyMetric {
        std::string_view name;
        double LatencyRunRow::*member;
    };

    constexpr std::array latencyMetrics{LatencyMetric{"mean_us", &LatencyRunRow::meanUs},
                                        LatencyMetric{"p50_us", &LatencyRunRow::p50Us},
                                        LatencyMetric{"p90_us", &LatencyRunRow::p90Us},
                                        LatencyMetric{"p95_us", &LatencyRunRow::p95Us},
                                        LatencyMetric{"p99_us", &LatencyRunRow::p99Us},
                                        LatencyMetric{"p999_us", &LatencyRunRow::p999Us},
                                        LatencyMetric{"max_us", &LatencyRunRow::maximumUs},
                                        LatencyMetric{"listener_coverage", &LatencyRunRow::listenerCoverage},
                                        LatencyMetric{"listener_deficit", &LatencyRunRow::listenerDeficit}};
    std::map<std::pair<std::string, std::string>, std::vector<const LatencyRunRow *>> latencyGroups;

    for (const auto &row : latencyRows) {
        latencyGroups[{row.identity.scenario, row.sampleKind}].push_back(&row);
    }

    std::vector<ComparisonRow> latencyComparisons;

    for (const auto &[key, rows] : latencyGroups) {
        for (const auto &metric : latencyMetrics) {
            std::vector<double> values;

            for (const auto *row : rows) {
                values.push_back(row->*(metric.member));
            }

            latencyComparisons.push_back({key.first, key.second, std::string{metric.name}, compareRuns(values)});
        }
    }

    std::ofstream latencyComparison;

    if (auto opened = openOutput(latencyComparison, runDirectory / "latency-comparison.csv"); !opened) {
        return opened;
    }

    writeComparisonHeader(latencyComparison);

    for (const auto &row : latencyComparisons) {
        writeComparisonRow(latencyComparison, row);
    }

    std::ofstream sourceTimeMethodRuns;

    if (auto opened = openOutput(sourceTimeMethodRuns, runDirectory / "source-time-method-runs.csv"); !opened) {
        return opened;
    }

    sourceTimeMethodRuns << "\"profile\",\"scenario\",\"repetition\",\"method\",\"monotonic_scope\","
                            "\"timestamp_resolution\",\"observations\",\"accepted\",\"rejected\","
                            "\"acceptance_ratio\",\"negative_clamped\",\"min_us\",\"mean_us\",\"p50_us\","
                            "\"p90_us\",\"p95_us\",\"p99_us\",\"p999_us\",\"max_us\",\"outliers\"\n";

    for (const auto &row : sourceTimeMethodRows) {
        writeColumn(sourceTimeMethodRuns, row.profile, true);
        writeColumn(sourceTimeMethodRuns, row.identity.scenario);
        writeColumn(sourceTimeMethodRuns, static_cast<double>(row.identity.repetition));
        writeColumn(sourceTimeMethodRuns, row.method);
        writeColumn(sourceTimeMethodRuns, row.monotonicScope);
        writeColumn(sourceTimeMethodRuns, row.timestampResolution);
        writeColumn(sourceTimeMethodRuns, row.observations);
        writeColumn(sourceTimeMethodRuns, row.accepted);
        writeColumn(sourceTimeMethodRuns, row.rejected);
        writeColumn(sourceTimeMethodRuns, row.acceptanceRatio);
        writeColumn(sourceTimeMethodRuns, row.negativeClamped);
        writeColumn(sourceTimeMethodRuns, row.minimumUs);
        writeColumn(sourceTimeMethodRuns, row.meanUs);
        writeColumn(sourceTimeMethodRuns, row.p50Us);
        writeColumn(sourceTimeMethodRuns, row.p90Us);
        writeColumn(sourceTimeMethodRuns, row.p95Us);
        writeColumn(sourceTimeMethodRuns, row.p99Us);
        writeColumn(sourceTimeMethodRuns, row.p999Us);
        writeColumn(sourceTimeMethodRuns, row.maximumUs);
        writeColumn(sourceTimeMethodRuns, row.outliers);
        sourceTimeMethodRuns << '\n';
    }

    /** Maps a report metric name to its source-time run member. */
    struct SourceTimeMetric {
        std::string_view name;
        double SourceTimeMethodRunRow::*member;
    };

    constexpr std::array sourceTimeMetrics{
        SourceTimeMetric{"observations", &SourceTimeMethodRunRow::observations},
        SourceTimeMetric{"accepted", &SourceTimeMethodRunRow::accepted},
        SourceTimeMetric{"rejected", &SourceTimeMethodRunRow::rejected},
        SourceTimeMetric{"acceptance_ratio", &SourceTimeMethodRunRow::acceptanceRatio},
        SourceTimeMetric{"p50_us", &SourceTimeMethodRunRow::p50Us},
        SourceTimeMetric{"p99_us", &SourceTimeMethodRunRow::p99Us},
        SourceTimeMetric{"p999_us", &SourceTimeMethodRunRow::p999Us},
        SourceTimeMetric{"max_us", &SourceTimeMethodRunRow::maximumUs}};
    std::map<std::pair<std::string, std::string>, std::vector<const SourceTimeMethodRunRow *>> sourceTimeGroups;

    for (const auto &row : sourceTimeMethodRows) {
        sourceTimeGroups[{row.identity.scenario, row.method}].push_back(&row);
    }

    std::ofstream sourceTimeMethodComparison;

    if (auto opened = openOutput(sourceTimeMethodComparison, runDirectory / "source-time-method-comparison.csv");
        !opened) {
        return opened;
    }

    writeComparisonHeader(sourceTimeMethodComparison);

    for (const auto &[key, rows] : sourceTimeGroups) {
        for (const auto &metric : sourceTimeMetrics) {
            std::vector<double> values;

            for (const auto *row : rows) {
                values.push_back(row->*(metric.member));
            }

            writeComparisonRow(sourceTimeMethodComparison,
                               {key.first, key.second, std::string{metric.name}, compareRuns(std::move(values))});
        }
    }

    std::map<std::tuple<std::string, std::string, std::string>, std::vector<double>> monitoringGroups;

    for (const auto &aggregate : analysis.aggregates) {
        const auto identity = parseBenchmarkProfile(aggregate.profile);
        const auto isCounter = aggregate.metric == "dropped";
        const auto isHighWaterMark = aggregate.metric == "buffer";
        const auto metric = aggregate.metric + (isCounter ? "_run_sum" : isHighWaterMark ? "_run_max" : "_run_mean");
        const auto value = isCounter ? aggregate.sum : isHighWaterMark ? aggregate.maximum : aggregate.mean;
        monitoringGroups[{identity.scenario, aggregate.process, metric}].push_back(value);
    }

    std::vector<ComparisonRow> monitoringComparisons;

    for (auto &[key, values] : monitoringGroups) {
        monitoringComparisons.push_back(
            {std::get<0>(key), std::get<1>(key), std::get<2>(key), compareRuns(std::move(values))});
    }

    std::ofstream monitoringComparison;

    if (auto opened = openOutput(monitoringComparison, runDirectory / "monitoring-comparison.csv"); !opened) {
        return opened;
    }

    writeComparisonHeader(monitoringComparison);

    for (const auto &row : monitoringComparisons) {
        writeComparisonRow(monitoringComparison, row);
    }

    const auto experiment = readExperimentMetadata(runDirectory);

    if (!experiment) {
        return std::unexpected{experiment.error()};
    }

    std::ofstream report;

    if (auto opened = openOutput(report, runDirectory / "REPORT.md"); !opened) {
        return opened;
    }

    writeExperimentDefinition(report, *experiment);

    if (!sourceTimeGroups.empty()) {
        report << R"(## Source-time measurement methods

Each method is applied to the same Trade and TradeETH callbacks at millisecond timestamp resolution. Values are
medians across repetitions. `customer-global-strict` uses one last-seen timestamp across all event types and symbols;
`per-series-strict` keeps independent state for each event-kind and symbol pair.

| Scenario | Method | Runs | Observations | Accepted | Acceptance | p50 | p99 | p99.9 | Maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
)";

        for (const auto &[key, rows] : sourceTimeGroups) {
            const auto collect = [&](double SourceTimeMethodRunRow::*member) {
                std::vector<double> values;

                for (const auto *row : rows) {
                    values.push_back(row->*member);
                }

                return compareRuns(std::move(values));
            };
            const auto observations = collect(&SourceTimeMethodRunRow::observations);
            const auto accepted = collect(&SourceTimeMethodRunRow::accepted);
            const auto acceptance = collect(&SourceTimeMethodRunRow::acceptanceRatio);
            const auto p50 = collect(&SourceTimeMethodRunRow::p50Us);
            const auto p99 = collect(&SourceTimeMethodRunRow::p99Us);
            const auto p999 = collect(&SourceTimeMethodRunRow::p999Us);
            const auto maximum = collect(&SourceTimeMethodRunRow::maximumUs);

            report << std::format("| {} | {} | {} | {:.0f} | {:.0f} | {:.4f}% | {:.3f} ms | {:.3f} ms | "
                                  "{:.3f} ms | {:.3f} ms |\n",
                                  key.first, key.second, rows.size(), observations.median, accepted.median,
                                  acceptance.median * 100.0, p50.median / 1000.0, p99.median / 1000.0,
                                  p999.median / 1000.0, maximum.median / 1000.0);
        }

        report << R"(

The global strict filter is order-dependent and can retain at most one observation for a group of events carrying
the same millisecond source timestamp. Its latency distribution therefore describes the surviving timestamp maxima,
not the full Trade/TradeETH population. Compare its acceptance ratio with marker-correlated listener coverage before
using its percentile values to assess API behavior.

)";
    }

    if (!deliveryScenarios.empty()) {
        report << R"(## Delivery-only client results

These values describe callback delivery and callback shape without timestamp correlation or per-event sample
allocation. They are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Client | Contract / role | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum callback batch | CPU, one-core basis | CPU, host basis | RSS mean / maximum |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
)";

        for (const auto &[scenario, rows] : deliveryScenarios) {
            std::vector<double> rates;
            std::vector<double> callbacks;
            std::vector<double> maximumDataCounts;
            std::vector<double> cpuCorePercents;
            std::vector<double> cpuHostPercents;
            std::vector<double> rssMeans;
            std::vector<double> rssMaximums;

            for (const auto *row : rows) {
                rates.push_back(row->actualEventsPerSecond);
                callbacks.push_back(row->callbacks);
                maximumDataCounts.push_back(row->maximumDataCount);

                if (std::isfinite(row->cpuCorePercent)) {
                    cpuCorePercents.push_back(row->cpuCorePercent);
                    cpuHostPercents.push_back(row->cpuHostPercent);
                    rssMeans.push_back(row->rssMeanBytes);
                    rssMaximums.push_back(row->rssMaximumBytes);
                }
            }

            const auto rate = compareRuns(std::move(rates));
            const auto callback = compareRuns(std::move(callbacks));
            const auto maximumDataCount = compareRuns(std::move(maximumDataCounts));
            const auto cpuCore = compareRuns(std::move(cpuCorePercents));
            const auto cpuHost = compareRuns(std::move(cpuHostPercents));
            const auto rssMean = compareRuns(std::move(rssMeans));
            const auto rssMaximum = compareRuns(std::move(rssMaximums));
            const auto *representative = rows.front();
            const auto resourceText = [&](const RunComparison &value, double divisor, std::string_view suffix) {
                return value.runs ? std::format("{:.3f}{}", value.median / divisor, suffix) : std::string{"n/a"};
            };
            report << std::format(
                "| {} | {} | {} | {} | {:.3f} | {:.3f} ({:.3f}–{:.3f}) | {:.3f} | {:.0f} | {} | {} | {} / {} |\n",
                scenario, representative->implementation, representative->contract, rate.runs,
                representative->nominalEventsPerSecond, rate.median, rate.minimum, rate.maximum, callback.median,
                maximumDataCount.maximum, resourceText(cpuCore, 1.0, "%"), resourceText(cpuHost, 1.0, "%"),
                resourceText(rssMean, 1'048'576.0, " MiB"), resourceText(rssMaximum, 1'048'576.0, " MiB"));
        }

        report << R"(

With the default legacy contract, the C API expands each base-symbol Quote, Trade, TradeETH, and Summary subscription
into the composite plus 26 regional symbols. A task can publish a configured subset of those regional record keys
while keeping its recurring event rate fixed. CPU uses both a one-core basis and a host-normalized basis; RSS is
sampled by the cross-platform `ttldtor/Process` library during the measurement interval. The Graal delivery-only
client preserves native vector callback sizes and performs only the type inspection needed for per-type counters.)"
               << "\n\n";
    }

    if (!clientResourceScenarios.empty()) {
        report << R"(## Client process resources

These measurements cover the complete client process during its measurement interval. Full Graal includes marker
correlation, latency-sample retention, window statistics, and outlier reporting; delivery-only clients count callback
delivery without retaining per-event latency samples. CPU is normalized both to one logical core and to the host.

| Scenario | Client workload | Runs | Nominal events/s | CPU, one-core median (range) | CPU, host median | RSS mean median | RSS maximum median |
|---|---|---:|---:|---:|---:|---:|---:|
)";

        for (const auto &[scenario, rows] : clientResourceScenarios) {
            const auto collect = [&](double ClientResourceRunRow::*member) {
                std::vector<double> values;

                for (const auto *row : rows) {
                    values.push_back(row->*member);
                }

                return compareRuns(std::move(values));
            };
            const auto cpuCore = collect(&ClientResourceRunRow::cpuCorePercent);
            const auto cpuHost = collect(&ClientResourceRunRow::cpuHostPercent);
            const auto rssMean = collect(&ClientResourceRunRow::rssMeanBytes);
            const auto rssMaximum = collect(&ClientResourceRunRow::rssMaximumBytes);
            const auto *representative = rows.front();

            report << std::format("| {} | {} | {} | {:.3f} | {:.3f}% ({:.3f}–{:.3f}%) | {:.3f}% | {:.3f} MiB | "
                                  "{:.3f} MiB |\n",
                                  scenario, representative->clientWorkload, cpuCore.runs,
                                  representative->nominalEventsPerSecond, cpuCore.median, cpuCore.minimum,
                                  cpuCore.maximum, cpuHost.median, rssMean.median / 1'048'576.0,
                                  rssMaximum.median / 1'048'576.0);
        }

        report << R"(

RSS means are arithmetic means of periodic samples; maximum RSS is less sensitive to different sample counts. The
full client calculates each window's distributions synchronously, so at high load that benchmark work can extend a
nominal window while listener callbacks continue. Use QD read/write rates and exact STREAM_FEED integrity alongside
these process measurements.)"
               << "\n\n";
    }

    if (!timeSeriesScenarios.empty()) {
        report << R"(## TimeAndSale snapshot and live cutover

The client adds a separate HISTORY subscription for the configured retained interval. Snapshot and live values are
medians across repetitions; the event range is the minimum and maximum complete snapshot size.

| Scenario | Runs | After snapshot | Completed symbols | Snapshot events median (range) | Snapshot callbacks | Snapshot duration | SNIP | First live vs global completion | Live p99 | CPU, one-core basis | RSS mean / maximum | Integrity |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
)";

        for (const auto &[scenario, rows] : timeSeriesScenarios) {
            const auto collect = [&](double TimeSeriesRunRow::*member) {
                std::vector<double> values;

                for (const auto *row : rows) {
                    if (std::isfinite(row->*member)) {
                        values.push_back(row->*member);
                    }
                }

                return compareRuns(std::move(values));
            };
            const auto completed = collect(&TimeSeriesRunRow::completedSymbols);
            const auto events = collect(&TimeSeriesRunRow::snapshotEvents);
            const auto callbacks = collect(&TimeSeriesRunRow::snapshotCallbacks);
            const auto duration = collect(&TimeSeriesRunRow::snapshotDurationMs);
            const auto snips = collect(&TimeSeriesRunRow::snapshotSnips);
            const auto cutover = collect(&TimeSeriesRunRow::firstLiveRelativeToGlobalCompletionMs);
            const auto liveP99 = collect(&TimeSeriesRunRow::liveLatencyP99Us);
            const auto cpuCore = collect(&TimeSeriesRunRow::cpuCorePercent);
            const auto rssMean = collect(&TimeSeriesRunRow::rssMeanBytes);
            const auto rssMaximum = collect(&TimeSeriesRunRow::rssMaximumBytes);
            const auto integrity = std::ranges::all_of(rows, [](const auto *row) {
                return row->integrityOk;
            });
            const auto removed = std::ranges::all_of(rows, [](const auto *row) {
                return row->unsubscribedAfterSnapshot;
            });
            const auto resourceText = [](const RunComparison &value, double divisor, std::string_view suffix) {
                return value.runs ? std::format("{:.3f}{}", value.median / divisor, suffix) : std::string{"n/a"};
            };

            report << std::format("| {} | {} | {} | {:.0f} | {:.0f} ({:.0f}–{:.0f}) | {:.0f} | {:.3f} ms | {:.0f} | "
                                  "{:.3f} ms | {:.3f} us | {} | {} / {} | {} |\n",
                                  scenario, rows.size(), removed ? "removed" : "retained", completed.median,
                                  events.median, events.minimum, events.maximum, callbacks.median, duration.median,
                                  snips.median, cutover.median, liveP99.median, resourceText(cpuCore, 1.0, "%"),
                                  resourceText(rssMean, 1'048'576.0, " MiB"),
                                  resourceText(rssMaximum, 1'048'576.0, " MiB"), integrity ? "OK" : "CHECK");
        }

        report << R"(

Integrity requires every requested symbol to complete with `SNAPSHOT_END` or `SNAPSHOT_SNIP`, no duplicate indices,
no live events before that symbol's snapshot completion, and no clock anomalies. A run that retains its TimeAndSale
subscription must also measure at least one live TimeAndSale event; a matched-recovery run may remove all symbols at
global snapshot completion.
`First live vs global completion` is negative when symbols that completed early start receiving live updates while
snapshots for other symbols are still in progress; this is valid per-symbol snapshot-to-live overlap. `SNAPSHOT_SNIP`
is reported separately because it is an expected bounded-history condition, not an integrity failure.
In a matched-recovery run, `Live p99` covers only per-symbol live-cutover events received before global snapshot
completion and removal; it is not a post-snapshot steady-state measurement.
CPU and RSS are sampled in the Graal client during the configured measurement interval. Delayed-subscription runs
include the snapshot, while subscriptions completed before measurement sample only the post-snapshot interval.)"
               << "\n\n";
    }

    if (!snapshotOverlapRows.empty()) {
        report << R"(## Ticker latency around TimeAndSale snapshot delivery

The client starts ticker measurement first, adds the TimeAndSale HISTORY subscription after the configured delay,
and partitions ticker observations into `before`, `during`, and `after` phases. Phase boundaries are detected by the
client at subscription and global snapshot completion. Values are medians across repetitions.

| Scenario | Phase | Runs | Duration | Listener coverage | Event p50 | Event p99 (range) | Event p99.9 | Event maximum | CPU, one-core basis | RSS mean / maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
)";
        std::map<std::pair<std::string, std::string>, std::vector<const SnapshotOverlapRunRow *>> phaseRows;

        for (const auto &row : snapshotOverlapRows) {
            if (row.sampleKind == "event") {
                phaseRows[{row.identity.scenario, row.phase}].push_back(&row);
            }
        }

        for (const auto &[key, rows] : phaseRows) {
            const auto collect = [&](double SnapshotOverlapRunRow::*member) {
                std::vector<double> values;

                for (const auto *row : rows) {
                    values.push_back(row->*member);
                }

                return compareRuns(std::move(values));
            };
            const auto duration = collect(&SnapshotOverlapRunRow::durationMs);
            const auto coverage = collect(&SnapshotOverlapRunRow::listenerCoverage);
            const auto p50 = collect(&SnapshotOverlapRunRow::p50Us);
            const auto p99 = collect(&SnapshotOverlapRunRow::p99Us);
            const auto p999 = collect(&SnapshotOverlapRunRow::p999Us);
            const auto maximum = collect(&SnapshotOverlapRunRow::maximumUs);
            const auto cpu = collect(&SnapshotOverlapRunRow::cpuCorePercent);
            const auto rssMean = collect(&SnapshotOverlapRunRow::rssMeanBytes);
            const auto rssMaximum = collect(&SnapshotOverlapRunRow::rssMaximumBytes);

            report << std::format("| {} | {} | {} | {:.3f} ms | {:.3f}% | {:.3f} us | {:.3f} "
                                  "({:.3f}–{:.3f}) us | {:.3f} us | {:.3f} us | {:.3f}% | {:.3f} / {:.3f} MiB |\n",
                                  key.first, key.second, rows.size(), duration.median, coverage.median * 100.0,
                                  p50.median, p99.median, p99.minimum, p99.maximum, p999.median, maximum.median,
                                  cpu.median, rssMean.median / 1'048'576.0, rssMaximum.median / 1'048'576.0);
        }

        report << R"(

Phase latency is based on marker-correlated Quote/Trade/TradeETH/Summary listener observations. Resource sampling is
performed separately in each phase; 100% CPU means one fully occupied logical core. A short `during` phase can have
few samples, so its range and the raw per-run CSV must be considered alongside the median. Phase delivery counters
can straddle a boundary when market events and their timestamp marker complete on opposite sides of it; whole-run
integrity and QD `Dropped` remain the authoritative loss checks.

)";
    }

    report << R"(## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
)";

    std::map<std::string, std::vector<const LatencyRunRow *>> scenarios;

    for (const auto &row : latencyRows) {
        scenarios[row.identity.scenario].push_back(&row);
    }

    for (const auto &[scenario, rows] : scenarios) {
        const auto collect = [&](std::string_view kind, double LatencyRunRow::*member) {
            std::vector<double> values;

            for (const auto *row : rows) {
                if (row->sampleKind == kind) {
                    values.push_back(row->*member / 1000.0);
                }
            }

            return compareRuns(std::move(values));
        };
        const auto p50 = collect("event-total", &LatencyRunRow::p50Us);
        const auto p99 = collect("event-total", &LatencyRunRow::p99Us);
        const auto p999 = collect("event-total", &LatencyRunRow::p999Us);
        const auto batchP99 = collect("batch-total", &LatencyRunRow::p99Us);
        const auto collectRaw = [&](double LatencyRunRow::*member) {
            std::vector<double> values;

            for (const auto *row : rows) {
                if (row->sampleKind == "event-total") {
                    values.push_back(row->*member);
                }
            }

            return compareRuns(std::move(values));
        };
        const auto listenerCoverage = collectRaw(&LatencyRunRow::listenerCoverage);
        const auto listenerDeficit = collectRaw(&LatencyRunRow::listenerDeficit);
        const auto eventRow = std::ranges::find_if(rows, [](const auto *row) {
            return row->sampleKind == "event-total";
        });
        const auto role = eventRow == rows.end() ? "unknown" : (*eventRow)->endpointRole;
        const auto eventsBatchLimit = eventRow == rows.end() ? "unknown" : (*eventRow)->eventsBatchLimit;
        const auto aggregationPeriodMs = eventRow == rows.end() ? 0.0 : (*eventRow)->aggregationPeriodMs;
        const auto integrity = std::ranges::all_of(rows, [](const auto *row) {
            return row->integrityOk;
        });
        const auto missing = rows.empty() ? 0.0 : rows.front()->missingBatches;
        const auto integrityText = integrity ? "OK" : missing > 0 ? "CHECK; missing batches reported" : "CHECK";
        report << "| " << scenario << " | " << role << " | " << eventsBatchLimit << " | " << std::fixed
               << std::setprecision(3) << aggregationPeriodMs << " ms | " << p50.runs << " | "
               << listenerCoverage.median * 100.0 << "% | " << listenerDeficit.median << " | " << p50.median << " | "
               << p99.median << " (" << p99.minimum << "–" << p99.maximum << ") | " << p999.median << " | "
               << batchP99.median << " | " << integrityText << " |\n";
    }

    report << R"(

`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage. Because FEED does not preserve publication boundaries, listener deficit and per-marker excess
are observations rather than integrity failures; STREAM_FEED still requires exact correlated delivery.
)";

    report << "\nGenerated files: `latency-runs.csv`, `latency-comparison.csv`, `source-time-method-runs.csv`, "
              "`source-time-method-comparison.csv`, `client-resource-runs.csv`, "
              "`client-resource-comparison.csv`, `time-series-runs.csv`, `time-series-comparison.csv`, "
              "`snapshot-overlap-runs.csv`, `snapshot-overlap-comparison.csv`, "
              "`delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and "
              "`monitoring-comparison.csv`.\n\n"
              "## Client monitoring\n\n"
              "The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run "
              "sum and buffer is the largest per-run high-water mark.\n\n"
              "| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |\n"
              "|---|---:|---:|---:|---:|---:|\n";

    std::map<std::string, bool> monitoringScenarios;

    for (const auto &[scenario, rows] : scenarios) {
        monitoringScenarios[scenario] = true;
    }

    for (const auto &[scenario, rows] : deliveryScenarios) {
        monitoringScenarios[scenario] = true;
    }

    for (const auto &[scenario, rows] : timeSeriesScenarios) {
        monitoringScenarios[scenario] = true;
    }

    for (const auto &row : snapshotOverlapRows) {
        monitoringScenarios[row.identity.scenario] = true;
    }

    const auto monitoringValue = [&](const std::string &scenario, std::string_view process, std::string_view metric) {
        const auto found = std::ranges::find_if(monitoringComparisons, [&](const ComparisonRow &row) {
            return row.scenario == scenario && row.category == process && row.metric == metric;
        });

        return found == monitoringComparisons.end() ? RunComparison{} : found->comparison;
    };
    const auto medianText = [](const RunComparison &value, double divisor = 1.0,
                               std::string_view suffix = std::string_view{}) {
        return value.runs ? std::format("{:.3f}{}", value.median / divisor, suffix) : std::string{"n/a"};
    };
    const auto maximumText = [](const RunComparison &value) {
        return value.runs ? std::format("{:.3f}", value.maximum) : std::string{"n/a"};
    };

    for (const auto &[scenario, unused] : monitoringScenarios) {
        const auto readRate = monitoringValue(scenario, "client", "read_data_rps_run_mean");
        const auto readLag = monitoringValue(scenario, "client", "read_data_lag_us_run_mean");
        const auto cpu = monitoringValue(scenario, "client", "cpu_percent_run_mean");
        const auto buffer = monitoringValue(scenario, "client", "buffer_run_max");
        const auto dropped = monitoringValue(scenario, "client", "dropped_run_sum");
        report << "| " << scenario << " | " << medianText(readRate) << " | " << medianText(readLag, 1000.0) << " | "
               << medianText(cpu, 1.0, "%") << " | " << maximumText(buffer) << " | " << maximumText(dropped) << " |\n";
    }

    report << R"(

## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
)";

    for (const auto &[scenario, unused] : monitoringScenarios) {
        const auto writeRate = monitoringValue(scenario, "server", "write_data_rps_run_mean");
        const auto writeLag = monitoringValue(scenario, "server", "write_data_lag_us_run_mean");
        const auto cpu = monitoringValue(scenario, "server", "cpu_percent_run_mean");
        const auto buffer = monitoringValue(scenario, "server", "buffer_run_max");
        const auto dropped = monitoringValue(scenario, "server", "dropped_run_sum");
        report << "| " << scenario << " | " << medianText(writeRate) << " | " << medianText(writeLag, 1000.0) << " | "
               << medianText(cpu, 1.0, "%") << " | " << maximumText(buffer) << " | " << maximumText(dropped) << " |\n";
    }

    report << R"(

`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
)";

    if (std::ranges::any_of(latencyRows,
                            [](const LatencyRunRow &row) {
                                return !row.integrityOk;
                            }) ||
        std::ranges::any_of(timeSeriesRows, [](const TimeSeriesRunRow &row) {
            return !row.integrityOk;
        })) {
        return std::unexpected("one or more benchmark runs failed latency integrity checks; see REPORT.md");
    }

    return {};
}

} // namespace latency
