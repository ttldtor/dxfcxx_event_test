// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/resources.hpp"

#include <process/process.hpp>

#include <algorithm>
#include <thread>

namespace latency {
namespace {
using Process = org::ttldtor::process::Process;
}

ResourceSampler::ResourceSampler() : initialCpu_(Process::getTotalProcessorTime()) {
    sample();
}

void ResourceSampler::sample() {
    const auto rss = Process::getPhysicalMemorySize();

    rssTotal_ += rss;
    rssMaximum_ = std::max(rssMaximum_, rss);
    ++samples_;
}

ResourceStatistics ResourceSampler::finish(double elapsedSeconds) const {
    const auto cpu = Process::getTotalProcessorTime() - initialCpu_;
    const auto cpuCorePercent = elapsedSeconds > 0 ? cpu.count() / 10.0 / elapsedSeconds : 0.0;
    const auto processors = std::max(1U, std::thread::hardware_concurrency());

    return {cpuCorePercent, cpuCorePercent / processors, samples_ ? rssTotal_ / samples_ : 0, rssMaximum_, samples_};
}

} // namespace latency
