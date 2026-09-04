#include "latency/core.hpp"

#include <dxfeed_graal_cpp_api/api.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace dxfcpp;

namespace {
std::atomic_bool interrupted{};

constexpr std::array EVENT_KINDS{latency::EventKind::QUOTE, latency::EventKind::TRADE, latency::EventKind::SUMMARY};
constexpr std::string_view MONITORING_STAT_PROPERTY = "monitoring.stat";

std::size_t eventKindIndex(latency::EventKind kind) {
    switch (kind) {
    case latency::EventKind::QUOTE:
        return 0;
    case latency::EventKind::TRADE:
        return 1;
    case latency::EventKind::SUMMARY:
        return 2;
    }

    throw std::invalid_argument("unknown event kind");
}

std::string_view eventSampleKind(latency::EventKind kind) {
    switch (kind) {
    case latency::EventKind::QUOTE:
        return "event-quote";
    case latency::EventKind::TRADE:
        return "event-trade";
    case latency::EventKind::SUMMARY:
        return "event-summary";
    }

    return "event-unknown";
}

void onSignal(int) {
    interrupted.store(true);
}

// Command-line settings for the warm-up, measurement windows, batch expiry, and report files.
struct Config {
    std::string address{"127.0.0.1:7400"};
    std::string task{"SUB:Q100"};
    std::chrono::milliseconds warmup{30s}, duration{5min}, window{10s}, batchTimeout{30s};
    std::optional<std::chrono::milliseconds> monitoringStat{10s};
    std::filesystem::path output{"latency"};
};

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
  --monitoring-stat 10s    0 disables QD statistics
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
            } else {
                throw std::invalid_argument(std::format("unknown argument: {}", arg));
            }
        }
    }

    return config;
}

void configureMonitoring(const std::optional<std::chrono::milliseconds> &period) {
    const auto value = latency::monitoringPeriodPropertyValue(period);

    if (!System::setProperty(MONITORING_STAT_PROPERTY, value)) {
        throw std::runtime_error(std::format("cannot set {}={}", MONITORING_STAT_PROPERTY, value));
    }
}

// An event is retained until its batch marker supplies the authoritative publication timestamp.
struct PendingEvent {
    std::int64_t observed{};
    std::optional<std::int64_t> publishTime;
    latency::EventKind kind{};
    std::string symbol;
};

// Tracks all events and timing bounds belonging to one server publication sequence.
struct PendingBatch {
    std::optional<std::int64_t> timestamp;
    std::size_t received{};
    std::int64_t firstObserved{}, lastObserved{};
    std::vector<PendingEvent> events;
};

// Correlates received events with their batch marker and accumulates latency samples for reporting.
class Collector {
    const std::size_t expectedPerBatch_;
    const std::chrono::milliseconds batchTimeout_;
    mutable std::mutex mutex_;
    bool measuring_{};
    bool acceptingNewBatches_{};
    std::optional<std::int32_t> startBoundarySequence_;
    std::vector<latency::Sample> eventWindow_, batchWindow_;
    std::array<std::vector<std::int64_t>, EVENT_KINDS.size()> eventGlobalByKind_;
    std::vector<std::int64_t> batchGlobal_;
    std::map<std::int32_t, PendingBatch> pending_;
    std::size_t callbacksWindow_{}, callbacksTotal_{}, negativeWindow_{}, negativeTotal_{};
    std::size_t missingWindow_{}, missingTotal_{};

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

    // A batch becomes measurable only after every expected event and its marker have arrived.
    void complete(std::map<std::int32_t, PendingBatch>::iterator it) {
        auto &batch = it->second;

        if (!batch.timestamp || batch.received != expectedPerBatch_) {
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

        const auto batchDelta = batch.lastObserved - *batch.timestamp;

        if (batchDelta < 0) {
            ++negativeWindow_;
            ++negativeTotal_;
        } else {
            batchWindow_.push_back(
                latency::Sample{batch.lastObserved, *batch.timestamp, batchDelta, latency::EventKind::QUOTE, {}});
            batchGlobal_.push_back(batchDelta);
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
        batch.events.push_back(std::move(event));
        complete(it);
    }

    // The first sequence observed after the warm-up may already be partially delivered. Exclude that whole
    // publication from the measured set. After the measurement deadline, only batches already in flight may finish.
    bool acceptBatch(std::int32_t sequence) {
        if (!startBoundarySequence_) {
            startBoundarySequence_ = sequence;

            return false;
        }

        if (sequence == *startBoundarySequence_) {
            return false;
        }

        return acceptingNewBatches_ || pending_.contains(sequence);
    }

    public:
    struct Window {
        std::vector<latency::Sample> events, batches;
        std::size_t callbacks{}, negative{}, missing{}, pending{};
    };

    struct Totals {
        std::array<std::vector<std::int64_t>, EVENT_KINDS.size()> eventsByKind;
        std::vector<std::int64_t> batches;
        std::size_t callbacks{}, negative{}, missing{}, pending{};
    };

    Collector(const latency::TaskPattern &pattern, std::chrono::milliseconds timeout, std::size_t reserveWindowEvents,
              std::size_t reserveBatches)
        : expectedPerBatch_(pattern.eventCount()), batchTimeout_(timeout) {
        eventWindow_.reserve(reserveWindowEvents);
        batchWindow_.reserve(32);

        for (const auto kind : EVENT_KINDS) {
            eventGlobalByKind_[eventKindIndex(kind)].reserve(pattern.quantity(kind).value_or(0) * reserveBatches);
        }

        batchGlobal_.reserve(reserveBatches);
    }

    void beginMeasurement() {
        std::lock_guard lock{mutex_};
        eventWindow_.clear();
        batchWindow_.clear();
        pending_.clear();
        callbacksWindow_ = negativeWindow_ = missingWindow_ = 0;
        startBoundarySequence_.reset();
        acceptingNewBatches_ = true;
        measuring_ = true;
    }

    void endMeasurement() {
        std::lock_guard lock{mutex_};
        acceptingNewBatches_ = false;
    }

    void handle(const std::vector<std::shared_ptr<EventType>> &events) {
        const auto observed = latency::unixNanosNow();
        std::lock_guard lock{mutex_};

        if (!measuring_) {
            return;
        }

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
                it->second.timestamp = *timestamp;

                if (!it->second.firstObserved) {
                    it->second.firstObserved = observed;
                }

                it->second.lastObserved = std::max(it->second.lastObserved, observed);

                if (const auto current = pending_.find(markerSequence); current != pending_.end()) {
                    complete(current);
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
    }

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
                ++missingWindow_;
                ++missingTotal_;
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        Window result{std::move(eventWindow_), std::move(batchWindow_), callbacksWindow_,
                      negativeWindow_,         missingWindow_,          pending_.size()};
        eventWindow_.clear();
        batchWindow_.clear();
        callbacksWindow_ = negativeWindow_ = missingWindow_ = 0;

        return result;
    }

    Totals totals() const {
        std::lock_guard lock{mutex_};

        return Totals{eventGlobalByKind_, batchGlobal_,  callbacksTotal_,
                      negativeTotal_,     missingTotal_, pending_.size()};
    }

    std::size_t pendingCount() const {
        std::lock_guard lock{mutex_};

        return pending_.size();
    }
};

// Writes per-window and whole-run statistics while retaining detailed rows only for outliers.
class Reporter {
    std::ofstream summary_, outliers_;
    std::int64_t runStart_{};
    std::size_t expectedEventsPerBatch_{};
    std::int64_t publishPeriodMs_{};
    double nominalEventsPerSecond_{};
    std::array<std::size_t, EVENT_KINDS.size()> expectedEventsByKind_{};
    std::size_t windowIndex_{};

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
                      std::size_t pending) {
        summary_ << latency::utcTimestamp(start) << ',' << latency::utcTimestamp(end) << ',' << kind << ',' << s.count
                 << ',' << expectedPerBatch << ',' << publishPeriodMs_ << ',' << nominalEventsPerSecond_ << ','
                 << callbacks << ',' << negative << ',' << missing << ',' << pending << ',' << microseconds(s.minimum)
                 << ',' << microseconds(s.mean) << ',' << microseconds(s.p50) << ',' << microseconds(s.p90) << ','
                 << microseconds(s.p95) << ',' << microseconds(s.p99) << ',' << microseconds(s.p999) << ','
                 << microseconds(s.maximum) << ',' << microseconds(s.q1) << ',' << microseconds(s.q3) << ','
                 << microseconds(s.iqr) << ',' << microseconds(s.outlierThreshold) << ',' << s.outlierCount << '\n';
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
    // Opens both reports together so a run cannot proceed with only one of its outputs available.
    Reporter(const std::filesystem::path &prefix, const latency::TaskPattern &pattern)
        : runStart_(latency::unixNanosNow()), expectedEventsPerBatch_(pattern.eventCount()),
          publishPeriodMs_(pattern.publishPeriod.count()), nominalEventsPerSecond_(pattern.nominalEventsPerSecond()) {
        for (const auto kind : EVENT_KINDS) {
            expectedEventsByKind_[eventKindIndex(kind)] = pattern.quantity(kind).value_or(0);
        }

        auto summaryPath = prefix;
        summaryPath += "-summary.csv";
        auto outliersPath = prefix;
        outliersPath += "-outliers.csv";

        if (summaryPath.has_parent_path()) {
            std::filesystem::create_directories(summaryPath.parent_path());
        }

        summary_.open(summaryPath);
        outliers_.open(outliersPath);

        if (!summary_ || !outliers_) {
            throw std::runtime_error("cannot open output CSV files");
        }

        summary_ << "window_start_utc,window_end_utc,sample_kind,samples,expected_per_batch,publish_period_ms,"
                    "nominal_events_per_second,callbacks,clock_anomalies,missing_batches,pending_batches,min_us,"
                    "mean_us,p50_us,p90_us,p95_us,p99_us,p999_us,max_us,q1_us,"
                    "q3_us,iqr_us,"
                    "outlier_threshold_us,outliers\n";
        outliers_ << "observed_at_utc,sample_kind,event_type,symbol,publish_time_ns,latency_ns,window_threshold_ns\n";
        std::cout << "Writing " << summaryPath << " and " << outliersPath << '\n';
    }

    void beginMeasurement(std::int64_t start) {
        runStart_ = start;
    }

    void window(Collector::Window data, std::int64_t start, std::int64_t end) {
        ++windowIndex_;
        const auto eventStats = latency::calculateStatistics(latencies(data.events));
        const auto batchStats = latency::calculateStatistics(latencies(data.batches));

        std::cout << "Window " << windowIndex_ << " [" << latency::utcTimestamp(start) << ", "
                  << latency::utcTimestamp(end) << "] callbacks=" << data.callbacks
                  << " clock-anomalies=" << data.negative << " missing-batches=" << data.missing << '\n';
        printStats("event", eventStats);

        writeSummary(start, end, "event", eventStats, expectedEventsPerBatch_, data.callbacks, data.negative,
                     data.missing, data.pending);

        for (const auto kind : EVENT_KINDS) {
            const auto sampleKind = eventSampleKind(kind);
            const auto stats = latency::calculateStatistics(latencies(data.events, kind));

            printStats(sampleKind, stats);
            writeSummary(start, end, sampleKind, stats, expectedEventsByKind_[eventKindIndex(kind)], data.callbacks,
                         data.negative, data.missing, data.pending);
            writeOutliers(data.events, sampleKind, stats, kind);
        }

        printStats("batch", batchStats);
        writeSummary(start, end, "batch", batchStats, 1, data.callbacks, data.negative, data.missing, data.pending);
        writeOutliers(data.batches, "batch", batchStats);
        summary_.flush();
        outliers_.flush();
    }

    void final(const Collector::Totals &totals, std::int64_t end) {
        const auto eventStats = latency::calculateStatistics(combinedLatencies(totals.eventsByKind));
        const auto batchStats = latency::calculateStatistics(totals.batches);

        std::cout << "Final summary callbacks=" << totals.callbacks << " clock-anomalies=" << totals.negative
                  << " missing-batches=" << totals.missing << " pending=" << totals.pending << '\n';
        printStats("event", eventStats);

        writeSummary(runStart_, end, "event-total", eventStats, expectedEventsPerBatch_, totals.callbacks,
                     totals.negative, totals.missing, totals.pending);

        for (const auto kind : EVENT_KINDS) {
            const auto sampleKind = std::string{eventSampleKind(kind)} + "-total";
            const auto stats = latency::calculateStatistics(totals.eventsByKind[eventKindIndex(kind)]);

            printStats(sampleKind, stats);
            writeSummary(runStart_, end, sampleKind, stats, expectedEventsByKind_[eventKindIndex(kind)],
                         totals.callbacks, totals.negative, totals.missing, totals.pending);
        }

        printStats("batch", batchStats);
        writeSummary(runStart_, end, "batch-total", batchStats, 1, totals.callbacks, totals.negative, totals.missing,
                     totals.pending);
    }
};

bool waitFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;

    while (!interrupted.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
    }

    return !interrupted.load();
}
} // namespace

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
        Collector collector{*pattern, config.batchTimeout, expected * windowBatches, runBatches};
        Reporter reporter{config.output, *pattern};

        configureMonitoring(config.monitoringStat);
        const auto endpoint =
            DXEndpoint::newBuilder()->withRole(DXEndpoint::Role::STREAM_FEED)->withName("latency-client")->build();
        const auto feed = endpoint->getFeed();
        std::vector<std::shared_ptr<DXFeedSubscription>> subscriptions;
        const auto subscribe = [&](const EventTypeEnum &type, latency::EventKind kind) {
            auto symbols = pattern->symbols(kind);

            if (symbols.empty()) {
                return;
            }

            auto subscription = feed->createSubscription(type);
            subscription->addEventListener([&collector](const auto &events) {
                collector.handle(events);
            });
            subscription->addSymbols(symbols);
            subscriptions.push_back(std::move(subscription));
        };
        subscribe(Quote::TYPE, latency::EventKind::QUOTE);
        subscribe(Trade::TYPE, latency::EventKind::TRADE);
        subscribe(Summary::TYPE, latency::EventKind::SUMMARY);
        auto control = feed->createSubscription(TextMessage::TYPE);
        control->addEventListener([&collector](const auto &events) {
            collector.handle(events);
        });
        control->addSymbols(config.task);
        endpoint->connect(config.address);
        std::cout << "Connected to " << config.address << ", task " << config.task << ", expected " << expected
                  << " events/batch every " << pattern->publishPeriod.count() << " ms (nominal "
                  << pattern->nominalEventsPerSecond() << " events/s). Warm-up " << config.warmup.count() << " ms.\n";

        if (!waitFor(config.warmup)) {
            control->removeSymbols(config.task);
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

        collector.endMeasurement();
        const auto drainDeadline = std::chrono::steady_clock::now() + config.batchTimeout;

        // Keep the marker subscription active while already-published batches drain. New sequences are ignored after
        // endMeasurement(), so the publisher can continue without extending the measured interval.
        while (collector.pendingCount() && std::chrono::steady_clock::now() < drainDeadline && !interrupted.load()) {
            std::this_thread::sleep_for(100ms);
        }

        control->removeSymbols(config.task);

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
