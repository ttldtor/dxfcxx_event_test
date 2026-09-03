#include "latency/core.hpp"

#include <dxfeed_graal_cpp_api/api.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
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

void onSignal(int) {
    interrupted.store(true);
}

// Command-line settings for the warm-up, measurement windows, batch expiry, and report files.
struct Config {
    std::string address{"127.0.0.1:7400"};
    std::string task{"SUB:Q100"};
    std::chrono::milliseconds warmup{30s}, duration{5min}, window{10s}, batchTimeout{30s};
    std::filesystem::path output{"latency"};
};

Config parseArgs(int argc, char **argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: latency_client [options]\n"
                         "  --address HOST:PORT       default 127.0.0.1:7400\n"
                         "  --task SUB:Q100;S1;T5    default SUB:Q100\n"
                         "  --warmup 30s             --duration 5m\n"
                         "  --window 10s             --batch-timeout 30s\n"
                         "  --output PREFIX          default latency\n";
            std::exit(0);
        }
        if (i + 1 >= argc) {
            throw std::invalid_argument("missing value for " + arg);
        }

        const std::string value = argv[++i];

        if (arg == "--address") {
            config.address = value;
        } else if (arg == "--task") {
            config.task = value;
        } else if (arg == "--output") {
            config.output = value;
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
                throw std::invalid_argument("unknown argument: " + arg);
            }
        }
    }

    return config;
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
    std::vector<latency::Sample> eventWindow_, batchWindow_;
    std::vector<std::int64_t> eventGlobal_, batchGlobal_;
    std::map<std::int32_t, PendingBatch> pending_;
    std::map<std::int64_t, std::int32_t> secondToSequence_;
    std::map<std::int64_t, std::vector<PendingEvent>> quotesBySecond_;
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
            return Description{latency::EventKind::Quote, value->getEventSymbol(),
                               value->getSequence() == 0 ? std::nullopt : std::optional{value->getSequence()},
                               value->getTimeNanos()};
        }

        if (auto value = event->sharedAs<Trade>()) {
            return Description{latency::EventKind::Trade, value->getEventSymbol(), value->getSequence(),
                               value->getTimeNanos()};
        }

        if (auto value = event->sharedAs<Summary>()) {
            return Description{latency::EventKind::Summary, value->getEventSymbol(), value->getDayId(), std::nullopt};
        }

        return std::nullopt;
    }

    static std::optional<std::int64_t> markerTimestamp(std::string_view text) {
        constexpr std::string_view prefix = "LATENCY_BATCH:";
        if (!text.starts_with(prefix)) {
            return std::nullopt;
        }

        text.remove_prefix(prefix.size());
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
            eventGlobal_.push_back(delta);
        }
        const auto batchDelta = batch.lastObserved - *batch.timestamp;
        if (batchDelta < 0) {
            ++negativeWindow_;
            ++negativeTotal_;
        } else {
            batchWindow_.push_back(
                latency::Sample{batch.lastObserved, *batch.timestamp, batchDelta, latency::EventKind::Quote, {}});
            batchGlobal_.push_back(batchDelta);
        }
        pending_.erase(it);
    }

    void addEvent(std::int32_t sequence, PendingEvent event) {
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

    public:
    struct Window {
        std::vector<latency::Sample> events, batches;
        std::size_t callbacks{}, negative{}, missing{};
    };
    struct Totals {
        std::vector<std::int64_t> events, batches;
        std::size_t callbacks{}, negative{}, missing{}, pending{};
    };

    Collector(std::size_t expectedPerBatch, std::chrono::milliseconds timeout, std::size_t reserveEvents)
        : expectedPerBatch_(expectedPerBatch), batchTimeout_(timeout) {
        eventWindow_.reserve(reserveEvents);
        batchWindow_.reserve(32);
        eventGlobal_.reserve(reserveEvents * 30);
        batchGlobal_.reserve(512);
    }

    void beginMeasurement() {
        std::lock_guard lock{mutex_};
        eventWindow_.clear();
        batchWindow_.clear();
        pending_.clear();
        secondToSequence_.clear();
        quotesBySecond_.clear();
        callbacksWindow_ = negativeWindow_ = missingWindow_ = 0;
        measuring_ = true;
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
                auto [it, inserted] = pending_.try_emplace(markerSequence);
                it->second.timestamp = *timestamp;

                if (!it->second.firstObserved) {
                    it->second.firstObserved = observed;
                }

                it->second.lastObserved = std::max(it->second.lastObserved, observed);
                const auto second = *timestamp / 1'000'000'000;
                secondToSequence_[second] = markerSequence;

                // Quotes may arrive before the marker, so release those waiting for this publication second.
                if (auto quotes = quotesBySecond_.find(second); quotes != quotesBySecond_.end()) {
                    auto waiting = std::move(quotes->second);
                    quotesBySecond_.erase(quotes);

                    for (auto &quote : waiting) {
                        addEvent(markerSequence, std::move(quote));
                    }
                }

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
            } else if (description->kind == latency::EventKind::Quote && description->publishTime) {
                // Quote sequence and fractional time are not preserved by the tested scheme. At one batch per second,
                // the exchange-time second is an unambiguous synthetic correlation key.
                const auto second = *description->publishTime / 1'000'000'000;

                if (const auto known = secondToSequence_.find(second); known != secondToSequence_.end()) {
                    addEvent(known->second, std::move(pendingEvent));
                } else {
                    quotesBySecond_[second].push_back(std::move(pendingEvent));
                }
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
            } else
                ++it;
        }
        const auto cutoffSecond = cutoff / 1'000'000'000;
        std::erase_if(secondToSequence_, [&](const auto &item) {
            return item.first < cutoffSecond;
        });
        std::erase_if(quotesBySecond_, [&](const auto &item) {
            return item.first < cutoffSecond;
        });
        Window result{std::move(eventWindow_), std::move(batchWindow_), callbacksWindow_, negativeWindow_,
                      missingWindow_};
        eventWindow_.clear();
        batchWindow_.clear();
        callbacksWindow_ = negativeWindow_ = missingWindow_ = 0;
        return result;
    }

    Totals totals() const {
        std::lock_guard lock{mutex_};
        return Totals{eventGlobal_, batchGlobal_, callbacksTotal_, negativeTotal_, missingTotal_, pending_.size()};
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
    std::size_t expectedPerBatch_{};
    std::size_t windowIndex_{};

    static std::vector<std::int64_t> latencies(const std::vector<latency::Sample> &samples) {
        std::vector<std::int64_t> result;
        result.reserve(samples.size());

        for (const auto &sample : samples) {
            result.push_back(sample.latencyNs);
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
                      std::size_t callbacks, std::size_t negative, std::size_t missing) {
        summary_ << latency::utcTimestamp(start) << ',' << latency::utcTimestamp(end) << ',' << kind << ',' << s.count
                 << ',' << expectedPerBatch_ << ',' << callbacks << ',' << negative << ',' << missing << ','
                 << microseconds(s.minimum) << ',' << microseconds(s.mean) << ',' << microseconds(s.p50) << ','
                 << microseconds(s.p90) << ',' << microseconds(s.p95) << ',' << microseconds(s.p99) << ','
                 << microseconds(s.p999) << ',' << microseconds(s.maximum) << ',' << microseconds(s.q1) << ','
                 << microseconds(s.q3) << ',' << microseconds(s.iqr) << ',' << microseconds(s.outlierThreshold) << ','
                 << s.outlierCount << '\n';
    }
    void writeOutliers(const std::vector<latency::Sample> &samples, std::string_view kind,
                       const latency::Statistics &stats) {
        for (const auto &sample : samples) {
            if (latency::isUpperOutlier(sample.latencyNs, stats)) {
                outliers_ << latency::utcTimestamp(sample.observedAtNs) << ',' << kind << ','
                          << (kind == "event" ? latency::eventKindName(sample.kind) : "") << ',' << sample.symbol << ','
                          << sample.publishTimeNs << ',' << sample.latencyNs << ','
                          << static_cast<std::int64_t>(stats.outlierThreshold) << '\n';
            }
        }
    }

    public:
    // Opens both reports together so a run cannot proceed with only one of its outputs available.
    Reporter(const std::filesystem::path &prefix, std::size_t expectedPerBatch)
        : runStart_(latency::unixNanosNow()), expectedPerBatch_(expectedPerBatch) {
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

        summary_ << "window_start_utc,window_end_utc,sample_kind,samples,expected_per_batch,callbacks,clock_anomalies,"
                    "missing_batches,min_us,mean_us,p50_us,p90_us,p95_us,p99_us,p999_us,max_us,q1_us,q3_us,iqr_us,"
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
        printStats("batch", batchStats);
        writeSummary(start, end, "event", eventStats, data.callbacks, data.negative, data.missing);
        writeSummary(start, end, "batch", batchStats, data.callbacks, data.negative, data.missing);
        writeOutliers(data.events, "event", eventStats);
        writeOutliers(data.batches, "batch", batchStats);
        summary_.flush();
        outliers_.flush();
    }

    void final(const Collector::Totals &totals, std::int64_t end) {
        const auto eventStats = latency::calculateStatistics(totals.events);
        const auto batchStats = latency::calculateStatistics(totals.batches);
        std::cout << "Final summary callbacks=" << totals.callbacks << " clock-anomalies=" << totals.negative
                  << " missing-batches=" << totals.missing << " pending=" << totals.pending << '\n';
        printStats("event", eventStats);
        printStats("batch", batchStats);
        writeSummary(runStart_, end, "event-total", eventStats, totals.callbacks, totals.negative, totals.missing);
        writeSummary(runStart_, end, "batch-total", batchStats, totals.callbacks, totals.negative, totals.missing);
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
            throw std::invalid_argument("task parse error at " + std::to_string(pattern.error().position) + ": " +
                                        pattern.error().message);
        }

        const auto expected = pattern->eventCount();
        const auto reserve =
            expected * static_cast<std::size_t>(std::max<std::int64_t>(1, config.window.count() / 1000));
        Collector collector{expected, config.batchTimeout, reserve};
        Reporter reporter{config.output, expected};

        System::setProperty("dxscheme.nanoTime", "true");
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
        subscribe(Quote::TYPE, latency::EventKind::Quote);
        subscribe(Trade::TYPE, latency::EventKind::Trade);
        subscribe(Summary::TYPE, latency::EventKind::Summary);
        auto control = feed->createSubscription(TextMessage::TYPE);
        control->addEventListener([&collector](const auto &events) {
            collector.handle(events);
        });
        control->addSymbols(config.task);
        endpoint->connect(config.address);
        std::cout << "Connected to " << config.address << ", task " << config.task << ", expected " << expected
                  << " events/batch. Warm-up " << config.warmup.count() << " ms.\n";
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

        control->removeSymbols(config.task);
        const auto drainDeadline = std::chrono::steady_clock::now() + config.batchTimeout;

        // Give already-published batches a chance to complete after the control subscription has been removed.
        while (collector.pendingCount() && std::chrono::steady_clock::now() < drainDeadline && !interrupted.load()) {
            std::this_thread::sleep_for(100ms);
        }

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
