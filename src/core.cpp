// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

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
/** Returns the zero-padded symbol width required for a quantity. */
std::size_t symbolWidth(std::size_t quantity) {
    return std::max<std::size_t>(2, std::to_string(quantity - 1).size());
}

/** Maps a task-DSL type code to a supported event kind. */
std::optional<EventKind> parseKind(char value) {
    switch (value) {
    case 'Q':
        return EventKind::QUOTE;
    case 'T':
        return EventKind::TRADE;
    case 'E':
        return EventKind::TRADE_ETH;
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

std::size_t TaskPattern::symbolCount() const {
    if (subscribedSymbolCount) {
        return *subscribedSymbolCount;
    }

    const auto largest = std::ranges::max_element(items, {}, &PatternItem::quantity);

    return largest == items.end() ? 0 : largest->quantity;
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

    if (publishPeriod != std::chrono::seconds{1}) {
        result.push_back('@');

        if (publishPeriod.count() % 1000 == 0) {
            result += std::to_string(publishPeriod.count() / 1000);
            result.push_back('s');
        } else {
            result += std::to_string(publishPeriod.count());
            result += "ms";
        }
    }

    if (subscribedSymbolCount) {
        result.push_back('#');
        result += std::to_string(*subscribedSymbolCount);
    }

    if (shuffleSeed) {
        result.push_back('~');
        result += std::to_string(*shuffleSeed);
    }

    return result;
}

double TaskPattern::nominalEventsPerSecond() const {
    return static_cast<double>(eventCount()) * 1000.0 / static_cast<double>(publishPeriod.count());
}

std::size_t TaskPattern::batchCount(std::chrono::milliseconds duration) const {
    if (duration <= std::chrono::milliseconds::zero()) {
        return 0;
    }

    const auto periods = duration.count() / publishPeriod.count();

    return static_cast<std::size_t>(periods + (duration.count() % publishPeriod.count() != 0));
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

    const auto width = symbolWidth(symbolCount());

    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(std::format("SYM{:0{}}", i, width));
    }

    return result;
}

std::vector<std::string> TaskPattern::symbols() const {
    const auto count = symbolCount();
    std::vector<std::string> result;

    result.reserve(count);

    if (!count) {
        return result;
    }

    const auto width = symbolWidth(count);

    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(std::format("SYM{:0{}}", i, width));
    }

    return result;
}

std::expected<TaskPattern, ParseError> parseTask(std::string_view text) {
    constexpr std::string_view PREFIX = "SUB:";

    if (!text.starts_with(PREFIX)) {
        return std::unexpected(ParseError{0, "expected SUB:"});
    }

    const auto periodSeparator = text.find('@', PREFIX.size());
    const auto symbolSeparator = text.find('#', PREFIX.size());
    const auto shuffleSeparator = text.find('~', PREFIX.size());

    if (periodSeparator != std::string_view::npos && symbolSeparator != std::string_view::npos &&
        symbolSeparator < periodSeparator) {
        return std::unexpected(ParseError{symbolSeparator, "symbol count must follow publish period"});
    }

    if (periodSeparator != std::string_view::npos && shuffleSeparator != std::string_view::npos &&
        shuffleSeparator < periodSeparator) {
        return std::unexpected(ParseError{shuffleSeparator, "shuffle seed must follow publish period"});
    }

    if (symbolSeparator != std::string_view::npos && shuffleSeparator != std::string_view::npos &&
        shuffleSeparator < symbolSeparator) {
        return std::unexpected(ParseError{shuffleSeparator, "shuffle seed must follow symbol count"});
    }

    const auto separatorPosition = [&](std::size_t separator) {
        return separator == std::string_view::npos ? text.size() : separator;
    };
    const auto patternEnd = std::min(
        {separatorPosition(periodSeparator), separatorPosition(symbolSeparator), separatorPosition(shuffleSeparator)});
    std::size_t pos = PREFIX.size();

    if (pos == patternEnd) {
        return std::unexpected(ParseError{pos, "expected event pattern"});
    }

    TaskPattern task;
    std::set<EventKind> seen;

    while (pos < patternEnd) {
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
        const auto [ptr, ec] = std::from_chars(text.data() + pos, text.data() + patternEnd, quantity);

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

        if (pos == patternEnd) {
            break;
        }

        if (text[pos] != ';') {
            return std::unexpected(ParseError{pos, "expected ';' or end of input"});
        }

        ++pos;

        if (pos == patternEnd) {
            return std::unexpected(ParseError{pos, "trailing separator"});
        }
    }

    if (periodSeparator != std::string_view::npos) {
        const auto periodEnd = std::min(separatorPosition(symbolSeparator), separatorPosition(shuffleSeparator));
        const auto periodText = text.substr(periodSeparator + 1, periodEnd - periodSeparator - 1);

        if (periodText.empty()) {
            return std::unexpected(ParseError{periodSeparator + 1, "expected publish period"});
        }

        if (periodText.find('@') != std::string_view::npos) {
            return std::unexpected(ParseError{periodSeparator + 1 + periodText.find('@'), "duplicate publish period"});
        }

        const auto period = parseDuration(periodText);

        if (!period) {
            return std::unexpected(ParseError{periodSeparator + 1, period.error()});
        }

        task.publishPeriod = *period;
    }

    if (symbolSeparator != std::string_view::npos) {
        const auto symbolEnd = separatorPosition(shuffleSeparator);
        const auto symbolText = text.substr(symbolSeparator + 1, symbolEnd - symbolSeparator - 1);

        if (symbolText.empty()) {
            return std::unexpected(ParseError{symbolSeparator + 1, "expected symbol count"});
        }

        if (symbolText.find('#') != std::string_view::npos) {
            return std::unexpected(ParseError{symbolSeparator + 1 + symbolText.find('#'), "duplicate symbol count"});
        }

        std::size_t count{};
        const auto [ptr, ec] = std::from_chars(symbolText.data(), symbolText.data() + symbolText.size(), count);

        if (ec != std::errc{} || ptr != symbolText.data() + symbolText.size() || !count) {
            return std::unexpected(ParseError{symbolSeparator + 1, "invalid symbol count"});
        }

        const auto largest = task.symbolCount();

        if (count < largest) {
            return std::unexpected(
                ParseError{symbolSeparator + 1, "symbol count must not be smaller than an event quantity"});
        }

        task.subscribedSymbolCount = count;
    }

    if (shuffleSeparator != std::string_view::npos) {
        const auto seedText = text.substr(shuffleSeparator + 1);

        if (seedText.empty()) {
            return std::unexpected(ParseError{shuffleSeparator + 1, "expected shuffle seed"});
        }

        if (seedText.find('~') != std::string_view::npos) {
            return std::unexpected(ParseError{shuffleSeparator + 1 + seedText.find('~'), "duplicate shuffle seed"});
        }

        std::uint64_t seed{};
        const auto [ptr, ec] = std::from_chars(seedText.data(), seedText.data() + seedText.size(), seed);

        if (ec != std::errc{} || ptr != seedText.data() + seedText.size()) {
            return std::unexpected(ParseError{shuffleSeparator + 1, "invalid shuffle seed"});
        }

        task.shuffleSeed = seed;
    }

    return task;
}

std::string eventKindName(EventKind kind) {
    switch (kind) {
    case EventKind::QUOTE:
        return "Quote";
    case EventKind::TRADE:
        return "Trade";
    case EventKind::TRADE_ETH:
        return "TradeETH";
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

    return static_cast<double>(sortedValues[lower]) +
           fraction * static_cast<double>(sortedValues[upper] - sortedValues[lower]);
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
    result.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
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
        return std::unexpected(std::format("invalid duration: {}", text));
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
        return std::format("{}s", seconds);
    }

    auto fraction = std::format("{:03}", remainder);

    while (fraction.ends_with('0')) {
        fraction.pop_back();
    }

    return std::format("{}.{}s", seconds, fraction);
}

} // namespace latency
