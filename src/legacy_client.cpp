// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/core.hpp"
#include "latency/resources.hpp"

#include <DXFeed.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::atomic<bool> interrupted{};

/** Identifies the subscription contract requested from the legacy C API. */
enum class Contract {
    /** Lets the C API select its normal contract for each record. */
    DEFAULT,

    /** Forces the ticker contract. */
    TICKER,

    /** Forces the stream contract. */
    STREAM
};

/** Command-line settings for the legacy C API delivery client. */
struct Config {
    /** Address of the synthetic publisher endpoint. */
    std::string address{"127.0.0.1:7400"};

    /** Task whose event mask and symbol universe are subscribed. */
    std::string task{"SUB:T1@100ms"};

    /** Warm-up period excluded from reported delivery counters. */
    std::chrono::milliseconds warmup{2s};

    /** Measurement period included in the delivery report. */
    std::chrono::milliseconds duration{5s};

    /** Maximum wait for initial Profiles and the first recurring event. */
    std::chrono::milliseconds startupTimeout{30s};

    /** Path and filename prefix for the delivery CSV. */
    std::filesystem::path output{"legacy"};

    /** C API subscription contract. */
    Contract contract{Contract::DEFAULT};

    /** Whether a run without recurring events must fail. */
    bool requireEvents{};
};

/** Thread-safe callback counters collected without interpreting event timestamps. */
struct CallbackState {
    /** Total listener invocations. */
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

    /** Largest `data_count` observed in one C API callback. */
    std::atomic<int> maximumBatch{};

    /** Whether the connection termination callback was invoked. */
    std::atomic<bool> terminated{};
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

/** Converts the legacy API's wide diagnostic text to printable ASCII/UTF-8-safe text. */
std::string narrow(dxf_const_string_t value) {
    if (!value) {
        return {};
    }

    std::string result;

    for (; *value; ++value) {
        const auto character = static_cast<unsigned long>(*value);

        result.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
    }

    return result;
}

/** Returns the last legacy C API error as one diagnostic string. */
std::string lastApiError() {
    int code{};
    dxf_const_string_t description{};

    if (dxf_get_last_error(&code, &description) != DXF_SUCCESS) {
        return "unable to retrieve the dxFeed C API error";
    }

    return std::format("dxFeed C API error {}: {}", code, narrow(description));
}

/** Throws when a legacy C API operation fails. */
void checkApi(ERRORCODE result, std::string_view operation) {
    if (result != DXF_SUCCESS) {
        throw std::runtime_error(std::format("{} failed: {}", operation, lastApiError()));
    }
}

/** Returns a stable display name for a legacy connection status. */
std::string_view connectionStatusName(dxf_connection_status_t status) {
    switch (status) {
    case dxf_cs_not_connected:
        return "not-connected";
    case dxf_cs_connected:
        return "connected";
    case dxf_cs_login_required:
        return "login-required";
    case dxf_cs_authorized:
        return "authorized";
    }

    return "unknown";
}

/** Reports an asynchronous legacy C API connection-state transition. */
void onConnectionStatus(dxf_connection_t, dxf_connection_status_t oldStatus, dxf_connection_status_t newStatus,
                        void *) {
    std::osyncstream{std::cout} << std::format("Legacy connection: {} -> {}\n", connectionStatusName(oldStatus),
                                               connectionStatusName(newStatus));
}

/** Records asynchronous termination of the legacy connection. */
void onConnectionTerminated(dxf_connection_t, void *userData) {
    auto &state = *static_cast<CallbackState *>(userData);

    state.terminated.store(true);
    std::osyncstream{std::cerr} << "Legacy connection terminated\n";
}

/** Atomically raises a maximum counter to at least the supplied value. */
void updateMaximum(std::atomic<int> &maximum, int value) {
    auto current = maximum.load(std::memory_order_relaxed);

    while (current < value &&
           !maximum.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

/** Counts one legacy event callback and preserves its actual `data_count`. */
void onEvents(int eventType, dxf_const_string_t, const dxf_event_data_t *, int dataCount, const dxf_event_params_t *,
              void *userData) {
    auto &state = *static_cast<CallbackState *>(userData);

    state.callbacks.fetch_add(1, std::memory_order_relaxed);
    updateMaximum(state.maximumBatch, dataCount);

    const auto count = static_cast<std::size_t>(std::max(0, dataCount));

    if (eventType == DXF_ET_PROFILE) {
        state.profiles.fetch_add(count, std::memory_order_relaxed);
    } else {
        state.recurringEvents.fetch_add(count, std::memory_order_relaxed);

        if (eventType == DXF_ET_QUOTE) {
            state.quotes.fetch_add(count, std::memory_order_relaxed);
        } else if (eventType == DXF_ET_TRADE) {
            state.trades.fetch_add(count, std::memory_order_relaxed);
        } else if (eventType == DXF_ET_TRADE_ETH) {
            state.tradeEths.fetch_add(count, std::memory_order_relaxed);
        } else if (eventType == DXF_ET_SUMMARY) {
            state.summaries.fetch_add(count, std::memory_order_relaxed);
        }
    }
}

/** Parses and validates legacy delivery-client command-line arguments. */
Config parseArgs(int argc, char **argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            std::cout << R"(Usage: latency_legacy_client [options]
  --address 127.0.0.1:7400   synthetic publisher address
  --task SUB:T1@100ms        subscribed types and common symbol universe
  --warmup 2s                callback warm-up excluded from the report
  --duration 5s              measured callback observation period
  --startup-timeout 30s      wait for initial Profiles and recurring data
  --output legacy            delivery CSV path and filename prefix
  --contract default         default, ticker, or stream
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
        } else if (arg == "--warmup" || arg == "--duration" || arg == "--startup-timeout") {
            const auto duration = latency::parseDuration(value);

            if (!duration) {
                throw std::invalid_argument(duration.error());
            }

            if (arg == "--warmup") {
                config.warmup = *duration;
            } else if (arg == "--duration") {
                config.duration = *duration;
            } else {
                config.startupTimeout = *duration;
            }
        } else if (arg == "--output") {
            config.output = value;
        } else if (arg == "--contract") {
            if (value == "default") {
                config.contract = Contract::DEFAULT;
            } else if (value == "ticker") {
                config.contract = Contract::TICKER;
            } else if (value == "stream") {
                config.contract = Contract::STREAM;
            } else {
                throw std::invalid_argument(std::format("unknown subscription contract: {}", value));
            }
        } else {
            throw std::invalid_argument(std::format("unknown argument: {}", arg));
        }
    }

    return config;
}

/** Builds the legacy C API event-type bitmask required by a task. */
int eventTypeMask(const latency::TaskPattern &pattern) {
    auto mask = DXF_ET_PROFILE;

    for (const auto &item : pattern.items) {
        switch (item.kind) {
        case latency::EventKind::QUOTE:
            mask |= DXF_ET_QUOTE;
            break;
        case latency::EventKind::TRADE:
            mask |= DXF_ET_TRADE;
            break;
        case latency::EventKind::TRADE_ETH:
            mask |= DXF_ET_TRADE_ETH;
            break;
        case latency::EventKind::SUMMARY:
            mask |= DXF_ET_SUMMARY;
            break;
        }
    }

    return mask;
}

/** Converts the configured subscription contract to the corresponding C API flag. */
dx_event_subscr_flag subscriptionFlag(Contract contract) {
    switch (contract) {
    case Contract::DEFAULT:
        return dx_esf_default;
    case Contract::TICKER:
        return dx_esf_force_ticker;
    case Contract::STREAM:
        return dx_esf_force_stream;
    }

    return dx_esf_default;
}

/** Owns a legacy C API connection and its combined subscription. */
class Session final {
    CallbackState &state_;
    dxf_connection_t connection_{};
    dxf_subscription_t subscription_{};

    public:
    /** Creates an empty session whose handles are safe to close during unwinding. */
    explicit Session(CallbackState &state) : state_(state) {
    }

    /** Disallows copying a native-handle owner. */
    Session(const Session &) = delete;

    /** Disallows copy assignment of a native-handle owner. */
    Session &operator=(const Session &) = delete;

    /** Closes the subscription before closing its owning connection. */
    ~Session() {
        if (subscription_) {
            dxf_close_subscription(subscription_);
        }

        if (connection_) {
            dxf_close_connection(connection_);
        }
    }

    /** Connects, creates the combined subscription, attaches the callback, and adds all symbols. */
    void start(const Config &config, const latency::TaskPattern &pattern) {
        checkApi(dxf_create_connection(config.address.c_str(), onConnectionTerminated, onConnectionStatus, nullptr,
                                       nullptr, &state_, &connection_),
                 "dxf_create_connection");

        const auto mask = eventTypeMask(pattern);

        if (config.contract == Contract::DEFAULT) {
            checkApi(dxf_create_subscription(connection_, mask, &subscription_), "dxf_create_subscription");
        } else {
            checkApi(dxf_create_subscription_with_flags(connection_, mask, subscriptionFlag(config.contract),
                                                        &subscription_),
                     "dxf_create_subscription_with_flags");
        }

        checkApi(dxf_attach_event_listener_v2(subscription_, onEvents, &state_), "dxf_attach_event_listener_v2");

        const auto narrowSymbols = pattern.symbols();
        std::vector<std::wstring> wideSymbols;
        std::vector<dxf_const_string_t> symbolPointers;

        wideSymbols.reserve(narrowSymbols.size());
        symbolPointers.reserve(narrowSymbols.size());

        for (const auto &symbol : narrowSymbols) {
            wideSymbols.emplace_back(symbol.begin(), symbol.end());
        }

        for (const auto &symbol : wideSymbols) {
            symbolPointers.push_back(symbol.c_str());
        }

        if (symbolPointers.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("too many symbols for the dxFeed C API");
        }

        checkApi(dxf_add_symbols(subscription_, symbolPointers.data(), static_cast<int>(symbolPointers.size())),
                 "dxf_add_symbols");
    }
};

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
void observe(std::chrono::milliseconds duration, const CallbackState &state,
             latency::ResourceSampler *resources = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + duration;

    while (!interrupted.load() && !state.terminated.load() && std::chrono::steady_clock::now() < deadline) {
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

    while (!interrupted.load() && !state.terminated.load() && std::chrono::steady_clock::now() < deadline) {
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

/** Returns the stable command-line name of a legacy subscription contract. */
std::string_view contractName(Contract contract) {
    switch (contract) {
    case Contract::DEFAULT:
        return "default";
    case Contract::TICKER:
        return "ticker";
    case Contract::STREAM:
        return "stream";
    }

    return "unknown";
}

/** Writes one whole-run delivery row without claiming timestamp-based E2E latency. */
void writeDelivery(const Config &config, const latency::TaskPattern &pattern, const CallbackSnapshot &measured,
                   const latency::ResourceStatistics &resources, int maximumBatch,
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
              "\"rss_mean_bytes\",\"rss_maximum_bytes\",\"resource_samples\",\"contract\"\n";
    output << std::format(
        "\"{}\",\"{}\",\"event\",{},{:.3f},{},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{},{},{},\"{}\"\n",
        formatUtc(start), formatUtc(end), pattern.eventCount(), pattern.nominalEventsPerSecond(), measured.callbacks,
        measured.recurringEvents, measured.quotes, measured.trades, measured.tradeEths, measured.summaries,
        measured.profiles, maximumBatch, elapsed > 0 ? measured.recurringEvents / elapsed : 0.0,
        resources.cpuCorePercent, resources.cpuHostPercent, resources.rssMeanBytes, resources.rssMaximumBytes,
        resources.samples, contractName(config.contract));
}

} // namespace

/** Runs the legacy dxFeed C API comparison delivery client. */
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

        CallbackState state;
        Session session{state};

        std::cout << std::format(
                         "Starting dxFeed C API {} delivery client: task={}, symbols={}, nominal={:.3f} events/s\n",
                         LATENCY_DXFEED_C_API_VERSION, config.task, pattern->symbolCount(),
                         pattern->nominalEventsPerSecond())
                  << std::flush;
        session.start(config, *pattern);

        if (!waitForStartup(config, *pattern, state)) {
            throw std::runtime_error(std::format("startup timed out: profiles={}/{} recurring-events={}",
                                                 state.profiles.load(), pattern->symbolCount(),
                                                 state.recurringEvents.load()));
        }

        std::cout << std::format("Initial Profile setup complete: {}/{}. Warm-up {} ms.\n", state.profiles.load(),
                                 pattern->symbolCount(), config.warmup.count())
                  << std::flush;
        observe(config.warmup, state);

        const auto before = snapshot(state);
        const auto startedSteady = std::chrono::steady_clock::now();
        const auto startedWall = std::chrono::system_clock::now();
        latency::ResourceSampler resources;

        observe(config.duration, state, &resources);

        const auto endedWall = std::chrono::system_clock::now();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startedSteady).count();
        const auto measured = snapshot(state) - before;
        const auto resourceStatistics = resources.finish(elapsed);

        writeDelivery(config, *pattern, measured, resourceStatistics, state.maximumBatch.load(), startedWall, endedWall,
                      elapsed);

        std::cout << std::format("Legacy summary: elapsed={:.3f}s callbacks={} recurring-events={} profiles={} "
                                 "maximum-data-count={} actual-events/s={:.3f}\n",
                                 elapsed, measured.callbacks, measured.recurringEvents, measured.profiles,
                                 state.maximumBatch.load(), elapsed > 0 ? measured.recurringEvents / elapsed : 0.0);
        std::cout << std::format("Legacy resources: cpu-core={:.3f}% cpu-host={:.3f}% rss-mean={:.3f} MiB "
                                 "rss-maximum={:.3f} MiB samples={}\n",
                                 resourceStatistics.cpuCorePercent, resourceStatistics.cpuHostPercent,
                                 resourceStatistics.rssMeanBytes / 1'048'576.0,
                                 resourceStatistics.rssMaximumBytes / 1'048'576.0, resourceStatistics.samples);

        if (interrupted.load()) {
            return 130;
        }

        if (state.terminated.load()) {
            return 1;
        }

        if (config.requireEvents && measured.recurringEvents == 0) {
            std::cerr << "Legacy client did not receive any recurring events\n";

            return 1;
        }

        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("Legacy client error: {}\n", e.what());

        return 1;
    }
}
