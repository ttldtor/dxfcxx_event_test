#include "latency/core.hpp"

#include <dxfeed_graal_cpp_api/api.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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

void onSignal(int) {
    interrupted.store(true);
}

struct Command {
    bool start{};
    std::string text;
};

// Owns the worker thread that turns subscription changes into a single active publishing task.
class Generator {
    std::shared_ptr<DXPublisher> publisher_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    bool stopping_{};
    std::thread thread_;

    struct ActiveTask {
        std::string command;
        latency::TaskPattern pattern;
        std::vector<std::shared_ptr<EventType>> events;
        std::uint32_t tick{};
        std::uint64_t publications{}, skippedDeadlines{};
        std::chrono::nanoseconds preparationTotal{}, preparationMaximum{};
        std::chrono::nanoseconds publishTotal{}, publishMaximum{};
        std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    };

    // Allocate and initialize the stable event objects that are updated and republished on every tick.
    static std::vector<std::shared_ptr<EventType>> makeEvents(const latency::TaskPattern &pattern,
                                                              const std::string &command) {
        std::vector<std::shared_ptr<EventType>> events;

        events.reserve(pattern.eventCount() + 1);

        for (const auto &symbol : pattern.symbols(latency::EventKind::QUOTE)) {
            auto event = std::make_shared<Quote>(symbol);
            event->setBidExchangeCode('B');
            event->setAskExchangeCode('A');
            event->setBidSize(1);
            event->setAskSize(1);
            events.push_back(std::move(event));
        }

        for (const auto &symbol : pattern.symbols(latency::EventKind::TRADE)) {
            auto event = std::make_shared<Trade>(symbol);
            event->setSize(1);
            events.push_back(std::move(event));
        }

        for (const auto &symbol : pattern.symbols(latency::EventKind::TRADE_ETH)) {
            auto event = std::make_shared<TradeETH>(symbol);
            event->setSize(1);
            events.push_back(std::move(event));
        }

        for (const auto &symbol : pattern.symbols(latency::EventKind::SUMMARY)) {
            auto event = std::make_shared<Summary>(symbol);
            event->setDayId(1);
            events.push_back(std::move(event));
        }

        events.push_back(std::make_shared<TextMessage>(command, "LATENCY_BATCH"));

        return events;
    }

    static std::vector<std::shared_ptr<EventType>> makeInitialEvents(const latency::TaskPattern &pattern) {
        std::vector<std::shared_ptr<EventType>> events;

        events.reserve(pattern.symbolCount());

        for (const auto &symbol : pattern.symbols()) {
            events.push_back(std::make_shared<Profile>(symbol));
        }

        return events;
    }

    void publish(ActiveTask &active) {
        const auto preparationStart = std::chrono::steady_clock::now();
        ++active.tick;
        const auto sequence = static_cast<std::int32_t>(active.tick % TextMessage::MAX_SEQUENCE);

        // Event fields identify the batch. The marker timestamp is captured after preparation, at the publish boundary.
        for (const auto &event : active.events) {
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
        }

        const auto nowNs = latency::unixNanosNow();
        const auto marker = active.events.back()->sharedAs<TextMessage>();
        marker->setTime(nowNs / 1'000'000);
        marker->setSequence(sequence);
        marker->setText(std::format("LATENCY_BATCH:{}", nowNs));
        const auto publishStart = std::chrono::steady_clock::now();
        const auto preparation = std::chrono::duration_cast<std::chrono::nanoseconds>(publishStart - preparationStart);
        publisher_->publishEvents(active.events);
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
                                 "preparation-ms(avg/max)={:.3f}/{:.3f} publish-ms(avg/max)={:.3f}/{:.3f}\n",
                                 active.command, active.publications, active.skippedDeadlines, actualBatchesPerSecond,
                                 actualEventsPerSecond, milliseconds(active.preparationTotal) / publications,
                                 milliseconds(active.preparationMaximum),
                                 milliseconds(active.publishTotal) / publications, milliseconds(active.publishMaximum))
                  << std::flush;
    }

    void run() {
        constexpr auto SPIN_WINDOW = 20ms;
        std::optional<ActiveTask> active;
        auto nextTick = std::chrono::steady_clock::now();

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

                if (command.start) {
                    const auto parsed = latency::parseTask(command.text);

                    if (!parsed) {
                        std::cerr << "Rejected task at " << parsed.error().position << ": " << parsed.error().message
                                  << " [" << command.text << "]\n";
                    } else if (!active) {
                        try {
                            const auto initialEvents = makeInitialEvents(*parsed);

                            if (!initialEvents.empty()) {
                                publisher_->publishEvents(initialEvents);
                                std::cout << std::format("Published {} initial Profile events for {}\n",
                                                         initialEvents.size(), command.text);
                            }

                            active.emplace(ActiveTask{command.text, *parsed, makeEvents(*parsed, command.text)});
                            nextTick = std::chrono::steady_clock::now();
                            std::cout << "Started " << command.text << " (" << active->pattern.eventCount()
                                      << " events/batch, period=" << active->pattern.publishPeriod.count()
                                      << " ms, nominal=" << active->pattern.nominalEventsPerSecond() << " events/s)\n"
                                      << std::flush;
                        } catch (const std::exception &e) {
                            std::cerr << "Cannot start task: " << e.what() << '\n';
                        }
                    } else if (active->command != command.text) {
                        // The protocol intentionally supports one active load profile at a time.
                        std::cerr << "Rejected concurrent task " << command.text << "; active task is "
                                  << active->command << '\n';
                    }
                } else if (active && (command.text.empty() || active->command == command.text)) {
                    logSummary(*active);
                    std::cout << "Stopped " << active->command << '\n';
                    active.reset();
                }

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
    explicit Generator(std::shared_ptr<DXPublisher> publisher)
        : publisher_(std::move(publisher)), thread_([this] {
              run();
          }) {
    }

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

    void enqueue(bool start, std::string text = {}) {
        {
            std::lock_guard lock{mutex_};

            commands_.push_back(Command{start, std::move(text)});
        }

        cv_.notify_one();
    }
};

struct Config {
    std::string address{":7400"};
    std::optional<std::chrono::milliseconds> monitoringStat{10s};
};

Config parseArgs(int argc, char **argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            std::cout << R"(Usage: latency_server [options]
  --address :7400          default :7400
  --monitoring-stat 10s    0 disables QD statistics
)";
            std::exit(0);
        }

        if (i + 1 >= argc) {
            throw std::invalid_argument(std::format("missing value for {}", arg));
        }

        const std::string value = argv[++i];

        if (arg == "--address") {
            config.address = value;
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

void configureMonitoring(const std::optional<std::chrono::milliseconds> &period) {
    const auto value = latency::monitoringPeriodPropertyValue(period);

    if (!System::setProperty(MONITORING_STAT_PROPERTY, value)) {
        throw std::runtime_error(std::format("cannot set {}={}", MONITORING_STAT_PROPERTY, value));
    }
}
} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        const auto config = parseArgs(argc, argv);

        configureMonitoring(config.monitoringStat);
        const auto endpoint =
            DXEndpoint::newBuilder()->withRole(DXEndpoint::Role::PUBLISHER)->withName("latency-server")->build();
        const auto publisher = endpoint->getPublisher();
        Generator generator{publisher};
        const auto observable = publisher->getSubscription(TextMessage::TYPE);

        // A client starts and stops its task by adding and removing the task string as a TextMessage symbol.
        const auto listener = ObservableSubscriptionChangeListener::create(
            [&generator](const auto &symbols) {
                for (const auto &symbol : symbols) {
                    if (symbol.isStringSymbol()) {
                        generator.enqueue(true, symbol.asStringSymbol());
                    }
                }
            },
            [&generator](const auto &symbols) {
                for (const auto &symbol : symbols) {
                    if (symbol.isStringSymbol()) {
                        generator.enqueue(false, symbol.asStringSymbol());
                    }
                }
            },
            [&generator] {
                generator.enqueue(false);
            });
        const auto listenerId = observable->addChangeListener(listener);

        endpoint->connect(config.address);
        std::cout << std::format("Latency server listening on {}. Press Ctrl+C to stop.\n", config.address)
                  << std::flush;

        while (!interrupted.load()) {
            std::this_thread::sleep_for(200ms);
        }

        observable->removeChangeListener(listenerId);
        endpoint->closeAndAwaitTermination();

        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Server error: " << e.what() << '\n';

        return 1;
    }
}
