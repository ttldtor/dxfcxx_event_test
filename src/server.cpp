#include "latency/core.hpp"

#include <dxfeed_graal_cpp_api/api.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
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

        for (const auto &symbol : pattern.symbols(latency::EventKind::SUMMARY)) {
            auto event = std::make_shared<Summary>(symbol);
            event->setDayId(1);
            events.push_back(std::move(event));
        }

        events.push_back(std::make_shared<TextMessage>(command, "LATENCY_BATCH"));

        return events;
    }

    void publish(ActiveTask &active) {
        const auto nowNs = latency::unixNanosNow();
        const auto nowMs = nowNs / 1'000'000;
        const auto nanoPart = static_cast<std::int32_t>(nowNs % 1'000'000);

        ++active.tick;
        const auto sequence = static_cast<std::int32_t>(active.tick % TextMessage::MAX_SEQUENCE);

        // All event-specific correlation fields and the authoritative marker timestamp describe this same batch.
        for (const auto &event : active.events) {
            if (auto quote = event->sharedAs<Quote>()) {
                quote->setBidTime(nowMs);
                quote->setAskTime(nowMs);
                quote->setTimeNanoPart(nanoPart);
                quote->setBidSize(1);
                quote->setAskSize(1);
                quote->setBidPrice(100.0 + active.tick % 100);
                quote->setAskPrice(100.01 + active.tick % 100);
                quote->setSequence(sequence);
            } else if (auto trade = event->sharedAs<Trade>()) {
                trade->setTimeNanos(nowNs);
                trade->setPrice(100.0 + active.tick % 100);
                trade->setSize(1);
                trade->setSequence(sequence);
            } else if (auto summary = event->sharedAs<Summary>()) {
                summary->setDayId(sequence);
                summary->setDayOpenPrice(99);
                summary->setDayHighPrice(101 + active.tick % 100);
                summary->setDayLowPrice(98);
                summary->setDayClosePrice(100 + active.tick % 100);
            } else if (auto marker = event->sharedAs<TextMessage>()) {
                marker->setTime(nowMs);
                marker->setSequence(sequence);
                marker->setText("LATENCY_BATCH:" + std::to_string(nowNs));
            }
        }

        publisher_->publishEvents(active.events);
    }

    void run() {
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
                cv_.wait_until(lock, nextTick, [&] {
                    return stopping_ || !commands_.empty();
                });
            }

            if (stopping_) {
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
                            active.emplace(ActiveTask{command.text, *parsed, makeEvents(*parsed, command.text)});
                            nextTick = std::chrono::steady_clock::now();
                            std::cout << "Started " << command.text << " (" << active->pattern.eventCount()
                                      << " events/batch)\n";
                        } catch (const std::exception &e) {
                            std::cerr << "Cannot start task: " << e.what() << '\n';
                        }
                    } else if (active->command != command.text) {
                        // The protocol intentionally supports one active load profile at a time.
                        std::cerr << "Rejected concurrent task " << command.text << "; active task is "
                                  << active->command << '\n';
                    }
                } else if (active && (command.text.empty() || active->command == command.text)) {
                    std::cout << "Stopped " << active->command << '\n';
                    active.reset();
                }

                continue;
            }

            lock.unlock();

            if (active && std::chrono::steady_clock::now() >= nextTick) {
                publish(*active);
                nextTick += 1s;
                const auto now = std::chrono::steady_clock::now();

                // Do not emit a burst to catch up after the publisher has fallen behind.
                if (nextTick < now) {
                    nextTick = now + 1s;
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
            std::cout << "Usage: latency_server [options]\n"
                         "  --address :7400          default :7400\n"
                         "  --monitoring-stat 10s    0 disables QD statistics\n";
            std::exit(0);
        }

        if (i + 1 >= argc) {
            throw std::invalid_argument("missing value for " + arg);
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
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    return config;
}

std::string configureMonitoring(const std::optional<std::chrono::milliseconds> &period) {
    const auto value = latency::monitoringPeriodPropertyValue(period);

    if (!System::setProperty(MONITORING_STAT_PROPERTY, value)) {
        throw std::runtime_error("cannot set " + std::string{MONITORING_STAT_PROPERTY} + "=" + value);
    }

    return value;
}
} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        const auto config = parseArgs(argc, argv);

        System::setProperty("dxscheme.nanoTime", "true");
        const auto monitoringStat = configureMonitoring(config.monitoringStat);
        const auto endpoint = DXEndpoint::newBuilder()
                                  ->withRole(DXEndpoint::Role::PUBLISHER)
                                  ->withName("latency-server")
                                  ->withProperty(MONITORING_STAT_PROPERTY, monitoringStat)
                                  ->build();
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
        std::cout << "Latency server listening on " << config.address << ". Press Ctrl+C to stop.\n";

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
