// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/core.hpp"

#include <dxfeed_graal_cpp_api/api.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;
using namespace dxfcpp;

namespace {
std::atomic_bool interrupted{};
constexpr std::string_view MONITORING_STAT_PROPERTY = "monitoring.stat";

/** Records an operating-system termination request. */
void onSignal(int) {
    interrupted.store(true);
}

/** Identifies a publisher-side observable subscription. */
enum class SubscriptionKind {
    /** Quote subscription. */
    QUOTE,

    /** Trade subscription. */
    TRADE,

    /** TradeETH subscription. */
    TRADE_ETH,

    /** Summary subscription. */
    SUMMARY,

    /** Initial Profile subscription. */
    PROFILE
};

/** Identifies a state transition consumed by the generator worker. */
enum class CommandType {
    /** Starts a task after all required subscriptions become ready. */
    START,

    /** Stops the active task. */
    STOP,

    /** Adds symbols to an observed subscription. */
    SYMBOLS_ADDED,

    /** Removes symbols from an observed subscription. */
    SYMBOLS_REMOVED,

    /** Clears all symbols for a closed subscription. */
    SUBSCRIPTION_CLOSED
};

/** Carries one control or subscription update to the generator worker. */
struct Command {
    CommandType type{};
    std::string text;
    SubscriptionKind subscription{};
    std::vector<std::string> symbols;
};

/** Returns the storage index assigned to an observable subscription kind. */
constexpr std::size_t subscriptionKindIndex(SubscriptionKind kind) {
    return static_cast<std::size_t>(kind);
}

/** Returns the display name assigned to an observable publisher subscription. */
std::string_view subscriptionKindName(SubscriptionKind kind) {
    switch (kind) {
    case SubscriptionKind::QUOTE:
        return "Quote";
    case SubscriptionKind::TRADE:
        return "Trade";
    case SubscriptionKind::TRADE_ETH:
        return "TradeETH";
    case SubscriptionKind::SUMMARY:
        return "Summary";
    case SubscriptionKind::PROFILE:
        return "Profile";
    }

    return "Unknown";
}

/** Maps a generated market event kind to its publisher subscription kind. */
SubscriptionKind subscriptionKind(latency::EventKind kind) {
    switch (kind) {
    case latency::EventKind::QUOTE:
        return SubscriptionKind::QUOTE;
    case latency::EventKind::TRADE:
        return SubscriptionKind::TRADE;
    case latency::EventKind::TRADE_ETH:
        return SubscriptionKind::TRADE_ETH;
    case latency::EventKind::SUMMARY:
        return SubscriptionKind::SUMMARY;
    }

    throw std::invalid_argument("unknown event kind");
}

/** Owns the worker thread that turns subscription changes into a single active publishing task. */
class Generator {
    std::shared_ptr<DXPublisher> publisher_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    std::array<std::unordered_set<std::string>, 5> subscribedSymbols_;
    bool traceSubscriptions_{};
    bool stopping_{};
    std::thread thread_;

    /** Owns reusable synthetic events of one market event kind. */
    struct EventPool {
        latency::EventKind kind{};
        std::size_t quantity{};
        std::vector<std::shared_ptr<EventType>> events;
    };

    /** Holds the state and timing counters of the currently published task. */
    struct ActiveTask {
        std::string command;
        latency::TaskPattern pattern;
        std::vector<EventPool> pools;
        std::shared_ptr<TextMessage> marker;
        std::vector<std::shared_ptr<EventType>> publication;
        std::vector<std::size_t> poolOrder;
        std::array<std::uint64_t, 4> lastBlockCounts{};
        std::uint32_t tick{};
        std::uint64_t publications{}, skippedDeadlines{};
        std::chrono::nanoseconds preparationTotal{}, preparationMaximum{};
        std::chrono::nanoseconds publishTotal{}, publishMaximum{};
        std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    };

    /** Holds a parsed task until every required subscription becomes ready. */
    struct PendingTask {
        std::string command;
        latency::TaskPattern pattern;
        std::vector<std::string> symbols;
    };

    bool containsAll(SubscriptionKind kind, const std::vector<std::string> &symbols) const {
        const auto &subscribed = subscribedSymbols_[subscriptionKindIndex(kind)];

        return std::ranges::all_of(symbols, [&subscribed](const auto &symbol) {
            return subscribed.contains(symbol);
        });
    }

    bool subscriptionsReady(const PendingTask &pending) const {
        if (!containsAll(SubscriptionKind::PROFILE, pending.symbols)) {
            return false;
        }

        return std::ranges::all_of(pending.pattern.items, [this, &pending](const auto &item) {
            return containsAll(subscriptionKind(item.kind), pending.symbols);
        });
    }

    static std::shared_ptr<EventType> makeEvent(latency::EventKind kind, const std::string &symbol) {
        switch (kind) {
        case latency::EventKind::QUOTE: {
            auto event = std::make_shared<Quote>(symbol);
            event->setBidExchangeCode('B');
            event->setAskExchangeCode('A');
            event->setBidSize(1);
            event->setAskSize(1);

            return event;
        }
        case latency::EventKind::TRADE: {
            auto event = std::make_shared<Trade>(symbol);
            event->setSize(1);

            return event;
        }
        case latency::EventKind::TRADE_ETH: {
            auto event = std::make_shared<TradeETH>(symbol);
            event->setSize(1);

            return event;
        }
        case latency::EventKind::SUMMARY: {
            auto event = std::make_shared<Summary>(symbol);
            event->setDayId(1);

            return event;
        }
        }

        throw std::invalid_argument("unknown event kind");
    }

    // Allocate one stable event object per subscribed record key. Each publication selects a rotating subset.
    static std::vector<EventPool> makeEventPools(const latency::TaskPattern &pattern) {
        std::vector<EventPool> pools;
        const auto symbols = pattern.symbols();

        pools.reserve(pattern.items.size());

        for (const auto &item : pattern.items) {
            EventPool pool{item.kind, item.quantity};

            pool.events.reserve(symbols.size());

            for (const auto &symbol : symbols) {
                pool.events.push_back(makeEvent(item.kind, symbol));
            }

            pools.push_back(std::move(pool));
        }

        return pools;
    }

    static std::vector<std::shared_ptr<EventType>> makeInitialEvents(const latency::TaskPattern &pattern) {
        std::vector<std::shared_ptr<EventType>> events;

        events.reserve(pattern.symbolCount());

        for (const auto &symbol : pattern.symbols()) {
            events.push_back(std::make_shared<Profile>(symbol));
        }

        return events;
    }

    static std::uint64_t nextRandom(std::uint64_t &state) {
        state += 0x9e3779b97f4a7c15ULL;
        auto value = state;

        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;

        return value ^ (value >> 31U);
    }

    void publish(ActiveTask &active) {
        const auto preparationStart = std::chrono::steady_clock::now();
        ++active.tick;
        const auto sequence = static_cast<std::int32_t>(active.tick % TextMessage::MAX_SEQUENCE);

        active.publication.clear();
        std::iota(active.poolOrder.begin(), active.poolOrder.end(), 0);

        if (active.pattern.shuffleSeed) {
            auto randomState = *active.pattern.shuffleSeed ^ active.publications;

            for (std::size_t i = active.poolOrder.size(); i > 1; --i) {
                const auto selected = static_cast<std::size_t>(nextRandom(randomState) % i);

                std::swap(active.poolOrder[i - 1], active.poolOrder[selected]);
            }
        }

        const auto lastKind = active.pools[active.poolOrder.back()].kind;

        ++active.lastBlockCounts[subscriptionKindIndex(subscriptionKind(lastKind))];

        // Event fields identify the batch. A larger symbol universe changes record-key cardinality, not batch size.
        for (const auto poolIndex : active.poolOrder) {
            const auto &pool = active.pools[poolIndex];
            const auto first = active.publications * pool.quantity % pool.events.size();

            for (std::size_t i = 0; i < pool.quantity; ++i) {
                const auto &event = pool.events[(first + i) % pool.events.size()];

                if (auto quote = event->sharedAs<Quote>()) {
                    quote->setBidSize(sequence);
                    quote->setAskSize(1);
                    quote->setBidPrice(100.0 + active.tick % 100);
                    quote->setAskPrice(100.01 + active.tick % 100);
                } else if (auto trade = event->sharedAs<Trade>()) {
                    trade->setPrice(100.0 + active.tick % 100);
                    trade->setSize(1);
                    trade->setSequence(sequence);
                } else if (auto tradeEth = event->sharedAs<TradeETH>()) {
                    tradeEth->setPrice(100.0 + active.tick % 100);
                    tradeEth->setSize(1);
                    tradeEth->setSequence(sequence);
                } else if (auto summary = event->sharedAs<Summary>()) {
                    summary->setDayId(sequence);
                    summary->setDayOpenPrice(99);
                    summary->setDayHighPrice(101 + active.tick % 100);
                    summary->setDayLowPrice(98);
                    summary->setDayClosePrice(100 + active.tick % 100);
                }

                active.publication.push_back(event);
            }
        }

        const auto nowNs = latency::unixNanosNow();
        active.marker->setTime(nowNs / 1'000'000);
        active.marker->setSequence(sequence);
        active.marker->setText(std::format("LATENCY_BATCH:{}", nowNs));
        active.publication.push_back(active.marker);
        const auto publishStart = std::chrono::steady_clock::now();
        const auto preparation = std::chrono::duration_cast<std::chrono::nanoseconds>(publishStart - preparationStart);
        publisher_->publishEvents(active.publication);
        const auto publishDuration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - publishStart);
        ++active.publications;
        active.preparationTotal += preparation;
        active.preparationMaximum = std::max(active.preparationMaximum, preparation);
        active.publishTotal += publishDuration;
        active.publishMaximum = std::max(active.publishMaximum, publishDuration);
    }

    static void logSummary(const ActiveTask &active) {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - active.started).count();
        const auto publications = std::max<std::uint64_t>(1, active.publications);
        const auto milliseconds = [](std::chrono::nanoseconds value) {
            return std::chrono::duration<double, std::milli>(value).count();
        };
        const auto actualBatchesPerSecond = elapsed > 0 ? active.publications / elapsed : 0.0;
        const auto actualEventsPerSecond =
            elapsed > 0 ? active.publications * active.pattern.eventCount() / elapsed : 0.0;

        std::cout << std::format("Generator summary {}: publications={} skipped-deadlines={} "
                                 "actual-batches/s={:.3f} actual-events/s={:.3f} "
                                 "preparation-ms(avg/max)={:.3f}/{:.3f} publish-ms(avg/max)={:.3f}/{:.3f} "
                                 "last-blocks(Q/T/E/S)={}/{}/{}/{}\n",
                                 active.command, active.publications, active.skippedDeadlines, actualBatchesPerSecond,
                                 actualEventsPerSecond, milliseconds(active.preparationTotal) / publications,
                                 milliseconds(active.preparationMaximum),
                                 milliseconds(active.publishTotal) / publications, milliseconds(active.publishMaximum),
                                 active.lastBlockCounts[0], active.lastBlockCounts[1], active.lastBlockCounts[2],
                                 active.lastBlockCounts[3])
                  << std::flush;
    }

    void run() {
        constexpr auto SPIN_WINDOW = 20ms;
        std::optional<ActiveTask> active;
        std::optional<PendingTask> pending;
        auto nextTick = std::chrono::steady_clock::now();

        const auto startWhenReady = [&] {
            if (!pending || active || !subscriptionsReady(*pending)) {
                return;
            }

            try {
                const auto initialEvents = makeInitialEvents(pending->pattern);

                if (!initialEvents.empty()) {
                    publisher_->publishEvents(initialEvents);
                    std::cout << std::format("Published {} initial Profile events for {}\n", initialEvents.size(),
                                             pending->command);
                }

                auto publication = std::vector<std::shared_ptr<EventType>>{};
                auto pools = makeEventPools(pending->pattern);
                auto poolOrder = std::vector<std::size_t>(pools.size());

                publication.reserve(pending->pattern.eventCount() + 1);
                std::iota(poolOrder.begin(), poolOrder.end(), 0);
                active.emplace(
                    ActiveTask{pending->command, pending->pattern, std::move(pools),
                               std::make_shared<TextMessage>(latency::markerSymbol(pending->command), "LATENCY_BATCH"),
                               std::move(publication), std::move(poolOrder)});
                nextTick = std::chrono::steady_clock::now();
                std::cout << std::format("Subscriptions ready for {} symbols. Started {} ({} events/batch, "
                                         "period={} ms, nominal={:.3f} events/s)\n",
                                         pending->symbols.size(), pending->command, active->pattern.eventCount(),
                                         active->pattern.publishPeriod.count(),
                                         active->pattern.nominalEventsPerSecond())
                          << std::flush;
                pending.reset();
            } catch (const std::exception &e) {
                std::cerr << "Cannot start task: " << e.what() << '\n';
                pending.reset();
            }
        };

        // Subscription callbacks only enqueue commands. Parsing and publishing remain serialized on this thread.
        for (;;) {
            std::unique_lock lock{mutex_};

            if (!active) {
                cv_.wait(lock, [&] {
                    return stopping_ || !commands_.empty();
                });
            } else {
                const auto now = std::chrono::steady_clock::now();

                if (nextTick - now > SPIN_WINDOW) {
                    cv_.wait_until(lock, nextTick - SPIN_WINDOW, [&] {
                        return stopping_ || !commands_.empty();
                    });
                }

                if (!stopping_ && commands_.empty() && std::chrono::steady_clock::now() < nextTick) {
                    // Windows timed condition-variable waits can be quantized to about 15.6 ms. Yielding near the
                    // deadline keeps 10 ms and 1 ms benchmark cadences portable without changing global timer state.
                    lock.unlock();

                    while (std::chrono::steady_clock::now() < nextTick) {
                        std::this_thread::yield();
                    }

                    lock.lock();
                }
            }

            if (stopping_) {
                if (active) {
                    logSummary(*active);
                }

                return;
            }

            if (!commands_.empty()) {
                auto command = std::move(commands_.front());

                commands_.pop_front();
                lock.unlock();

                if (command.type == CommandType::START) {
                    const auto parsed = latency::parseTask(command.text);

                    if (!parsed) {
                        std::cerr << "Rejected task at " << parsed.error().position << ": " << parsed.error().message
                                  << " [" << command.text << "]\n";
                    } else if (!active && !pending) {
                        pending.emplace(PendingTask{command.text, *parsed, parsed->symbols()});
                        std::cout << std::format("Waiting for subscriptions before starting {} ({} symbols)\n",
                                                 command.text, pending->symbols.size())
                                  << std::flush;
                    } else if ((active && active->command != command.text) ||
                               (pending && pending->command != command.text)) {
                        // The protocol intentionally supports one active load profile at a time.
                        const auto &current = active ? active->command : pending->command;

                        std::cerr << "Rejected concurrent task " << command.text << "; current task is " << current
                                  << '\n';
                    }
                } else if (command.type == CommandType::STOP) {
                    if (pending && (command.text.empty() || pending->command == command.text)) {
                        std::cout << "Cancelled pending task " << pending->command << '\n';
                        pending.reset();
                    }

                    if (active && (command.text.empty() || active->command == command.text)) {
                        logSummary(*active);
                        std::cout << "Stopped " << active->command << '\n';
                        active.reset();
                    }
                } else {
                    auto &subscribed = subscribedSymbols_[subscriptionKindIndex(command.subscription)];

                    if (command.type == CommandType::SYMBOLS_ADDED) {
                        subscribed.insert(command.symbols.begin(), command.symbols.end());
                    } else if (command.type == CommandType::SYMBOLS_REMOVED) {
                        for (const auto &symbol : command.symbols) {
                            subscribed.erase(symbol);
                        }
                    } else if (command.type == CommandType::SUBSCRIPTION_CLOSED) {
                        subscribed.clear();
                    }

                    if (traceSubscriptions_) {
                        const auto regional =
                            static_cast<std::size_t>(std::ranges::count_if(subscribed, [](const auto &symbol) {
                                return symbol.find('&') != std::string::npos;
                            }));

                        std::cout << std::format(
                                         "Observed {} subscription: update={} total={} composite={} regional={}\n",
                                         subscriptionKindName(command.subscription), command.symbols.size(),
                                         subscribed.size(), subscribed.size() - regional, regional)
                                  << std::flush;
                    }
                }

                startWhenReady();

                continue;
            }

            lock.unlock();

            if (active && std::chrono::steady_clock::now() >= nextTick) {
                publish(*active);
                nextTick += active->pattern.publishPeriod;
                const auto now = std::chrono::steady_clock::now();

                // Do not emit a burst to catch up after the publisher has fallen behind.
                if (nextTick < now) {
                    const auto overdue = now - nextTick;
                    const auto skipped = overdue / active->pattern.publishPeriod + 1;
                    active->skippedDeadlines += static_cast<std::uint64_t>(skipped);
                    nextTick += active->pattern.publishPeriod * skipped;
                }
            }
        }
    }

    public:
    /** Starts the generator worker for a publisher and optionally logs observed symbol cardinality. */
    explicit Generator(std::shared_ptr<DXPublisher> publisher, bool traceSubscriptions = false)
        : publisher_(std::move(publisher)), traceSubscriptions_(traceSubscriptions), thread_([this] {
              run();
          }) {
    }

    /** Stops and joins the generator worker. */
    ~Generator() {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
        }

        cv_.notify_one();

        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /** Enqueues a task-control command. */
    void enqueueControl(CommandType type, std::string text = {}) {
        {
            std::lock_guard lock{mutex_};

            commands_.push_back(Command{type, std::move(text)});
        }

        cv_.notify_one();
    }

    /** Enqueues symbols added to or removed from an observable subscription. */
    void enqueueSymbols(CommandType type, SubscriptionKind subscription,
                        const std::unordered_set<SymbolWrapper> &symbols) {
        std::vector<std::string> strings;

        strings.reserve(symbols.size());

        for (const auto &symbol : symbols) {
            if (symbol.isStringSymbol()) {
                strings.push_back(symbol.asStringSymbol());
            }
        }

        {
            std::lock_guard lock{mutex_};

            commands_.push_back(Command{type, {}, subscription, std::move(strings)});
        }

        cv_.notify_one();
    }

    /** Enqueues closure of an observable subscription. */
    void enqueueSubscriptionClosed(SubscriptionKind subscription) {
        {
            std::lock_guard lock{mutex_};

            commands_.push_back(Command{CommandType::SUBSCRIPTION_CLOSED, {}, subscription});
        }

        cv_.notify_one();
    }
};

/** Command-line settings for the publisher endpoint, optional automatic task, and QD monitoring. */
struct Config {
    /** Address on which the publisher endpoint listens. */
    std::string address{":7400"};

    /** Task queued without relying on a TextMessage control subscription. */
    std::optional<std::string> task;

    /** QD monitoring output period, or empty when monitoring is disabled. */
    std::optional<std::chrono::milliseconds> monitoringStat{10s};

    /** Whether publisher-observable subscription cardinalities are logged. */
    bool traceSubscriptions{};
};

/** Parses and validates benchmark-server command-line arguments. */
Config parseArgs(int argc, char **argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            std::cout << R"(Usage: latency_server [options]
  --address :7400          default :7400
  --task SUB:T1@100ms      queue a task without the TextMessage control channel
  --monitoring-stat 10s    0 disables QD statistics
  --trace-subscriptions    log observed composite and regional symbol counts
)";
            std::exit(0);
        }

        if (arg == "--trace-subscriptions") {
            config.traceSubscriptions = true;

            continue;
        }

        if (i + 1 >= argc) {
            throw std::invalid_argument(std::format("missing value for {}", arg));
        }

        const std::string value = argv[++i];

        if (arg == "--address") {
            config.address = value;
        } else if (arg == "--task") {
            const auto pattern = latency::parseTask(value);

            if (!pattern) {
                throw std::invalid_argument(
                    std::format("task parse error at {}: {}", pattern.error().position, pattern.error().message));
            }

            config.task = value;
        } else if (arg == "--monitoring-stat") {
            auto period = latency::parseMonitoringPeriod(value);

            if (!period) {
                throw std::invalid_argument(period.error());
            }

            config.monitoringStat = *period;
        } else {
            throw std::invalid_argument(std::format("unknown argument: {}", arg));
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
} // namespace

/** Runs the synthetic benchmark publisher application. */
int main(int argc, char **argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        const auto config = parseArgs(argc, argv);

        const auto endpointBuilder =
            DXEndpoint::newBuilder()->withRole(DXEndpoint::Role::PUBLISHER)->withName("latency-server");

        configureMonitoring(endpointBuilder, config.monitoringStat);

        const auto endpoint = endpointBuilder->build();
        const auto publisher = endpoint->getPublisher();
        Generator generator{publisher, config.traceSubscriptions};

        /** Owns an observable subscription and the identifier of its registered listener. */
        struct ObservedSubscription {
            std::shared_ptr<ObservableSubscription> subscription;
            std::size_t listenerId{};
        };

        std::vector<ObservedSubscription> observedSubscriptions;
        const auto observe = [&generator, &publisher, &observedSubscriptions](SubscriptionKind kind,
                                                                              const EventTypeEnum &eventType) {
            const auto subscription = publisher->getSubscription(eventType);
            const auto listener = ObservableSubscriptionChangeListener::create(
                [&generator, kind](const auto &symbols) {
                    generator.enqueueSymbols(CommandType::SYMBOLS_ADDED, kind, symbols);
                },
                [&generator, kind](const auto &symbols) {
                    generator.enqueueSymbols(CommandType::SYMBOLS_REMOVED, kind, symbols);
                },
                [&generator, kind] {
                    generator.enqueueSubscriptionClosed(kind);
                });

            observedSubscriptions.push_back({subscription, subscription->addChangeListener(listener)});
        };

        observe(SubscriptionKind::QUOTE, Quote::TYPE);
        observe(SubscriptionKind::TRADE, Trade::TYPE);
        observe(SubscriptionKind::TRADE_ETH, TradeETH::TYPE);
        observe(SubscriptionKind::SUMMARY, Summary::TYPE);
        observe(SubscriptionKind::PROFILE, Profile::TYPE);

        const auto controlSubscription = publisher->getSubscription(TextMessage::TYPE);

        // A client starts and stops its task by adding and removing the task string as a TextMessage symbol.
        const auto controlListener = ObservableSubscriptionChangeListener::create(
            [&generator](const auto &symbols) {
                for (const auto &symbol : symbols) {
                    if (symbol.isStringSymbol()) {
                        const auto text = symbol.asStringSymbol();

                        if (!latency::isMarkerSymbol(text)) {
                            generator.enqueueControl(CommandType::START, text);
                        }
                    }
                }
            },
            [&generator](const auto &symbols) {
                for (const auto &symbol : symbols) {
                    if (symbol.isStringSymbol()) {
                        const auto text = symbol.asStringSymbol();

                        if (!latency::isMarkerSymbol(text)) {
                            generator.enqueueControl(CommandType::STOP, text);
                        }
                    }
                }
            },
            [&generator] {
                generator.enqueueControl(CommandType::STOP);
            });
        const auto controlListenerId = controlSubscription->addChangeListener(controlListener);

        endpoint->connect(config.address);
        std::cout << std::format("Latency server listening on {}. Press Ctrl+C to stop.\n", config.address)
                  << std::flush;

        if (config.task) {
            generator.enqueueControl(CommandType::START, *config.task);
            std::cout << std::format("Queued automatic task {}\n", *config.task) << std::flush;
        }

        while (!interrupted.load()) {
            std::this_thread::sleep_for(200ms);
        }

        controlSubscription->removeChangeListener(controlListenerId);

        for (const auto &observed : observedSubscriptions) {
            observed.subscription->removeChangeListener(observed.listenerId);
        }

        endpoint->closeAndAwaitTermination();

        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Server error: " << e.what() << '\n';

        return 1;
    }
}
