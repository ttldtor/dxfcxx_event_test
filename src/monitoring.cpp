#include "latency/monitoring.hpp"

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
constexpr std::string_view NUMBER_PATTERN = R"([-+]?[0-9][0-9,]*(?:\.[0-9]+)?)";

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

struct Measurement {
    TimePoint start;
    TimePoint end;
    double nominalEventsPerSecond{};
};

struct MetricDefinition {
    std::string_view name;
    std::optional<double> MonitoringSample::*member;
};

struct LatencyRunRow {
    std::string profile;
    BenchmarkProfile identity;
    std::string sampleKind;
    double nominalEventsPerSecond{};
    double samples{};
    double expectedPerBatch{};
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

std::optional<double> findNumber(const std::string &text, const std::string &pattern) {
    std::smatch match;

    if (!std::regex_search(text, match, std::regex{pattern})) {
        return std::nullopt;
    }

    return parseNumber(match[1].str()).value_or(std::numeric_limits<double>::quiet_NaN());
}

std::time_t utcTime(std::tm *value) {
#ifdef _WIN32
    const auto result = _mkgmtime(value);
#else
    const auto result = timegm(value);
#endif

    return result;
}

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

std::expected<Measurement, std::string> readMeasurement(const std::filesystem::path &path) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected(std::format("cannot read latency summary: {}", path.string()));
    }

    std::string line;

    if (!std::getline(input, line)) {
        return std::unexpected(std::format("empty latency summary: {}", path.string()));
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
        return std::unexpected(std::format("latency summary has no required columns: {}", path.string()));
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
            return std::unexpected(std::format("invalid event row in latency summary: {}", path.string()));
        }

        if (!measurement) {
            measurement = Measurement{*start, *end, *nominal};
        } else {
            measurement->end = *end;
        }
    }

    if (!measurement) {
        return std::unexpected(std::format("no event windows found in latency summary: {}", path.string()));
    }

    return *measurement;
}

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

void writeCsvValue(std::ostream &output, double value) {
    std::ostringstream formatted;
    formatted << std::setprecision(17) << value;
    writeCsvValue(output, formatted.str());
}

void writeCsvValue(std::ostream &output, const std::optional<double> &value) {
    if (value) {
        writeCsvValue(output, *value);
    }
}

template <typename Value> void writeColumn(std::ostream &output, const Value &value, bool first = false) {
    if (!first) {
        output << ',';
    }

    writeCsvValue(output, value);
}

std::expected<void, std::string> openOutput(std::ofstream &output, const std::filesystem::path &path) {
    output.open(path, std::ios::trunc);

    if (!output) {
        return std::unexpected(std::format("cannot write output: {}", path.string()));
    }

    return {};
}

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

        rows.push_back(LatencyRunRow{profile, identity, columns[*kindIndex], nominal, *values[0], *values[1],
                                     *values[2], *values[3], *values[4], pending, *values[5], *values[6], *values[7],
                                     *values[8], *values[9], *values[10], *values[11], *values[12], *values[13]});
    }

    const auto batch = std::ranges::find(rows, "batch-total", &LatencyRunRow::sampleKind);
    const auto batches = batch == rows.end() ? 0.0 : batch->samples;

    for (auto &row : rows) {
        if (row.sampleKind == "batch-total") {
            row.nominalEventsPerSecond = 0;
        }

        row.integrityOk = batches > 0 && row.clockAnomalies == 0 && row.missingBatches == 0 &&
                          row.pendingBatches == 0 &&
                          (row.sampleKind == "batch-total" || row.samples == batches * row.expectedPerBatch);
    }

    return rows;
}

void writeComparisonHeader(std::ostream &output) {
    output << "\"scenario\",\"category\",\"metric\",\"runs\",\"minimum\",\"median\",\"maximum\"\n";
}

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

    std::vector<std::filesystem::path> summaries;

    for (const auto &entry : std::filesystem::directory_iterator{runDirectory}) {
        const auto filename = entry.path().filename().string();

        if (entry.is_regular_file() && filename.ends_with(SUMMARY_SUFFIX) && filename != "monitoring-summary.csv") {
            summaries.push_back(entry.path());
        }
    }

    std::ranges::sort(summaries);

    if (summaries.empty()) {
        return std::unexpected(std::format("no profile summary files found in: {}", runDirectory.string()));
    }

    MonitoringAnalysis analysis;

    for (const auto &summary : summaries) {
        const auto filename = summary.filename().string();
        const auto profile = filename.substr(0, filename.size() - SUMMARY_SUFFIX.size());
        auto measurement = readMeasurement(summary);

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

    for (const auto &entry : std::filesystem::directory_iterator{runDirectory}) {
        const auto filename = entry.path().filename().string();

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

    std::ranges::sort(latencyRows, {}, [](const LatencyRunRow &row) {
        return std::tuple{row.nominalEventsPerSecond, row.identity.scenario, row.identity.repetition, row.sampleKind};
    });

    std::ofstream runs;

    if (auto opened = openOutput(runs, runDirectory / "latency-runs.csv"); !opened) {
        return opened;
    }

    runs << "\"profile\",\"scenario\",\"repetition\",\"nominal_events_per_second\",\"sample_kind\","
            "\"samples\",\"expected_per_batch\",\"callbacks\",\"clock_anomalies\",\"missing_batches\","
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

    struct LatencyMetric {
        std::string_view name;
        double LatencyRunRow::*member;
    };

    constexpr std::array latencyMetrics{
        LatencyMetric{"mean_us", &LatencyRunRow::meanUs},  LatencyMetric{"p50_us", &LatencyRunRow::p50Us},
        LatencyMetric{"p90_us", &LatencyRunRow::p90Us},    LatencyMetric{"p95_us", &LatencyRunRow::p95Us},
        LatencyMetric{"p99_us", &LatencyRunRow::p99Us},    LatencyMetric{"p999_us", &LatencyRunRow::p999Us},
        LatencyMetric{"max_us", &LatencyRunRow::maximumUs}};
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

    std::ofstream report;

    if (auto opened = openOutput(report, runDirectory / "REPORT.md"); !opened) {
        return opened;
    }

    report << R"(# Repeated latency benchmark

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Runs | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---:|---:|---:|---:|---:|---|
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
        const auto integrity = std::ranges::all_of(rows, [](const auto *row) {
            return row->integrityOk;
        });
        const auto missing = rows.empty() ? 0.0 : rows.front()->missingBatches;
        const auto integrityText = integrity ? "OK" : missing > 0 ? "CHECK; missing batches reported" : "CHECK";
        report << "| " << scenario << " | " << p50.runs << " | " << std::fixed << std::setprecision(3) << p50.median
               << " | " << p99.median << " (" << p99.minimum << "–" << p99.maximum << ") | " << p999.median << " | "
               << batchP99.median << " | " << integrityText << " |\n";
    }

    report << "\nGenerated files: `latency-runs.csv`, `latency-comparison.csv`, `monitoring.csv`, "
              "`monitoring-summary.csv`, and `monitoring-comparison.csv`.\n\n"
              "## Client monitoring\n\n"
              "The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run "
              "sum and buffer is the largest per-run high-water mark.\n\n"
              "| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |\n"
              "|---|---:|---:|---:|---:|---:|\n";

    const auto monitoringValue = [&](const std::string &scenario, std::string_view metric) {
        const auto found = std::ranges::find_if(monitoringComparisons, [&](const ComparisonRow &row) {
            return row.scenario == scenario && row.category == "client" && row.metric == metric;
        });

        return found == monitoringComparisons.end() ? RunComparison{} : found->comparison;
    };

    for (const auto &[scenario, rows] : scenarios) {
        const auto readRate = monitoringValue(scenario, "read_data_rps_run_mean");
        const auto readLag = monitoringValue(scenario, "read_data_lag_us_run_mean");
        const auto cpu = monitoringValue(scenario, "cpu_percent_run_mean");
        const auto buffer = monitoringValue(scenario, "buffer_run_max");
        const auto dropped = monitoringValue(scenario, "dropped_run_sum");
        report << "| " << scenario << " | " << readRate.median << " | " << readLag.median / 1000.0 << " | "
               << cpu.median << "% | " << buffer.maximum << " | " << dropped.maximum << " |\n";
    }

    if (std::ranges::any_of(latencyRows, [](const LatencyRunRow &row) {
            return !row.integrityOk;
        })) {
        return std::unexpected("one or more benchmark runs failed latency integrity checks; see REPORT.md");
    }

    return {};
}

} // namespace latency
