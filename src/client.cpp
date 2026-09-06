// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/core.hpp"

#include <dxfeed_graal_cpp_api/api.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;
using namespace dxfcpp;

namespace {
std::atomic_bool interrupted{};

constexpr std::array EVENT_KINDS{latency::EventKind::QUOTE, latency::EventKind::TRADE, latency::EventKind::TRADE_ETH,
                                 latency::EventKind::SUMMARY};
constexpr std::string_view MONITORING_STAT_PROPERTY = "monitoring.stat";
constexpr auto CALLBACK_QUIET_PERIOD = 500ms;

/** Selects the dxFeed endpoint contract used by the benchmark client. */
enum class ClientRole {
    /** Delivers every queued stream update. */
    STREAM_FEED,

    /** Retains the latest ticker state and permits intermediate-state supersession. */
    FEED
};

/** Returns the fixed array index assigned to a supported event kind. */
std::size_t eventKindIndex(latency::EventKind kind) {
    switch (kind) {
    case latency::EventKind::QUOTE:
        return 0;
    case latency::EventKind::TRADE:
        return 1;
    case latency::EventKind::TRADE_ETH:
        return 2;
    case latency::EventKind::SUMMARY:
        return 3;
    }

    throw std::invalid_argument("unknown event kind");
}

/** Returns the CSV sample-kind label for a supported market event. */
std::string_view eventSampleKind(latency::EventKind kind) {
    switch (kind) {
    case latency::EventKind::QUOTE:
        return "event-quote";
    case latency::EventKind::TRADE:
        return "event-trade";
    case latency::EventKind::TRADE_ETH:
        return "event-trade-eth";
    case latency::EventKind::SUMMARY:
        return "event-summary";
    }

    return "event-unknown";
}

/** Records an operating-system termination request. */
void onSignal(int) {
    interrupted.store(true);
}

/** Command-line settings for the warm-up, measurement windows, batch expiry, and report files. */
struct Config {
    std::string address{"127.0.0.1:7400"};
    std::string task{"SUB:Q100"};
    std::chrono::milliseconds warmup{30s}, duration{5min}, window{10s}, batchTimeout{30s}, startupTimeout{30s};
    std::chrono::milliseconds listenerDelay{};
    std::chrono::milliseconds aggregationPeriod{};
    std::optional<std::chrono::milliseconds> monitoringStat{10s};
    std::filesystem::path output{"latency"};
    ClientRole role{ClientRole::STREAM_FEED};
    std::int32_t eventsBatchLimit{DXFeedSubscription::OPTIMAL_BATCH_LIMIT};
};

/** Returns the command-line name of a client endpoint role. */
std::string_view roleName(ClientRole role) {
    return role == ClientRole::FEED ? "feed" : "stream-feed";
}

/** Returns the command-line name of a native event-notification batch limit. */
std::string eventsBatchLimitName(std::int32_t limit) {
    if (limit == DXFeedSubscription::OPTIMAL_BATCH_LIMIT) {
        return "optimal";
    }

    if (limit == DXFeedSubscription::MAX_BATCH_LIMIT) {
        return "maximum";
    }

    return std::to_string(limit);
}

/** Parses a named or positive numeric native event-notification batch limit. */
std::int32_t parseEventsBatchLimit(std::string_view value) {
    if (value == "optimal" || value == "0") {
        return DXFeedSubscription::OPTIMAL_BATCH_LIMIT;
    }

    if (value == "maximum") {
        return DXFeedSubscription::MAX_BATCH_LIMIT;
    }

    std::int32_t result{};
    const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), result);

    if (error != std::errc{} || ptr != value.data() + value.size() || result <= 0) {
        throw std::invalid_argument(std::format("invalid events batch limit: {}", value));
    }

    return result;
}

/** Parses and validates benchmark-client command-line arguments. */
Config parseArgs(int argc, char **argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            std::cout << R"(Usage: latency_client [options]
  --address HOST:PORT       default 127.0.0.1:7400
  --task SUB:Q100;S1;T5@100ms    default SUB:Q100@1s
  --warmup 30s             --duration 5m
  --window 10s             --batch-timeout 30s
  --startup-timeout 30s    initial Profile delivery timeout
  --listener-delay 1ms     delay each market-event callback; default 0
  --aggregation-period 1ms aggregate market notifications; default 0
  --events-batch-limit N   optimal, maximum, or a positive integer; default optimal
  --monitoring-stat 10s    0 disables QD statistics
  --role stream-feed|feed  default stream-feed
  --output PREFIX          default latency
)";
            std::exit(0);
        }

        if (i + 1 >= argc) {
            throw std::invalid_argument(std::format("missing value for {}", arg));
        }

        const std::string value = argv[++i];

        if (arg == "--address") {
            config.address = value;
        } else if (arg == "--task") {
            config.task = value;
        } else if (arg == "--output") {
            config.output = value;
        } else if (arg == "--monitoring-stat") {
            auto period = latency::parseMonitoringPeriod(value);

            if (!period) {
                throw std::invalid_argument(period.error());
            }

            config.monitoringStat = *period;
        } else if (arg == "--role") {
            if (value == "feed") {
                config.role = ClientRole::FEED;
            } else if (value == "stream-feed") {
                config.role = ClientRole::STREAM_FEED;
            } else {
                throw std::invalid_argument(std::format("unknown role: {}", value));
            }
        } else if (arg == "--events-batch-limit") {
            config.eventsBatchLimit = parseEventsBatchLimit(value);
        } else if (arg == "--aggregation-period" && value == "0") {
            config.aggregationPeriod = 0ms;
        } else if (arg == "--listener-delay" && value == "0") {
            config.listenerDelay = 0ms;
        } else {
            auto duration = latency::parseDuration(value);

            if (!duration) {
                throw std::invalid_argument(duration.error());
            }

            if (arg == "--warmup") {
                config.warmup = *duration;
            } else if (arg == "--duration") {
                config.duration = *duration;
            } else if (arg == "--window") {
                config.window = *duration;
            } else if (arg == "--batch-timeout") {
                config.batchTimeout = *duration;
            } else if (arg == "--startup-timeout") {
                config.startupTimeout = *duration;
            } else if (arg == "--listener-delay") {
                config.listenerDelay = *duration;
            } else if (arg == "--aggregation-period") {
                config.aggregationPeriod = *duration;
            } else {
                throw std::invalid_argument(std::format("unknown argument: {}", arg));
            }
        }
    }

    return config;
}

/** Applies the selected QD monitoring period directly to an endpoint builder. */
void configureMonitoring(const std::shared_ptr<DXEndpoint::Builder> &builder,
                         const std::optional<std::chrono::milliseconds> &period) {
    const auto value = latency::monitoringPeriodPropertyValue(period);

    if (!builder->supportsProperty(MONITORING_STAT_PROPERTY)) {
        throw std::runtime_error(std::format("endpoint builder does not support {}", MONITORING_STAT_PROPERTY));
    }

    builder->withProperty(MONITORING_STAT_PROPERTY, value);
}

/** An event retained until its batch marker supplies the authoritative publication timestamp. */
struct PendingEvent {
    std::int64_t observed{};
    std::optional<std::int64_t> publishTime;
    latency::EventKind kind{};
    std::string symbol;
};

/** Tracks all events and timing bounds belonging to one server publication sequence. */
struct PendingBatch {
    std::optional<std::int64_t> timestamp;
    std::size_t received{};
    std::array<std::size_t, EVENT_KINDS.size()> receivedByKind{};
    std::int64_t firstObserved{}, lastObserved{};
    std::vector<PendingEvent> events;
};

/** Counts expected and observed delivery outcomes for each supported event kind. */
struct DeliveryCounters {
    std::array<std::size_t, EVENT_KINDS.size()> published{}, delivered{}, listenerDeficit{}, excess{};
    std::size_t fullPublications{}, partialPublications{}, emptyPublications{};
    std::size_t uncorrelatedEvents{};
};

/** Correlates received events with their batch marker and accumulates latency samples for reporting. */
class Collector {
    const std::size_t expectedPerBatch_;
    const std::array<std::size_t, EVENT_KINDS.size()> expectedByKind_;
    const std::chrono::milliseconds batchTimeout_;
    const bool allowConflation_;
    mutable std::mutex mutex_;
    std::condition_variable profilesCv_;
    bool measuring_{};
    bool acceptingNewBatches_{};
    std::unordered_set<std::int32_t> preMeasurementSequences_;
    std::vector<latency::Sample> eventWindow_, batchWindow_;
    std::vector<std::int64_t> callbackSizeWindow_, callbackDurationWindow_;
    std::array<std::vector<std::int64_t>, EVENT_KINDS.size()> eventGlobalByKind_;
    std::vector<std::int64_t> batchGlobal_;
    std::vector<std::int64_t> callbackSizeGlobal_, callbackDurationGlobal_;
    std::map<std::int32_t, PendingBatch> pending_;
    std::size_t callbacksWindow_{}, callbacksTotal_{}, negativeWindow_{}, negativeTotal_{};
    std::size_t missingWindow_{}, missingTotal_{};
    std::unordered_set<std::string> profileSymbolsReceived_;
    std::size_t activity_{};
    DeliveryCounters deliveryWindow_, deliveryTotal_;

    /** Identifies the correlation fields extracted from one supported market event. */
    struct Description {
        latency::EventKind kind;
        std::string symbol;
        std::optional<std::int32_t> sequence;
        std::optional<std::int64_t> publishTime;
    };

    // Extract the wire fields used to associate each supported event type with a published batch.
    static std::optional<Description> describe(const std::shared_ptr<EventType> &event) {
        if (auto value = event->sharedAs<Quote>()) {
            const auto encodedSequence = value->getBidSize();

            if (!std::isfinite(encodedSequence) || encodedSequence <= 0 ||
                encodedSequence >= static_cast<double>(TextMessage::MAX_SEQUENCE) ||
                std::trunc(encodedSequence) != encodedSequence) {
                return std::nullopt;
            }

            return Description{latency::EventKind::QUOTE, value->getEventSymbol(),
                               static_cast<std::int32_t>(encodedSequence), std::nullopt};
        }

        if (auto value = event->sharedAs<Trade>()) {
            return Description{latency::EventKind::TRADE, value->getEventSymbol(), value->getSequence(), std::nullopt};
        }

        if (auto value = event->sharedAs<TradeETH>()) {
            return Description{latency::EventKind::TRADE_ETH, value->getEventSymbol(), value->getSequence(),
                               std::nullopt};
        }

        if (auto value = event->sharedAs<Summary>()) {
            return Description{latency::EventKind::SUMMARY, value->getEventSymbol(), value->getDayId(), std::nullopt};
        }

        return std::nullopt;
    }

    static std::optional<std::int64_t> markerTimestamp(std::string_view text) {
        constexpr std::string_view PREFIX = "LATENCY_BATCH:";

        if (!text.starts_with(PREFIX)) {
            return std::nullopt;
        }

        text.remove_prefix(PREFIX.size());
        std::int64_t result{};
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);

        if (ec != std::errc{} || ptr != text.data() + text.size() || result <= 0) {
            return std::nullopt;
        }

        return result;
    }

    void emitEvents(PendingBatch &batch) {
        if (!batch.timestamp) {
            return;
        }

        for (auto &event : batch.events) {
            const auto publishTime = *batch.timestamp;
            const auto delta = event.observed - publishTime;

            if (delta < 0) {
                ++negativeWindow_;
                ++negativeTotal_;
                continue;
            }

            eventWindow_.push_back(
                latency::Sample{event.observed, publishTime, delta, event.kind, std::move(event.symbol)});
            eventGlobalByKind_[eventKindIndex(event.kind)].push_back(delta);
        }

        batch.events.clear();
    }

    bool hasExpectedComposition(const PendingBatch &batch) const {
        return std::ranges::equal(batch.receivedByKind, expectedByKind_);
    }

    void finish(std::map<std::int32_t, PendingBatch>::iterator it) {
        auto &batch = it->second;

        if (!batch.timestamp) {
            deliveryWindow_.uncorrelatedEvents += batch.received;
            deliveryTotal_.uncorrelatedEvents += batch.received;

            if (!allowConflation_) {
                ++missingWindow_;
                ++missingTotal_;
            }

            pending_.erase(it);

            return;
        }

        emitEvents(batch);
        bool complete = true;

        for (const auto kind : EVENT_KINDS) {
            const auto index = eventKindIndex(kind);
            const auto expected = expectedByKind_[index];
            const auto received = batch.receivedByKind[index];

            deliveryWindow_.delivered[index] += received;
            deliveryTotal_.delivered[index] += received;

            if (received < expected) {
                const auto difference = expected - received;
                deliveryWindow_.listenerDeficit[index] += difference;
                deliveryTotal_.listenerDeficit[index] += difference;
                complete = false;
            } else if (received > expected) {
                const auto difference = received - expected;
                deliveryWindow_.excess[index] += difference;
                deliveryTotal_.excess[index] += difference;
                complete = false;
            }
        }

        if (complete) {
            ++deliveryWindow_.fullPublications;
            ++deliveryTotal_.fullPublications;
        } else if (!batch.received) {
            ++deliveryWindow_.emptyPublications;
            ++deliveryTotal_.emptyPublications;
        } else {
            ++deliveryWindow_.partialPublications;
            ++deliveryTotal_.partialPublications;
        }

        if (!complete && !allowConflation_) {
            ++missingWindow_;
            ++missingTotal_;
        }

        if (batch.received) {
            const auto batchDelta = batch.lastObserved - *batch.timestamp;

            if (batchDelta < 0) {
                ++negativeWindow_;
                ++negativeTotal_;
            } else {
                batchWindow_.push_back(
                    latency::Sample{batch.lastObserved, *batch.timestamp, batchDelta, latency::EventKind::QUOTE, {}});
                batchGlobal_.push_back(batchDelta);
            }
        }

        pending_.erase(it);
    }

    void addEvent(std::int32_t sequence, PendingEvent event) {
        if (!acceptBatch(sequence)) {
            return;
        }

        auto [it, inserted] = pending_.try_emplace(sequence);
        auto &batch = it->second;

        if (!batch.received) {
            batch.firstObserved = event.observed;
        }

        batch.lastObserved = std::max(batch.lastObserved, event.observed);
        ++batch.received;
        ++batch.receivedByKind[eventKindIndex(event.kind)];
        batch.events.push_back(std::move(event));
        emitEvents(batch);

        if (batch.timestamp && hasExpectedComposition(batch)) {
            finish(it);
        }
    }

    // Publications observed during warm-up can straddle the measurement boundary because marker and market events
    // use separate subscriptions. Exclude every such sequence. After the stop request, accept tail publications
    // until both subscriptions become quiet.
    bool acceptBatch(std::int32_t sequence) {
        if (preMeasurementSequences_.contains(sequence)) {
            return false;
        }

        return acceptingNewBatches_ || pending_.contains(sequence);
    }

    void handle(const std::vector<std::shared_ptr<EventType>> &events, bool marketCallback,
                std::chrono::milliseconds delay) {
        const auto callbackStart = std::chrono::steady_clock::now();

        if (delay > 0ms) {
            std::this_thread::sleep_for(delay);
        }

        const auto observed = latency::unixNanosNow();
        std::lock_guard lock{mutex_};
        bool profilesChanged = false;

        for (const auto &event : events) {
            if (const auto profile = event->sharedAs<Profile>()) {
                profilesChanged |= profileSymbolsReceived_.insert(profile->getEventSymbol()).second;
            }
        }

        if (profilesChanged) {
            profilesCv_.notify_all();
        }

        if (!measuring_) {
            for (const auto &event : events) {
                if (const auto marker = event->sharedAs<TextMessage>(); marker && markerTimestamp(marker->getText())) {
                    preMeasurementSequences_.insert(marker->getSequence());

                    continue;
                }

                if (const auto description = describe(event); description && description->sequence) {
                    preMeasurementSequences_.insert(*description->sequence);
                }
            }

            return;
        }

        ++activity_;
        ++callbacksWindow_;
        ++callbacksTotal_;

        for (const auto &event : events) {
            if (const auto marker = event->sharedAs<TextMessage>(); marker) {
                const auto timestamp = markerTimestamp(marker->getText());

                if (!timestamp) {
                    continue;
                }

                const auto markerSequence = marker->getSequence();

                if (!acceptBatch(markerSequence)) {
                    continue;
                }

                auto [it, inserted] = pending_.try_emplace(markerSequence);
                auto &batch = it->second;

                if (!batch.timestamp) {
                    batch.timestamp = *timestamp;

                    for (const auto kind : EVENT_KINDS) {
                        const auto index = eventKindIndex(kind);
                        deliveryWindow_.published[index] += expectedByKind_[index];
                        deliveryTotal_.published[index] += expectedByKind_[index];
                    }
                }

                if (!batch.firstObserved) {
                    batch.firstObserved = observed;
                }

                batch.lastObserved = std::max(batch.lastObserved, observed);
                emitEvents(batch);

                if (hasExpectedComposition(batch)) {
                    finish(it);
                }

                continue;
            }

            const auto description = describe(event);

            if (!description) {
                continue;
            }

            PendingEvent pendingEvent{observed, description->publishTime, description->kind,
                                      std::move(description->symbol)};

            if (description->sequence) {
                addEvent(*description->sequence, std::move(pendingEvent));
            }
        }

        if (marketCallback) {
            const auto duration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - callbackStart)
                    .count();
            const auto size = static_cast<std::int64_t>(events.size());

            callbackSizeWindow_.push_back(size);
            callbackDurationWindow_.push_back(duration);
            callbackSizeGlobal_.push_back(size);
            callbackDurationGlobal_.push_back(duration);
        }
    }

    public:
    /** Owns the latency and delivery data accumulated during one reporting window. */
    struct Window {
        /** Correlated event and whole-batch latency samples. */
        std::vector<latency::Sample> events, batches;

        /** Market-listener callback sizes and durations. */
        std::vector<std::int64_t> callbackSizes, callbackDurations;

        /** Callback, clock-anomaly, incomplete-batch, and pending-batch counts. */
        std::size_t callbacks{}, negative{}, missing{}, pending{};

        /** Delivery accounting for this reporting window. */
        DeliveryCounters delivery;
    };

    /** Owns whole-run samples and delivery counters for final reporting. */
    struct Totals {
        /** Event latency samples partitioned by supported event kind. */
        std::array<std::vector<std::int64_t>, EVENT_KINDS.size()> eventsByKind;

        /** Whole-publication latency samples. */
        std::vector<std::int64_t> batches;

        /** Market-listener callback sizes and durations. */
        std::vector<std::int64_t> callbackSizes, callbackDurations;

        /** Callback, clock-anomaly, incomplete-batch, and pending-batch counts. */
        std::size_t callbacks{}, negative{}, missing{}, pending{};

        /** Number of unique initial Profile symbols received. */
        std::size_t profilesReceived{};

        /** Whole-run delivery accounting. */
        DeliveryCounters delivery;
    };

    /** Creates a collector and reserves storage for the expected workload. */
    Collector(const latency::TaskPattern &pattern, std::chrono::milliseconds timeout, std::size_t reserveWindowEvents,
              std::size_t reserveBatches, bool allowConflation)
        : expectedPerBatch_(pattern.eventCount()), expectedByKind_([&pattern] {
              std::array<std::size_t, EVENT_KINDS.size()> result{};

              for (const auto kind : EVENT_KINDS) {
                  result[eventKindIndex(kind)] = pattern.quantity(kind).value_or(0);
              }

              return result;
          }()),
          batchTimeout_(timeout), allowConflation_(allowConflation) {
        eventWindow_.reserve(reserveWindowEvents);
        batchWindow_.reserve(32);
        callbackSizeWindow_.reserve(reserveBatches);
        callbackDurationWindow_.reserve(reserveBatches);
        callbackSizeGlobal_.reserve(reserveBatches);
        callbackDurationGlobal_.reserve(reserveBatches);

        for (const auto kind : EVENT_KINDS) {
            eventGlobalByKind_[eventKindIndex(kind)].reserve(pattern.quantity(kind).value_or(0) * reserveBatches);
        }

        batchGlobal_.reserve(reserveBatches);
    }

    /** Clears window state and begins accepting measurement batches. */
    void beginMeasurement() {
        std::lock_guard lock{mutex_};
        eventWindow_.clear();
        batchWindow_.clear();
        callbackSizeWindow_.clear();
        callbackDurationWindow_.clear();
        pending_.clear();
        callbacksWindow_ = negativeWindow_ = missingWindow_ = 0;
        deliveryWindow_ = {};
        acceptingNewBatches_ = true;
        measuring_ = true;
    }

    /** Stops accepting new publication sequences while retaining pending batches. */
    void endMeasurement() {
        std::lock_guard lock{mutex_};
        acceptingNewBatches_ = false;
    }

    /** Handles control-marker and initial-profile events. */
    void handle(const std::vector<std::shared_ptr<EventType>> &events) {
        handle(events, false, 0ms);
    }

    /** Handles a market-event callback after an optional artificial delay. */
    void handleMarket(const std::vector<std::shared_ptr<EventType>> &events, std::chrono::milliseconds delay) {
        handle(events, true, delay);
    }

    /** Extracts and resets the current reporting window, optionally expiring all pending batches. */
    Window takeWindow(bool expireAll = false) {
        std::lock_guard lock{mutex_};
        const auto cutoff = latency::unixNanosNow() - batchTimeout_.count() * 1'000'000;

        // Expired incomplete batches are diagnostics, not latency samples, because their publication time or event
        // set is insufficient for a valid measurement.
        for (auto it = pending_.begin(); it != pending_.end();) {
            const auto reference = it->second.timestamp.value_or(it->second.firstObserved);

            if (expireAll || reference < cutoff) {
                std::cerr << "Incomplete batch sequence=" << it->first << " received=" << it->second.received << '/'
                          << expectedPerBatch_ << " marker=" << (it->second.timestamp ? "yes" : "no");

                if (!it->second.events.empty()) {
                    std::cerr << " first-event-time=" << it->second.events.front().publishTime.value_or(-1);
                }

                std::cerr << '\n';
                auto current = it++;
                finish(current);
            } else {
                ++it;
            }
        }

        Window result{std::move(eventWindow_),
                      std::move(batchWindow_),
                      std::move(callbackSizeWindow_),
                      std::move(callbackDurationWindow_),
                      callbacksWindow_,
                      negativeWindow_,
                      missingWindow_,
                      pending_.size(),
                      deliveryWindow_};
        eventWindow_.clear();
        batchWindow_.clear();
        callbackSizeWindow_.clear();
        callbackDurationWindow_.clear();
        callbacksWindow_ = negativeWindow_ = missingWindow_ = 0;
        deliveryWindow_ = {};

        return result;
    }

    /** Returns a snapshot of whole-run samples and counters. */
    Totals totals() const {
        std::lock_guard lock{mutex_};

        return Totals{
            eventGlobalByKind_, batchGlobal_,  callbackSizeGlobal_, callbackDurationGlobal_,        callbacksTotal_,
            negativeTotal_,     missingTotal_, pending_.size(),     profileSymbolsReceived_.size(), deliveryTotal_};
    }

    /** Waits until all expected initial Profile symbols arrive or the timeout expires. */
    bool waitForProfiles(std::size_t expected, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock lock{mutex_};

        while (!interrupted.load() && profileSymbolsReceived_.size() < expected) {
            const auto wakeAt = std::min(deadline, std::chrono::steady_clock::now() + 100ms);

            profilesCv_.wait_until(lock, wakeAt);

            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }

        return profileSymbolsReceived_.size() >= expected;
    }

    /** Returns the number of unique initial Profile symbols received. */
    std::size_t profileCount() const {
        std::lock_guard lock{mutex_};

        return profileSymbolsReceived_.size();
    }

    /** Returns the number of publication sequences awaiting completion. */
    std::size_t pendingCount() const {
        std::lock_guard lock{mutex_};

        return pending_.size();
    }

    /** Returns a monotonic counter of listener activity. */
    std::size_t activity() const {
        std::lock_guard lock{mutex_};

        return activity_;
    }
};

/** Writes per-window and whole-run statistics while retaining detailed rows only for outliers. */
class Reporter {
    std::ofstream summary_, outliers_, callbacks_;
    std::int64_t runStart_{};
    std::size_t expectedEventsPerBatch_{};
    std::int64_t publishPeriodMs_{};
    double nominalEventsPerSecond_{};
    std::array<std::size_t, EVENT_KINDS.size()> expectedEventsByKind_{};
    std::size_t initialProfilesExpected_{};
    std::size_t windowIndex_{};
    std::string endpointRole_;
    std::string eventsBatchLimit_;
    std::int64_t aggregationPeriodMs_{};

    /** Flattens delivery counters into the values written to one report row. */
    struct DeliveryView {
        std::size_t published{}, delivered{}, listenerDeficit{}, excess{};
        std::size_t fullPublications{}, partialPublications{}, emptyPublications{};
        std::size_t uncorrelatedEvents{};
    };

    static DeliveryView deliveryView(const DeliveryCounters &delivery,
                                     std::optional<latency::EventKind> kind = std::nullopt) {
        DeliveryView result;

        for (const auto current : EVENT_KINDS) {
            if (kind && current != *kind) {
                continue;
            }

            const auto index = eventKindIndex(current);
            result.published += delivery.published[index];
            result.delivered += delivery.delivered[index];
            result.listenerDeficit += delivery.listenerDeficit[index];
            result.excess += delivery.excess[index];
        }

        result.fullPublications = delivery.fullPublications;
        result.partialPublications = delivery.partialPublications;
        result.emptyPublications = delivery.emptyPublications;
        result.uncorrelatedEvents = delivery.uncorrelatedEvents;

        return result;
    }

    static std::vector<std::int64_t> latencies(const std::vector<latency::Sample> &samples,
                                               std::optional<latency::EventKind> kind = std::nullopt) {
        std::vector<std::int64_t> result;
        result.reserve(samples.size());

        for (const auto &sample : samples) {
            if (kind && sample.kind != *kind) {
                continue;
            }

            result.push_back(sample.latencyNs);
        }

        return result;
    }

    static std::vector<std::int64_t>
    combinedLatencies(const std::array<std::vector<std::int64_t>, EVENT_KINDS.size()> &valuesByKind) {
        std::size_t size = 0;

        for (const auto &values : valuesByKind) {
            size += values.size();
        }

        std::vector<std::int64_t> result;
        result.reserve(size);

        for (const auto &values : valuesByKind) {
            result.insert(result.end(), values.begin(), values.end());
        }

        return result;
    }

    static double microseconds(double nanoseconds) {
        return latency::nanosecondsToMicroseconds(nanoseconds);
    }

    static void printStats(std::string_view kind, const latency::Statistics &s) {
        std::cout << std::fixed << std::setprecision(2) << "  " << kind << " N=" << s.count;

        if (s.count) {
            std::cout << " min=" << microseconds(s.minimum) << " mean=" << microseconds(s.mean)
                      << " p50=" << microseconds(s.p50) << " p90=" << microseconds(s.p90)
                      << " p95=" << microseconds(s.p95) << " p99=" << microseconds(s.p99)
                      << " p99.9=" << microseconds(s.p999) << " max=" << microseconds(s.maximum)
                      << " IQR=" << microseconds(s.iqr) << " threshold=" << microseconds(s.outlierThreshold)
                      << " outliers=" << s.outlierCount;
        }

        std::cout << " us\n";
    }

    void writeSummary(std::int64_t start, std::int64_t end, std::string_view kind, const latency::Statistics &s,
                      std::size_t expectedPerBatch, std::size_t callbacks, std::size_t negative, std::size_t missing,
                      std::size_t pending, const DeliveryView &delivery) {
        const auto listenerCoverage =
            delivery.published ? static_cast<double>(delivery.delivered) / delivery.published : 0.0;

        summary_ << latency::utcTimestamp(start) << ',' << latency::utcTimestamp(end) << ',' << kind << ',' << s.count
                 << ',' << expectedPerBatch << ',' << publishPeriodMs_ << ',' << nominalEventsPerSecond_ << ','
                 << endpointRole_ << ',' << eventsBatchLimit_ << ',' << aggregationPeriodMs_ << ','
                 << delivery.published << ',' << delivery.delivered << ',' << delivery.listenerDeficit << ','
                 << listenerCoverage << ',' << delivery.excess << ',' << delivery.fullPublications << ','
                 << delivery.partialPublications << ',' << delivery.emptyPublications << ','
                 << delivery.uncorrelatedEvents << ',' << callbacks << ',' << negative << ',' << missing << ','
                 << pending << ',' << microseconds(s.minimum) << ',' << microseconds(s.mean) << ','
                 << microseconds(s.p50) << ',' << microseconds(s.p90) << ',' << microseconds(s.p95) << ','
                 << microseconds(s.p99) << ',' << microseconds(s.p999) << ',' << microseconds(s.maximum) << ','
                 << microseconds(s.q1) << ',' << microseconds(s.q3) << ',' << microseconds(s.iqr) << ','
                 << microseconds(s.outlierThreshold) << ',' << s.outlierCount << '\n';
    }

    void writeCallbackSummary(std::int64_t start, std::int64_t end, std::string_view kind, std::string_view unit,
                              const latency::Statistics &statistics, double divisor = 1.0) {
        const auto value = [divisor](double current) {
            return current / divisor;
        };

        callbacks_ << latency::utcTimestamp(start) << ',' << latency::utcTimestamp(end) << ',' << kind << ',' << unit
                   << ',' << statistics.count << ',' << value(statistics.minimum) << ',' << value(statistics.mean)
                   << ',' << value(statistics.p50) << ',' << value(statistics.p90) << ',' << value(statistics.p95)
                   << ',' << value(statistics.p99) << ',' << value(statistics.p999) << ',' << value(statistics.maximum)
                   << ',' << value(statistics.q1) << ',' << value(statistics.q3) << ',' << value(statistics.iqr) << ','
                   << value(statistics.outlierThreshold) << ',' << statistics.outlierCount << '\n';
    }

    void writeOutliers(const std::vector<latency::Sample> &samples, std::string_view kind,
                       const latency::Statistics &stats, std::optional<latency::EventKind> eventKind = std::nullopt) {
        for (const auto &sample : samples) {
            if (eventKind && sample.kind != *eventKind) {
                continue;
            }

            if (latency::isUpperOutlier(sample.latencyNs, stats)) {
                outliers_ << latency::utcTimestamp(sample.observedAtNs) << ',' << kind << ','
                          << (eventKind ? latency::eventKindName(sample.kind) : "") << ',' << sample.symbol << ','
                          << sample.publishTimeNs << ',' << sample.latencyNs << ','
                          << static_cast<std::int64_t>(stats.outlierThreshold) << '\n';
            }
        }
    }

    public:
    /** Opens all report files together so a run cannot proceed with only some outputs available. */
    Reporter(const std::filesystem::path &prefix, const latency::TaskPattern &pattern, ClientRole role,
             std::int32_t eventsBatchLimit, std::int64_t aggregationPeriodMs)
        : runStart_(latency::unixNanosNow()), expectedEventsPerBatch_(pattern.eventCount()),
          publishPeriodMs_(pattern.publishPeriod.count()), nominalEventsPerSecond_(pattern.nominalEventsPerSecond()),
          initialProfilesExpected_(pattern.symbolCount()), endpointRole_(roleName(role)),
          eventsBatchLimit_(eventsBatchLimitName(eventsBatchLimit)), aggregationPeriodMs_(aggregationPeriodMs) {
        for (const auto kind : EVENT_KINDS) {
            expectedEventsByKind_[eventKindIndex(kind)] = pattern.quantity(kind).value_or(0);
        }

        auto summaryPath = prefix;
        summaryPath += "-summary.csv";
        auto outliersPath = prefix;
        outliersPath += "-outliers.csv";
        auto callbacksPath = prefix;
        callbacksPath += "-callbacks.csv";

        if (summaryPath.has_parent_path()) {
            std::filesystem::create_directories(summaryPath.parent_path());
        }

        summary_.open(summaryPath);
        outliers_.open(outliersPath);
        callbacks_.open(callbacksPath);

        if (!summary_ || !outliers_ || !callbacks_) {
            throw std::runtime_error("cannot open output CSV files");
        }

        summary_ << "window_start_utc,window_end_utc,sample_kind,samples,expected_per_batch,publish_period_ms,"
                    "nominal_events_per_second,endpoint_role,events_batch_limit,aggregation_period_ms,published,"
                    "delivered,listener_deficit,"
                    "listener_coverage,"
                    "excess_events,full_publications,partial_publications,empty_publications,uncorrelated_events,"
                    "callbacks,"
                    "clock_anomalies,missing_batches,pending_batches,min_us,"
                    "mean_us,p50_us,p90_us,p95_us,p99_us,p999_us,max_us,q1_us,"
                    "q3_us,iqr_us,"
                    "outlier_threshold_us,outliers\n";
        outliers_ << "observed_at_utc,sample_kind,event_type,symbol,publish_time_ns,latency_ns,window_threshold_ns\n";
        callbacks_ << "window_start_utc,window_end_utc,sample_kind,unit,samples,min,mean,p50,p90,p95,p99,p999,max,"
                      "q1,q3,iqr,outlier_threshold,outliers\n";
        std::cout << std::format("Writing {}, {}, and {}\n", summaryPath.string(), outliersPath.string(),
                                 callbacksPath.string());
    }

    /** Changes the whole-run start boundary after warm-up completes. */
    void beginMeasurement(std::int64_t start) {
        runStart_ = start;
    }

    /** Calculates and writes all statistics for one measurement window. */
    void window(Collector::Window data, std::int64_t start, std::int64_t end) {
        ++windowIndex_;
        const auto eventStats = latency::calculateStatistics(latencies(data.events));
        const auto batchStats = latency::calculateStatistics(latencies(data.batches));
        const auto callbackSizeStats = latency::calculateStatistics(std::move(data.callbackSizes));
        const auto callbackDurationStats = latency::calculateStatistics(std::move(data.callbackDurations));

        const auto delivery = deliveryView(data.delivery);
        const auto listenerCoveragePercent =
            delivery.published ? static_cast<double>(delivery.delivered) * 100.0 / delivery.published : 0.0;

        std::cout << std::format("Window {} [{}, {}] callbacks={} clock-anomalies={} missing-batches={} "
                                 "listener-events={}/{} ({:.3f}%) listener-deficit={} excess={} uncorrelated={} "
                                 "publications(full/partial/empty)={}/{}/{}\n",
                                 windowIndex_, latency::utcTimestamp(start), latency::utcTimestamp(end), data.callbacks,
                                 data.negative, data.missing, delivery.delivered, delivery.published,
                                 listenerCoveragePercent, delivery.listenerDeficit, delivery.excess,
                                 delivery.uncorrelatedEvents, delivery.fullPublications, delivery.partialPublications,
                                 delivery.emptyPublications);
        printStats("event", eventStats);

        writeSummary(start, end, "event", eventStats, expectedEventsPerBatch_, data.callbacks, data.negative,
                     data.missing, data.pending, deliveryView(data.delivery));

        for (const auto kind : EVENT_KINDS) {
            const auto sampleKind = eventSampleKind(kind);
            const auto stats = latency::calculateStatistics(latencies(data.events, kind));

            printStats(sampleKind, stats);
            writeSummary(start, end, sampleKind, stats, expectedEventsByKind_[eventKindIndex(kind)], data.callbacks,
                         data.negative, data.missing, data.pending, deliveryView(data.delivery, kind));
            writeOutliers(data.events, sampleKind, stats, kind);
        }

        printStats("batch", batchStats);
        writeSummary(start, end, "batch", batchStats, 1, data.callbacks, data.negative, data.missing, data.pending,
                     deliveryView(data.delivery));
        writeOutliers(data.batches, "batch", batchStats);
        writeCallbackSummary(start, end, "market-callback-size", "events", callbackSizeStats);
        writeCallbackSummary(start, end, "market-callback-duration", "us", callbackDurationStats, 1'000.0);
        summary_.flush();
        outliers_.flush();
        callbacks_.flush();
    }

    /** Calculates and writes the final whole-run statistics. */
    void final(const Collector::Totals &totals, std::int64_t end) {
        const auto eventStats = latency::calculateStatistics(combinedLatencies(totals.eventsByKind));
        const auto batchStats = latency::calculateStatistics(totals.batches);
        const auto callbackSizeStats = latency::calculateStatistics(totals.callbackSizes);
        const auto callbackDurationStats = latency::calculateStatistics(totals.callbackDurations);
        const auto delivery = deliveryView(totals.delivery);
        const auto listenerCoveragePercent =
            delivery.published ? static_cast<double>(delivery.delivered) * 100.0 / delivery.published : 0.0;

        std::cout << std::format("Final summary callbacks={} clock-anomalies={} missing-batches={} pending={} "
                                 "listener-events={}/{} ({:.3f}%) listener-deficit={} excess={} uncorrelated={} "
                                 "publications(full/partial/empty)={}/{}/{}\n",
                                 totals.callbacks, totals.negative, totals.missing, totals.pending, delivery.delivered,
                                 delivery.published, listenerCoveragePercent, delivery.listenerDeficit, delivery.excess,
                                 delivery.uncorrelatedEvents, delivery.fullPublications, delivery.partialPublications,
                                 delivery.emptyPublications);
        printStats("event", eventStats);

        writeSummary(runStart_, end, "event-total", eventStats, expectedEventsPerBatch_, totals.callbacks,
                     totals.negative, totals.missing, totals.pending, deliveryView(totals.delivery));

        for (const auto kind : EVENT_KINDS) {
            const auto sampleKind = std::string{eventSampleKind(kind)} + "-total";
            const auto stats = latency::calculateStatistics(totals.eventsByKind[eventKindIndex(kind)]);

            printStats(sampleKind, stats);
            writeSummary(runStart_, end, sampleKind, stats, expectedEventsByKind_[eventKindIndex(kind)],
                         totals.callbacks, totals.negative, totals.missing, totals.pending,
                         deliveryView(totals.delivery, kind));
        }

        printStats("batch", batchStats);
        writeSummary(runStart_, end, "batch-total", batchStats, 1, totals.callbacks, totals.negative, totals.missing,
                     totals.pending, deliveryView(totals.delivery));
        writeCallbackSummary(runStart_, end, "market-callback-size-total", "events", callbackSizeStats);
        writeCallbackSummary(runStart_, end, "market-callback-duration-total", "us", callbackDurationStats, 1'000.0);
        std::cout << std::format("Initial Profile events received={}/{}\n", totals.profilesReceived,
                                 initialProfilesExpected_);
    }
};

/** Waits for a duration while polling for an operating-system termination request. */
bool waitFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;

    while (!interrupted.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
    }

    return !interrupted.load();
}
} // namespace

/** Runs the benchmark client application. */
int main(int argc, char **argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    try {
        const auto config = parseArgs(argc, argv);
        const auto pattern = latency::parseTask(config.task);

        if (!pattern) {
            throw std::invalid_argument(
                std::format("task parse error at {}: {}", pattern.error().position, pattern.error().message));
        }

        const auto expected = pattern->eventCount();
        const auto windowBatches = std::max<std::size_t>(1, pattern->batchCount(config.window));
        const auto runBatches = std::max<std::size_t>(1, pattern->batchCount(config.duration));
        Collector collector{*pattern, config.batchTimeout, expected * windowBatches, runBatches,
                            config.role == ClientRole::FEED};

        const auto endpointRole =
            config.role == ClientRole::FEED ? DXEndpoint::Role::FEED : DXEndpoint::Role::STREAM_FEED;
        const auto endpointBuilder = DXEndpoint::newBuilder()->withRole(endpointRole)->withName("latency-client");

        configureMonitoring(endpointBuilder, config.monitoringStat);

        const auto endpoint = endpointBuilder->build();
        const auto feed = endpoint->getFeed();
        std::vector<EventTypeEnum> eventTypes;
        const auto addType = [&](const EventTypeEnum &type, latency::EventKind kind) {
            if (pattern->quantity(kind).value_or(0)) {
                eventTypes.push_back(type);
            }
        };
        addType(Quote::TYPE, latency::EventKind::QUOTE);
        addType(Trade::TYPE, latency::EventKind::TRADE);
        addType(TradeETH::TYPE, latency::EventKind::TRADE_ETH);
        addType(Summary::TYPE, latency::EventKind::SUMMARY);

        auto subscription = feed->createSubscription(eventTypes);
        subscription->setAggregationPeriod(config.aggregationPeriod);
        subscription->setEventsBatchLimit(config.eventsBatchLimit);
        subscription->addEventListener([&collector, delay = config.listenerDelay](const auto &events) {
            collector.handleMarket(events, delay);
        });
        subscription->addSymbols(pattern->marketSymbols());

        auto profiles = feed->createSubscription(Profile::TYPE);
        profiles->addEventListener([&collector](const auto &events) {
            collector.handle(events);
        });
        profiles->addSymbols(pattern->symbols());

        auto control = feed->createSubscription(TextMessage::TYPE);
        control->addEventListener([&collector](const auto &events) {
            collector.handle(events);
        });
        const auto timestampMarkerSymbol = latency::markerSymbol(config.task);

        control->addSymbols(std::vector{config.task, timestampMarkerSymbol});

        const auto effectiveAggregationPeriodMs = subscription->getAggregationPeriod().getTime();
        Reporter reporter{config.output, *pattern, config.role, subscription->getEventsBatchLimit(),
                          effectiveAggregationPeriodMs};

        endpoint->connect(config.address);
        std::cout << std::format("Connected to {} using {}, task {}, expected {} events/batch every {} ms "
                                 "(nominal {:.3f} events/s), listener delay {} ms, events batch limit {}, "
                                 "market aggregation period {} ms (requested {} ms). Waiting for {} initial "
                                 "Profile events on a separate subscription.\n",
                                 config.address, roleName(config.role), config.task, expected,
                                 pattern->publishPeriod.count(), pattern->nominalEventsPerSecond(),
                                 config.listenerDelay.count(),
                                 eventsBatchLimitName(subscription->getEventsBatchLimit()),
                                 effectiveAggregationPeriodMs, config.aggregationPeriod.count(), pattern->symbolCount())
                  << std::flush;

        if (!collector.waitForProfiles(pattern->symbolCount(), config.startupTimeout)) {
            const auto received = collector.profileCount();

            control->removeSymbols(config.task);
            control->removeSymbols(timestampMarkerSymbol);
            endpoint->closeAndAwaitTermination();

            if (interrupted.load()) {
                return 130;
            }

            std::cerr << std::format("Initial Profile timeout: received {}/{} unique symbols within {} ms\n", received,
                                     pattern->symbolCount(), config.startupTimeout.count());

            return 1;
        }

        std::cout << std::format("Initial Profile setup complete: {}/{}. Warm-up {} ms.\n", collector.profileCount(),
                                 pattern->symbolCount(), config.warmup.count())
                  << std::flush;

        if (!waitFor(config.warmup)) {
            control->removeSymbols(config.task);
            control->removeSymbols(timestampMarkerSymbol);
            endpoint->closeAndAwaitTermination();

            return 130;
        }

        collector.beginMeasurement();
        const auto measurementStart = std::chrono::steady_clock::now();
        auto windowStartWall = latency::unixNanosNow();
        reporter.beginMeasurement(windowStartWall);
        auto nextWindow = measurementStart + config.window;
        const auto measurementEnd = measurementStart + config.duration;

        while (!interrupted.load() && std::chrono::steady_clock::now() < measurementEnd) {
            const auto target = std::min(nextWindow, measurementEnd);
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(target - std::chrono::steady_clock::now());

            if (remaining > 0ms) {
                waitFor(std::min(remaining, 100ms));
            }

            if (std::chrono::steady_clock::now() >= target) {
                const auto endWall = latency::unixNanosNow();
                reporter.window(collector.takeWindow(), windowStartWall, endWall);
                windowStartWall = endWall;
                nextWindow += config.window;
            }
        }

        control->removeSymbols(config.task);
        const auto drainDeadline = std::chrono::steady_clock::now() + config.batchTimeout;
        auto lastActivity = collector.activity();
        auto quietSince = std::chrono::steady_clock::now();

        // Removing the control symbol stops the publisher asynchronously. Keep accepting tail publications until
        // both subscriptions remain quiet and every correlated publication is complete.
        while (std::chrono::steady_clock::now() < drainDeadline && !interrupted.load()) {
            std::this_thread::sleep_for(100ms);
            const auto activity = collector.activity();

            if (activity != lastActivity) {
                lastActivity = activity;
                quietSince = std::chrono::steady_clock::now();
            } else if (!collector.pendingCount() &&
                       std::chrono::steady_clock::now() - quietSince >= CALLBACK_QUIET_PERIOD) {
                break;
            }
        }

        collector.endMeasurement();
        control->removeSymbols(timestampMarkerSymbol);

        const auto finalWall = latency::unixNanosNow();
        auto last = collector.takeWindow(true);

        if (!last.events.empty() || !last.batches.empty() || last.missing || last.negative) {
            reporter.window(std::move(last), windowStartWall, finalWall);
        }

        reporter.final(collector.totals(), finalWall);
        endpoint->closeAndAwaitTermination();

        return interrupted.load() ? 130 : 0;
    } catch (const std::exception &e) {
        std::cerr << "Client error: " << e.what() << '\n';

        return 1;
    }
}
