// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace latency {

/// Process resource measurements collected during one benchmark interval.
struct ResourceStatistics {
    /// CPU utilization where 100 percent represents one fully occupied logical core.
    double cpuCorePercent{};

    /// CPU utilization normalized by the number of logical processors in the host.
    double cpuHostPercent{};

    /// Arithmetic mean of sampled resident-set sizes in bytes.
    std::uint64_t rssMeanBytes{};

    /// Largest sampled resident-set size in bytes.
    std::uint64_t rssMaximumBytes{};

    /// Number of resident-set samples.
    std::size_t samples{};
};

/// Samples current-process CPU time and resident-set size through the cross-platform Process library.
class ResourceSampler final {
    std::chrono::milliseconds initialCpu_;
    std::uint64_t rssTotal_{};
    std::uint64_t rssMaximum_{};
    std::size_t samples_{};

    public:
    /// Captures the initial process CPU time and first resident-set sample.
    ResourceSampler();

    /// Records the current resident-set size.
    void sample();

    /// Calculates interval CPU utilization and resident-set aggregates.
    /// @param elapsedSeconds Wall-clock length of the sampled interval.
    /// @return CPU and resident-memory statistics for the interval.
    [[nodiscard]] ResourceStatistics finish(double elapsedSeconds) const;
};

} // namespace latency
