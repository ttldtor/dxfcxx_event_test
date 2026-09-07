// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace latency {

/** Identifies a supported market event type. */
enum class EventKind : char {
    /** A Quote event. */
    QUOTE = 'Q',
    /** A Trade event. */
    TRADE = 'T',
    /** A TradeETH event. */
    TRADE_ETH = 'E',
    /** A Summary event. */
    SUMMARY = 'S',
    /** A TimeAndSale time-series event. */
    TIME_AND_SALE = 'N'
};

/** Describes one event type and its instrument count in a task pattern. */
struct PatternItem {
    /** The event type. */
    EventKind kind{};
    /** The number of synthetic instruments to create. */
    std::size_t quantity{};

    /** Compares all fields of two pattern items. */
    friend bool operator==(const PatternItem &, const PatternItem &) = default;
};

/** A parsed subscription task, for example `SUB:Q100;S1;T5@100ms`. */
struct TaskPattern {
    /** Event types and instrument counts in their source order. */
    std::vector<PatternItem> items;
    /** Interval between server publications. */
    std::chrono::milliseconds publishPeriod{std::chrono::seconds{1}};
    /** Explicit subscribed symbol-universe size, or empty when the largest event quantity defines it. */
    std::optional<std::size_t> subscribedSymbolCount;

    /** Number of active regional sources, from `A` onward, published in addition to composite symbols. */
    std::size_t regionalSourceCount{};

    /** Seed for deterministic per-publication event-type block shuffling, or empty for source order. */
    std::optional<std::uint64_t> shuffleSeed;

    /**
     * Returns the total number of recurring events generated in each batch.
     *
     * @return The sum of all item quantities.
     */
    [[nodiscard]] std::size_t eventCount() const;

    /**
     * Serializes this pattern to its canonical DSL representation.
     *
     * @return A string such as `SUB:Q100;S1;T5`.
     */
    [[nodiscard]] std::string toString() const;

    /**
     * Generates synthetic symbols for one event type.
     *
     * @param kind The event type to inspect.
     * @return Symbols numbered from zero up to the configured quantity, or an empty vector if the type is absent.
     */
    [[nodiscard]] std::vector<std::string> symbols(EventKind kind) const;

    /** Returns the shared symbol universe used by the combined subscription and initial Profile state. */
    [[nodiscard]] std::vector<std::string> symbols() const;

    /**
     * Returns composite and configured regional symbols used by market-event subscriptions and publication pools.
     *
     * Composite symbols are followed by complete `&A`, `&B`, ... regional blocks. Profiles continue to use only
     * the base symbols returned by `symbols()`.
     */
    [[nodiscard]] std::vector<std::string> marketSymbols() const;

    /** Returns the number of composite and regional record keys for each recurring event type. */
    [[nodiscard]] std::size_t marketSymbolCount() const;

    /**
     * Looks up the configured quantity for an event type.
     *
     * @param kind The event type to inspect.
     * @return The quantity, or `std::nullopt` if the type is absent.
     */
    [[nodiscard]] std::optional<std::size_t> quantity(EventKind kind) const;

    /** Returns the number of instruments in the shared symbol universe. */
    [[nodiscard]] std::size_t symbolCount() const;

    /** Returns the configured recurring event rate, excluding initial events and the marker record. */
    [[nodiscard]] double nominalEventsPerSecond() const;

    /** Returns the number of publication slots intersecting a positive duration, rounded up. */
    [[nodiscard]] std::size_t batchCount(std::chrono::milliseconds duration) const;

    /** Compares all fields of two task patterns. */
    friend bool operator==(const TaskPattern &, const TaskPattern &) = default;
};

/** Describes a task-pattern parsing failure. */
struct ParseError {
    /** Zero-based position at which parsing failed. */
    std::size_t position{};
    /** Human-readable description of the failure. */
    std::string message;
};

/**
 * Parses a subscription task in the `SUB:<type><quantity>[;...][@<period>][#<symbols>][&<regions>][~<seed>]` DSL.
 *
 * Supported type codes are `Q`, `T`, `E`, `S`, and `N` (`TimeAndSale`). Each type may occur at most once and its
 * quantity must be positive.
 * An optional symbol count expands the subscribed universe without changing the events published per batch.
 * An optional regional-source count from 1 to 26 expands market-event record keys without changing the batch size.
 * An optional shuffle seed deterministically changes the order of event-type blocks for each publication.
 *
 * @param text The task text, for example `SUB:Q100;S1;T5@100ms`.
 * @return The parsed pattern, or a positional parse error.
 */
std::expected<TaskPattern, ParseError> parseTask(std::string_view text);

/**
 * Builds the persistent `TextMessage` symbol used to deliver timestamp markers for a task.
 *
 * @param task The task-control symbol.
 * @return A marker symbol that cannot be parsed as a task command.
 */
std::string markerSymbol(std::string_view task);

/**
 * Tests whether a `TextMessage` symbol belongs to the timestamp-marker channel.
 *
 * @param symbol The symbol to inspect.
 * @return `true` for symbols produced by `markerSymbol`.
 */
bool isMarkerSymbol(std::string_view symbol);

/**
 * Returns the display name of an event type.
 *
 * @param kind The event type.
 * @return `Quote`, `Trade`, `TradeETH`, `Summary`, or `TimeAndSale`.
 */
std::string eventKindName(EventKind kind);

/** A single end-to-end latency observation. All time values are in nanoseconds. */
struct Sample {
    /** Local Unix time at which the client received the event. */
    std::int64_t observedAtNs{};
    /** Unix time carried by the server's batch marker. */
    std::int64_t publishTimeNs{};
    /** Difference between observation and publication time. */
    std::int64_t latencyNs{};
    /** The observed event type. */
    EventKind kind{EventKind::QUOTE};
    /** The observed event symbol. */
    std::string symbol;
};

/** Descriptive statistics for a set of nanosecond values. All floating-point metrics use nanoseconds. */
struct Statistics {
    /** Number of input values. */
    std::size_t count{};
    /** Minimum value. */
    double minimum{};
    /** Arithmetic mean. */
    double mean{};
    /** 50th percentile. */
    double p50{};
    /** 90th percentile. */
    double p90{};
    /** 95th percentile. */
    double p95{};
    /** 99th percentile. */
    double p99{};
    /** 99.9th percentile. */
    double p999{};
    /** Maximum value. */
    double maximum{};
    /** First quartile. */
    double q1{};
    /** Third quartile. */
    double q3{};
    /** Interquartile range (`q3 - q1`). */
    double iqr{};
    /** Upper outlier threshold (`q3 + 1.5 * iqr`). */
    double outlierThreshold{};
    /** Number of values strictly above the upper outlier threshold. */
    std::size_t outlierCount{};
};

/**
 * Owns latency samples selected from one Trade/TradeETH stream by three timestamp-filtering methods.
 *
 * Source timestamps and observations are deliberately reduced to milliseconds to reproduce the customer's
 * measurement resolution. Negative latencies are represented as zero, matching the first histogram bin used by
 * that measurement.
 */
struct SourceTimeMethodSnapshot {
    /** Every Trade and TradeETH observation with a positive source timestamp. */
    std::vector<std::int64_t> all;

    /** Observations whose source timestamp strictly advances within one event-kind and symbol pair. */
    std::vector<std::int64_t> perSeriesMonotonic;

    /** Observations whose source timestamp strictly advances across the entire mixed event stream. */
    std::vector<std::int64_t> globalMonotonic;

    /** Number of per-series observations rejected because their timestamp was equal to or older than the last one. */
    std::size_t perSeriesRejected{};

    /** Number of globally filtered observations rejected because their timestamp did not advance the global maximum. */
    std::size_t globalRejected{};

    /** Negative latencies clamped to zero for the all, per-series, and global methods, respectively. */
    std::array<std::size_t, 3> negativeClamped{};
};

/**
 * Applies unfiltered, per-series monotonic, and global monotonic selection to the same source-time observations.
 *
 * The global method intentionally models a measurement that shares one last-seen timestamp across every Trade and
 * TradeETH symbol. The per-series method provides the corresponding instrument-aware control.
 */
class SourceTimeMethodComparison {
    SourceTimeMethodSnapshot data_;
    std::unordered_map<std::string, std::int64_t> lastBySeries_;
    std::optional<std::int64_t> lastGlobal_;

    public:
    /** Reserves storage for an expected run without changing accumulated observations. */
    void reserve(std::size_t observations, std::size_t series);

    /** Clears samples and timestamp-filter state for a new measurement interval. */
    void reset();

    /**
     * Adds one Trade or TradeETH source-time observation to all applicable methods.
     *
     * @param kind Trade or TradeETH event kind used to distinguish independent series.
     * @param symbol Event symbol used to distinguish independent series.
     * @param sourceTimeMs Event source time in Unix milliseconds; non-positive values are ignored.
     * @param observedAtNs Client observation time in Unix nanoseconds.
     */
    void observe(EventKind kind, std::string_view symbol, std::int64_t sourceTimeMs, std::int64_t observedAtNs);

    /** Returns a copy of all accumulated samples and rejection counters. */
    [[nodiscard]] SourceTimeMethodSnapshot snapshot() const;
};

/**
 * Computes an Excel-compatible `PERCENTILE.INC` value from sorted input.
 *
 * @param sortedValues Values sorted in ascending order.
 * Values at or below zero select the first item, and values at or above one select the last item.
 *
 * @param percentile The requested percentile.
 * @return The linearly interpolated percentile, or NaN for empty input.
 */
double percentileInc(const std::vector<std::int64_t> &sortedValues, double percentile);

/**
 * Calculates latency statistics and upper-IQR outliers.
 *
 * @param values Input values in nanoseconds; their order is not significant.
 * @return Calculated statistics. All fields are zero for empty input.
 */
Statistics calculateStatistics(const std::vector<std::int64_t> &values);

/**
 * Tests whether a value exceeds a calculated upper outlier threshold.
 *
 * @param value The value in nanoseconds.
 * @param statistics Statistics containing the threshold to apply.
 * @return `true` if the statistics are non-empty and the value is strictly above the threshold.
 */
bool isUpperOutlier(std::int64_t value, const Statistics &statistics);

/**
 * Converts nanoseconds to microseconds.
 *
 * @param nanoseconds The duration in nanoseconds.
 * @return The duration in microseconds.
 */
double nanosecondsToMicroseconds(double nanoseconds);

/**
 * Obtains the current Unix time with nanosecond units.
 *
 * @return Nanoseconds elapsed since the Unix epoch.
 */
std::int64_t unixNanosNow();

/**
 * Formats a Unix timestamp as UTC with nine fractional digits.
 *
 * @param unixNanos Nanoseconds elapsed since the Unix epoch.
 * @return An ISO-8601 timestamp such as `2026-01-02T03:04:05.123456789Z`.
 */
std::string utcTimestamp(std::int64_t unixNanos);

/**
 * Parses a positive duration with an optional `ms`, `s`, `m`, or `h` suffix. A value without a suffix is interpreted
 * as seconds.
 *
 * @param text The duration text, for example `500ms`, `30s`, `5m`, or `1h`.
 * @return The duration in milliseconds, or a human-readable error message.
 */
std::expected<std::chrono::milliseconds, std::string> parseDuration(std::string_view text);

/**
 * Parses the reporting period used by QD monitoring.
 *
 * A positive value follows the same syntax as `parseDuration`. The literal `0` disables periodic monitoring.
 *
 * @param text The reporting period, for example `10s`, or `0` to disable reporting.
 * @return A positive period, `std::nullopt` when disabled, or a human-readable error message.
 */
std::expected<std::optional<std::chrono::milliseconds>, std::string> parseMonitoringPeriod(std::string_view text);

/**
 * Formats a monitoring period using the fractional-second syntax accepted by QD `TimePeriod`.
 *
 * @param period A positive reporting period, or `std::nullopt` when reporting is disabled.
 * @return A value such as `10s`, `0.5s`, or `0`.
 */
std::string monitoringPeriodPropertyValue(const std::optional<std::chrono::milliseconds> &period);

} // namespace latency
