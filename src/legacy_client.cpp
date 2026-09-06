// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/core.hpp"

#include <DXFeed.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
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

/** Command-line settings for the legacy C API smoke client. */
struct Config {
    /** Address of the synthetic publisher endpoint. */
    std::string address{"127.0.0.1:7400"};

    /** Task whose event mask and symbol universe are subscribed. */
    std::string task{"SUB:T1@100ms"};

    /** Time for which callbacks are observed after the subscription is installed. */
    std::chrono::milliseconds duration{5s};

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

    /** Initial or updated Profile events. */
    std::atomic<std::size_t> profiles{};

    /** Largest `data_count` observed in one C API callback. */
    std::atomic<int> maximumBatch{};

    /** Whether the connection termination callback was invoked. */
    std::atomic<bool> terminated{};
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

    if (eventType == DXF_ET_PROFILE) {
        state.profiles.fetch_add(static_cast<std::size_t>(std::max(0, dataCount)), std::memory_order_relaxed);
    } else if (eventType == DXF_ET_QUOTE || eventType == DXF_ET_TRADE || eventType == DXF_ET_TRADE_ETH ||
               eventType == DXF_ET_SUMMARY) {
        state.recurringEvents.fetch_add(static_cast<std::size_t>(std::max(0, dataCount)), std::memory_order_relaxed);
    }
}

/** Parses and validates legacy smoke-client command-line arguments. */
Config parseArgs(int argc, char **argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            std::cout << R"(Usage: latency_legacy_client [options]
  --address 127.0.0.1:7400   synthetic publisher address
  --task SUB:T1@100ms        subscribed types and common symbol universe
  --duration 5s              callback observation period
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
        } else if (arg == "--duration") {
            const auto duration = latency::parseDuration(value);

            if (!duration) {
                throw std::invalid_argument(duration.error());
            }

            config.duration = *duration;
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

/** Waits for the requested observation period or an asynchronous stop condition. */
void observe(const Config &config, const CallbackState &state) {
    const auto deadline = std::chrono::steady_clock::now() + config.duration;

    while (!interrupted.load() && !state.terminated.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
}

} // namespace

/** Runs the legacy dxFeed C API comparison smoke client. */
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
                         "Starting dxFeed C API {} smoke client: task={}, symbols={}, nominal={:.3f} events/s\n",
                         LATENCY_DXFEED_C_API_VERSION, config.task, pattern->symbolCount(),
                         pattern->nominalEventsPerSecond())
                  << std::flush;
        session.start(config, *pattern);

        const auto started = std::chrono::steady_clock::now();

        observe(config, state);

        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto recurringEvents = state.recurringEvents.load();

        std::cout << std::format("Legacy summary: elapsed={:.3f}s callbacks={} recurring-events={} profiles={} "
                                 "maximum-data-count={} actual-events/s={:.3f}\n",
                                 elapsed, state.callbacks.load(), recurringEvents, state.profiles.load(),
                                 state.maximumBatch.load(), elapsed > 0 ? recurringEvents / elapsed : 0.0);

        if (interrupted.load()) {
            return 130;
        }

        if (state.terminated.load()) {
            return 1;
        }

        if (config.requireEvents && recurringEvents == 0) {
            std::cerr << "Legacy client did not receive any recurring events\n";

            return 1;
        }

        return 0;
    } catch (const std::exception &e) {
        std::cerr << std::format("Legacy client error: {}\n", e.what());

        return 1;
    }
}
