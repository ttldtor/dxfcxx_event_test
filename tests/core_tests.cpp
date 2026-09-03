#include "latency/core.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    using namespace latency;
    const auto parsed = parseTask("SUB:Q100;S1;T5");
    check(parsed.has_value(), "valid mixed task parses");
    if (parsed) {
        check(parsed->toString() == "SUB:Q100;S1;T5", "task round trip");
        check(parsed->eventCount() == 106, "total quantity");
        const auto q = parsed->symbols(EventKind::Quote);
        check(q.size() == 100 && q.front() == "Q00" && q.back() == "Q99", "Q100 symbols");
    }
    check(!parseTask("SUB:"), "empty task rejected");
    check(!parseTask("SUB:Q0"), "zero rejected");
    check(!parseTask("SUB:Q1;Q2"), "duplicate rejected");
    check(!parseTask("SUB:X1"), "unknown type rejected");
    check(!parseTask("SUB:Q1junk"), "trailing input rejected");
    check(!parseTask("SUB:Q1;"), "trailing separator rejected");
    check(!parseTask("SUB:Q999999999999999999999999999999"), "overflow rejected");
    check(parseDuration("10s") == std::chrono::seconds{10}, "seconds duration");
    check(parseDuration("2m") == std::chrono::minutes{2}, "minutes duration");
    check(!parseDuration("0s"), "zero duration rejected");

    const auto stats = calculateStatistics({1, 1, 2, 2, 100});
    check(stats.count == 5 && stats.p50 == 2 && stats.q1 == 1 && stats.q3 == 2, "percentiles");
    check(stats.outlierThreshold == 3.5 && stats.outlierCount == 1, "IQR outlier");
    const auto flat = calculateStatistics({5, 5, 5, 5, 6});
    check(flat.iqr == 0 && flat.outlierCount == 1, "zero IQR behavior");
    check(calculateStatistics({}).count == 0, "empty statistics");
    check(nanosecondsToMicroseconds(123'456) == 123.456, "nanoseconds to microseconds");
    return failures ? 1 : 0;
}
