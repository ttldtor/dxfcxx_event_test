// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/core.hpp"
#include "latency/resources.hpp"

#include <dxfeed_graal_cpp_api/api.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace dxfcpp;

namespace {

std::atomic_bool interrupted{};
constexpr std::string_view MONITORING_STAT_PROPERTY = "monitoring.stat";

/** Selects the Graal endpoint role used for delivery-only observation. */
enum class ClientRole {
    /** Delivers every queued stream update. */
    STREAM_FEED,

    /** Retains current ticker state and permits intermediate-state supersession. */
    FEED
};

/** Command-line settings for the Graal delivery-only client. */
struct Config {
    /** Address of the synthetic publisher endpoint. */
    std::string address{"127.0.0.1:7400"};

    /** Task whose recurring event types and symbol universe are subscribed. */
    std::string task{"SUB:T1@100ms"};

    /** Warm-up period excluded from reported delivery counters. */
    std::chrono::milliseconds warmup{2s};

    /** Measurement period included in the delivery report. */
    std::chrono::milliseconds duration{5s};

    /** Maximum wait for initial Profiles and the first recurring event. */
    std::chrono::milliseconds startupTimeout{30s};

    /** Optional QD monitoring period applied through the endpoint builder. */
    std::optional<std::chrono::milliseconds> monitoringStat;

    /** Path and filename prefix for the delivery CSV. */
    std::filesystem::path output{"graal-delivery"};

    /** C++ API endpoint role. */
    ClientRole role{ClientRole::STREAM_FEED};

    /** Maximum native event-notification batch size. */
    std::int32_t eventsBatchLimit{DXFeedSubscription::OPTIMAL_BATCH_LIMIT};

    /** Subscription notification aggregation period. */
    std::chrono::milliseconds aggregationPeriod{};

    /** Whether a run without recurring events must fail. */
    bool requireEvents{};
};

/** Thread-safe callback counters collected without timestamp correlation or sample allocation. */
struct CallbackState {
    /** Total recurring listener invocations. */
    std::atomic<std::size_t> callbacks{};

    /** Total recurring Quote, Trade, TradeETH, and Summary events. */
    std::atomic<std::size_t> recurringEvents{};

    /** Recurring Quote events. */
    std::atomic<std::size_t> quotes{};

    /** Recurring Trade events. */
    std::atomic<std::size_t> trades{};

    /** Recurring TradeETH events. */
    std::atomic<std::size_t> tradeEths{};

    /** Recurring Summary events. */
    std::atomic<std::size_t> summaries{};

    /** Initial or updated Profile events. */
    std::atomic<std::size_t> profiles{};

    /** Largest recurring vector delivered in one C++ listener invocation. */
    std::atomic<std::size_t> maximumBatch{};
};

/** Immutable callback-counter snapshot used to isolate the measurement interval. */
struct CallbackSnapshot {
    std::size_t callbacks{};
    std::size_t recurringEvents{};
    std::size_t quotes{};
    std::size_t trades{};
    std::size_t tradeEths{};
    std::size_t summaries{};
    std::size_t profiles{};
};

/** Records an operating-system termination request. */
void onSignal(int) {
    interrupted.store(true);
}

/** Returns the stable command-line name of an endpoint role. */
std::string_view roleName(ClientRole role) {
    return role == ClientRole::FEED ? "feed" : "stream-feed";
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
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);

    if (error != std::errc{} || end != value.data() + value.size() || result <= 0) {
        throw std::invalid_argument(std::format("invalid events batch limit: {}", value));
    }

    return result;
}

/** Parses and validates Graal delivery-client command-line arguments. */
Config parseArgs(int argc, char **argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            std::cout << R"(Usage: latency_graal_delivery_client [options]
  --address 127.0.0.1:7400   synthetic publisher address
  --task SUB:T1@100ms        subscribed types and common symbol universe
  --role stream-feed         stream-feed or feed endpoint role
  --events-batch-limit optimal
                             optimal, maximum, or a positive integer
  --aggregation-period 0     listener notification aggregation period
  --monitoring-stat 10s      QD monitoring period; 0 disables it
  --warmup 2s                callback warm-up excluded from the report
  --duration 5s              measured callback observation period
  --startup-timeout 30s      wait for initial Profiles and recurring data
  --output graal-delivery    delivery CSV path and filename prefix
  --require-events           fail if no recurring event reaches the listener
)";
            std::exit(0);
        }

        if (arg == "--require-events") {
            config.requireEvents = true;

            continue;
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
        } else if (arg == "--role") {
            if (value == "stream-feed") {
                config.role = ClientRole::STREAM_FEED;
            } else if (value == "feed") {
                config.role = ClientRole::FEED;
            } else {
                throw std::invalid_argument(std::format("unknown role: {}", value));
            }
        } else if (arg == "--events-batch-limit") {
            config.eventsBatchLimit = parseEventsBatchLimit(value);
        } else if (arg == "--aggregation-period" && value == "0") {
            config.aggregationPeriod = 0ms;
        } else if (arg == "--monitoring-stat") {
            auto period = latency::parseMonitoringPeriod(value);

            if (!period) {
                throw std::invalid_argument(period.error());
            }

            config.monitoringStat = *period;
        } else if (arg == "--warmup" || arg == "--duration" || arg == "--startup-timeout" ||
                   arg == "--aggregation-period") {
            const auto duration = latency::parseDuration(value);

            if (!duration) {
                throw std::invalid_argument(duration.error());
            }

            if (arg == "--warmup") {
                config.warmup = *duration;
            } else if (arg == "--duration") {
                config.duration = *duration;
            } else if (arg == "--startup-timeout") {
                config.startupTimeout = *duration;
            } else {
                config.aggregationPeriod = *duration;
            }
        } else {
            throw std::invalid_argument(std::format("unknown argument: {}", arg));
        }
    }

    return config;
}

/** Atomically raises a maximum counter to at least the supplied value. */
void updateMaximum(std::atomic<std::size_t> &maximum, std::size_t value) {
    auto current = maximum.load(std::memory_order_relaxed);

    while (current < value &&
           !maximum.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

/** Counts one C++ API market-event callback with the minimum type inspection required for the report. */
void handleMarketEvents(CallbackState &state, const std::vector<std::shared_ptr<EventType>> &events) {
    state.callbacks.fetch_add(1, std::memory_order_relaxed);
    state.recurringEvents.fetch_add(events.size(), std::memory_order_relaxed);
    updateMaximum(state.maximumBatch, events.size());

    std::size_t quotes{};
    std::size_t trades{};
    std::size_t tradeEths{};
    std::size_t summaries{};

    for (const auto &event : events) {
        if (event->sharedAs<Quote>()) {
            ++quotes;
        } else if (event->sharedAs<Trade>()) {
            ++trades;
        } else if (event->sharedAs<TradeETH>()) {
            ++tradeEths;
        } else if (event->sharedAs<Summary>()) {
            ++summaries;
        }
    }

    state.quotes.fetch_add(quotes, std::memory_order_relaxed);
    state.trades.fetch_add(trades, std::memory_order_relaxed);
    state.tradeEths.fetch_add(tradeEths, std::memory_order_relaxed);
    state.summaries.fetch_add(summaries, std::memory_order_relaxed);
}

/** Returns a consistent-enough atomic snapshot for interval counter subtraction. */
CallbackSnapshot snapshot(const CallbackState &state) {
    return {state.callbacks.load(), state.recurringEvents.load(), state.quotes.load(),  state.trades.load(),
            state.tradeEths.load(), state.summaries.load(),       state.profiles.load()};
}

/** Subtracts callback counters captured at two measurement boundaries. */
CallbackSnapshot operator-(const CallbackSnapshot &end, const CallbackSnapshot &start) {
    return {end.callbacks - start.callbacks, end.recurringEvents - start.recurringEvents,
            end.quotes - start.quotes,       end.trades - start.trades,
            end.tradeEths - start.tradeEths, end.summaries - start.summaries,
            end.profiles - start.profiles};
}

/** Waits for a duration or an asynchronous stop condition. */
void observe(std::chrono::milliseconds duration, latency::ResourceSampler *resources = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + duration;

    while (!interrupted.load() && std::chrono::steady_clock::now() < deadline) {
        if (resources) {
            resources->sample();
        }

        std::this_thread::sleep_for(50ms);
    }

    if (resources) {
        resources->sample();
    }
}

/** Waits until subscription propagation produces all initial Profiles and recurring data. */
bool waitForStartup(const Config &config, const latency::TaskPattern &pattern, const CallbackState &state) {
    const auto deadline = std::chrono::steady_clock::now() + config.startupTimeout;

    while (!interrupted.load() && std::chrono::steady_clock::now() < deadline) {
        if (state.profiles.load() >= pattern.symbolCount() && state.recurringEvents.load() > 0) {
            return true;
        }

        std::this_thread::sleep_for(50ms);
    }

    return false;
}

/** Formats a system-clock time point as an ISO-8601 UTC timestamp with millisecond precision. */
std::string formatUtc(std::chrono::system_clock::time_point value) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fraction = milliseconds - seconds;
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point{seconds});
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

/** Writes one whole-run delivery row without performing timestamp-based E2E correlation. */
void writeDelivery(const Config &config, const latency::TaskPattern &pattern, const CallbackSnapshot &measured,
                   const latency::ResourceStatistics &resources, std::size_t maximumBatch,
                   std::chrono::system_clock::time_point start, std::chrono::system_clock::time_point end,
                   double elapsed) {
    auto path = config.output;
    path += "-delivery.csv";
    std::ofstream output{path};

    if (!output) {
        throw std::runtime_error(std::format("unable to write {}", path.string()));
    }

    output << "\"window_start_utc\",\"window_end_utc\",\"sample_kind\",\"expected_per_batch\","
              "\"nominal_events_per_second\",\"callbacks\",\"recurring_events\",\"quote\",\"trade\","
              "\"trade_eth\",\"summary\",\"profiles\",\"maximum_data_count\","
              "\"actual_events_per_second\",\"cpu_core_percent\",\"cpu_host_percent\","
              "\"rss_mean_bytes\",\"rss_maximum_bytes\",\"resource_samples\",\"implementation\",\"contract\"\n";
    output << std::format(
        "\"{}\",\"{}\",\"event\",{},{:.3f},{},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{},{},{},\"graal\",\"{}\"\n",
        formatUtc(start), formatUtc(end), pattern.eventCount(), pattern.nominalEventsPerSecond(), measured.callbacks,
        measured.recurringEvents, measured.quotes, measured.trades, measured.tradeEths, measured.summaries,
        measured.profiles, maximumBatch, elapsed > 0 ? measured.recurringEvents / elapsed : 0.0,
        resources.cpuCorePercent, resources.cpuHostPercent, resources.rssMeanBytes, resources.rssMaximumBytes,
        resources.samples, roleName(config.role));
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

} // namespace

/** Runs the dxFeed Graal C++ API delivery-only comparison client. */
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

        if (pattern->quantity(latency::EventKind::TIME_AND_SALE).value_or(0)) {
            throw std::invalid_argument("TimeAndSale is not supported by the delivery-only client");
        }

        CallbackState state;
        const auto endpointRole =
            config.role == ClientRole::FEED ? DXEndpoint::Role::FEED : DXEndpoint::Role::STREAM_FEED;
        const auto builder =
            DXEndpoint::newBuilder()->withRole(endpointRole)->withName("latency-graal-delivery-client");

        configureMonitoring(builder, config.monitoringStat);

        const auto endpoint = builder->build();
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

        if (eventTypes.empty()) {
            throw std::invalid_argument("task has no delivery-only recurring event type");
        }

        auto market = feed->createSubscription(eventTypes);
        market->setEventsBatchLimit(config.eventsBatchLimit);
        market->setAggregationPeriod(config.aggregationPeriod);
        market->addEventListener([&state](const auto &events) {
            handleMarketEvents(state, events);
        });
        market->addSymbols(pattern->marketSymbols());

        auto profiles = feed->createSubscription(Profile::TYPE);
        profiles->addEventListener([&state](const auto &events) {
            state.profiles.fetch_add(events.size(), std::memory_order_relaxed);
        });
        profiles->addSymbols(pattern->symbols());

        endpoint->connect(config.address);
        std::cout << std::format("Starting Graal delivery-only client: role={}, task={}, base-symbols={}, "
                                 "market-symbols={}, nominal={:.3f} events/s, batch-limit={}, aggregation={} ms\n",
                                 roleName(config.role), config.task, pattern->symbolCount(),
                                 pattern->marketSymbols().size(), pattern->nominalEventsPerSecond(),
                                 market->getEventsBatchLimit(), market->getAggregationPeriod().getTime())
                  << std::flush;

        if (!waitForStartup(config, *pattern, state)) {
            endpoint->closeAndAwaitTermination();
            throw std::runtime_error(std::format("startup timed out: profiles={}/{} recurring-events={}",
                                                 state.profiles.load(), pattern->symbolCount(),
                                                 state.recurringEvents.load()));
        }

        std::cout << std::format("Initial Profile setup complete: {}/{}. Warm-up {} ms.\n", state.profiles.load(),
                                 pattern->symbolCount(), config.warmup.count())
                  << std::flush;
        observe(config.warmup);

        const auto before = snapshot(state);
        const auto startedSteady = std::chrono::steady_clock::now();
        const auto startedWall = std::chrono::system_clock::now();
        latency::ResourceSampler resources;

        observe(config.duration, &resources);

        const auto endedWall = std::chrono::system_clock::now();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startedSteady).count();
        const auto measured = snapshot(state) - before;
        const auto resourceStatistics = resources.finish(elapsed);

        writeDelivery(config, *pattern, measured, resourceStatistics, state.maximumBatch.load(), startedWall, endedWall,
                      elapsed);
        endpoint->closeAndAwaitTermination();

        std::cout << std::format("Graal delivery summary: elapsed={:.3f}s callbacks={} recurring-events={} profiles={} "
                                 "maximum-batch={} actual-events/s={:.3f}\n",
                                 elapsed, measured.callbacks, measured.recurringEvents, measured.profiles,
                                 state.maximumBatch.load(), elapsed > 0 ? measured.recurringEvents / elapsed : 0.0);
        std::cout << std::format("Graal delivery resources: cpu-core={:.3f}% cpu-host={:.3f}% "
                                 "rss-mean={:.3f} MiB rss-maximum={:.3f} MiB samples={}\n",
                                 resourceStatistics.cpuCorePercent, resourceStatistics.cpuHostPercent,
                                 resourceStatistics.rssMeanBytes / 1'048'576.0,
                                 resourceStatistics.rssMaximumBytes / 1'048'576.0, resourceStatistics.samples);

        if (interrupted.load()) {
            return 130;
        }

        if (config.requireEvents && measured.recurringEvents == 0) {
            std::cerr << "Graal delivery-only client did not receive any recurring events\n";

            return 1;
        }

        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("Graal delivery client error: {}\n", e.what());

        return 1;
    }
}
