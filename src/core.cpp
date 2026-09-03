#include "latency/core.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

namespace latency {
namespace {
std::size_t symbolWidth(std::size_t quantity) {
    return std::max<std::size_t>(2, std::to_string(quantity - 1).size());
}

std::optional<EventKind> parseKind(char value) {
    switch (value) {
    case 'Q':
        return EventKind::QUOTE;
    case 'T':
        return EventKind::TRADE;
    case 'S':
        return EventKind::SUMMARY;
    default:
        return std::nullopt;
    }
}
} // namespace

std::size_t TaskPattern::eventCount() const {
    std::size_t result = 0;

    for (const auto &item : items) {
        if (item.quantity > std::numeric_limits<std::size_t>::max() - result) {
            throw std::overflow_error("total event count overflows size_t");
        }

        result += item.quantity;
    }

    return result;
}

std::string TaskPattern::toString() const {
    std::string result = "SUB:";

    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) {
            result.push_back(';');
        }

        result.push_back(static_cast<char>(items[i].kind));
        result += std::to_string(items[i].quantity);
    }

    return result;
}

std::optional<std::size_t> TaskPattern::quantity(EventKind kind) const {
    for (const auto &item : items) {
        if (item.kind == kind) {
            return item.quantity;
        }
    }

    return std::nullopt;
}

std::vector<std::string> TaskPattern::symbols(EventKind kind) const {
    const auto count = quantity(kind).value_or(0);
    std::vector<std::string> result;

    result.reserve(count);

    if (!count) {
        return result;
    }

    const auto width = symbolWidth(count);

    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(std::format("{}{:0{}}", static_cast<char>(kind), i, width));
    }

    return result;
}

std::expected<TaskPattern, ParseError> parseTask(std::string_view text) {
    constexpr std::string_view PREFIX = "SUB:";

    if (!text.starts_with(PREFIX)) {
        return std::unexpected(ParseError{0, "expected SUB:"});
    }

    std::size_t pos = PREFIX.size();

    if (pos == text.size()) {
        return std::unexpected(ParseError{pos, "expected event pattern"});
    }

    TaskPattern task;
    std::set<EventKind> seen;

    while (pos < text.size()) {
        const auto itemPos = pos;
        const auto kind = parseKind(text[pos]);

        if (!kind) {
            return std::unexpected(ParseError{pos, "unknown event type"});
        }

        if (!seen.insert(*kind).second) {
            return std::unexpected(ParseError{pos, "duplicate event type"});
        }

        ++pos;
        const auto numberStart = pos;
        std::size_t quantity{};
        const auto [ptr, ec] = std::from_chars(text.data() + pos, text.data() + text.size(), quantity);

        if (ec == std::errc::result_out_of_range) {
            return std::unexpected(ParseError{numberStart, "quantity is too large"});
        }

        if (ec != std::errc{} || ptr == text.data() + numberStart) {
            return std::unexpected(ParseError{numberStart, "expected positive quantity"});
        }

        pos = static_cast<std::size_t>(ptr - text.data());

        if (!quantity) {
            return std::unexpected(ParseError{numberStart, "quantity must be greater than zero"});
        }

        task.items.push_back(PatternItem{*kind, quantity});

        try {
            (void)task.eventCount();
        } catch (const std::overflow_error &) {
            return std::unexpected(ParseError{itemPos, "total quantity is too large"});
        }

        if (pos == text.size()) {
            break;
        }

        if (text[pos] != ';') {
            return std::unexpected(ParseError{pos, "expected ';' or end of input"});
        }

        ++pos;

        if (pos == text.size()) {
            return std::unexpected(ParseError{pos, "trailing separator"});
        }
    }

    return task;
}

std::string eventKindName(EventKind kind) {
    switch (kind) {
    case EventKind::QUOTE:
        return "Quote";
    case EventKind::TRADE:
        return "Trade";
    case EventKind::SUMMARY:
        return "Summary";
    }

    return "Unknown";
}

double percentileInc(const std::vector<std::int64_t> &sortedValues, double percentile) {
    if (sortedValues.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    if (percentile <= 0) {
        return static_cast<double>(sortedValues.front());
    }

    if (percentile >= 1) {
        return static_cast<double>(sortedValues.back());
    }

    const double index = (static_cast<double>(sortedValues.size()) - 1.0) * percentile;
    const auto lower = static_cast<std::size_t>(std::floor(index));
    const auto upper = static_cast<std::size_t>(std::ceil(index));
    const double fraction = index - static_cast<double>(lower);

    return static_cast<double>(sortedValues[lower]) + fraction * (sortedValues[upper] - sortedValues[lower]);
}

Statistics calculateStatistics(const std::vector<std::int64_t> &values) {
    Statistics result;

    result.count = values.size();

    if (values.empty()) {
        return result;
    }

    auto sorted = values;

    std::ranges::sort(sorted);
    result.minimum = static_cast<double>(sorted.front());
    result.maximum = static_cast<double>(sorted.back());
    result.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
    result.p50 = percentileInc(sorted, .50);
    result.p90 = percentileInc(sorted, .90);
    result.p95 = percentileInc(sorted, .95);
    result.p99 = percentileInc(sorted, .99);
    result.p999 = percentileInc(sorted, .999);
    result.q1 = percentileInc(sorted, .25);
    result.q3 = percentileInc(sorted, .75);
    result.iqr = result.q3 - result.q1;
    result.outlierThreshold = result.q3 + 1.5 * result.iqr;
    result.outlierCount = static_cast<std::size_t>(std::ranges::count_if(sorted, [&](auto value) {
        return static_cast<double>(value) > result.outlierThreshold;
    }));

    return result;
}

bool isUpperOutlier(std::int64_t value, const Statistics &statistics) {
    return statistics.count && static_cast<double>(value) > statistics.outlierThreshold;
}

double nanosecondsToMicroseconds(double nanoseconds) {
    return nanoseconds / 1'000.0;
}

std::int64_t unixNanosNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string utcTimestamp(std::int64_t unixNanos) {
    const auto time = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{unixNanos}};

    return std::format("{:%FT%T}Z", time);
}

std::expected<std::chrono::milliseconds, std::string> parseDuration(std::string_view text) {
    if (text.empty()) {
        return std::unexpected("empty duration");
    }

    std::int64_t multiplier = 1000;
    auto number = text;

    if (text.ends_with("ms")) {
        multiplier = 1;
        number.remove_suffix(2);
    } else if (text.ends_with('s')) {
        multiplier = 1000;
        number.remove_suffix(1);
    } else if (text.ends_with('m')) {
        multiplier = 60'000;
        number.remove_suffix(1);
    } else if (text.ends_with('h')) {
        multiplier = 3'600'000;
        number.remove_suffix(1);
    }

    std::int64_t value{};
    const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);

    if (ec != std::errc{} || ptr != number.data() + number.size() || value <= 0 ||
        value > std::numeric_limits<std::int64_t>::max() / multiplier) {
        return std::unexpected("invalid duration: " + std::string{text});
    }

    return std::chrono::milliseconds{value * multiplier};
}

std::expected<std::optional<std::chrono::milliseconds>, std::string> parseMonitoringPeriod(std::string_view text) {
    if (text == "0") {
        return std::nullopt;
    }

    auto duration = parseDuration(text);

    if (!duration) {
        return std::unexpected(duration.error());
    }

    return *duration;
}

std::string monitoringPeriodPropertyValue(const std::optional<std::chrono::milliseconds> &period) {
    if (!period) {
        return "0";
    }

    const auto milliseconds = period->count();
    const auto seconds = milliseconds / 1000;
    const auto remainder = milliseconds % 1000;

    if (remainder == 0) {
        return std::to_string(seconds) + "s";
    }

    auto fraction = std::format("{:03}", remainder);

    while (fraction.ends_with('0')) {
        fraction.pop_back();
    }

    return std::to_string(seconds) + "." + fraction + "s";
}
} // namespace latency
