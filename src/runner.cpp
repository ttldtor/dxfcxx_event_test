// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include "latency/runner.hpp"
#include "latency/core.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#    define NOMINMAX
#    include <Windows.h>
#else
#    include <fcntl.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace latency {
namespace {

using namespace std::chrono_literals;

std::atomic_bool interrupted{};

/// Records an operating-system termination request for cooperative cleanup.
void handleSignal(int) {
    interrupted = true;
}

/// Removes leading and trailing whitespace from a copied string.
std::string trim(std::string value) {
    const auto isNotSpace = [](unsigned char character) {
        return !std::isspace(character);
    };
    const auto first = std::ranges::find_if(value, isNotSpace);
    const auto last = std::ranges::find_if(value | std::views::reverse, isNotSpace).base();

    return first < last ? std::string{first, last} : std::string{};
}

/// Splits a string while preserving empty fields, including a trailing field.
std::vector<std::string> split(std::string_view value, char separator) {
    std::vector<std::string> result;
    std::size_t begin{};

    do {
        const auto end = value.find(separator, begin);
        result.emplace_back(value.substr(begin, end - begin));
        begin = end == std::string_view::npos ? value.size() + 1 : end + 1;
    } while (begin <= value.size());

    return result;
}

/// Parses a decimal size setting and enforces its allowed range.
std::expected<std::size_t, std::string> parseSize(std::string_view value, std::string_view name,
                                                  bool allowZero = false) {
    std::size_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);

    if (error != std::errc{} || end != value.data() + value.size() || (!allowZero && result == 0)) {
        return std::unexpected{
            std::format("{} must be {}integer", name, allowZero ? "a non-negative " : "a positive ")};
    }

    return result;
}

/// Checks whether a client endpoint role is supported by the benchmark.
bool validRole(std::string_view role) {
    return role == "feed" || role == "stream-feed";
}

/// Checks whether a client implementation is supported by the benchmark.
bool validClientImplementation(std::string_view implementation) {
    return implementation == "graal" || implementation == "legacy";
}

/// Escapes one field for RFC 4180-compatible CSV output.
std::string csv(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');

    for (const auto character : value) {
        escaped.push_back(character);

        if (character == '"') {
            escaped.push_back('"');
        }
    }

    escaped.push_back('"');

    return escaped;
}

/// Formats the current UTC time for a directory name or metadata value.
std::string timestamp(bool compact) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};

#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    std::ostringstream output;
    output << std::put_time(&utc, compact ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%dT%H:%M:%SZ");

    return output.str();
}

/// Returns the platform-specific filename for a benchmark executable.
std::string executableName(std::string_view name) {
#ifdef _WIN32

    return std::format("{}.exe", name);
#else

    return std::string{name};
#endif
}

/// Checks whether a text file currently contains a marker string.
bool containsText(const std::filesystem::path &path, std::string_view needle) {
    std::ifstream input{path};

    if (!input) {
        return false;
    }

    const std::string text{std::istreambuf_iterator<char>{input}, {}};

    return text.contains(needle);
}

/// Reads and trims the first line of a text file.
std::string readFirstLine(const std::filesystem::path &path) {
    std::ifstream input{path};
    std::string line;
    std::getline(input, line);

    return trim(line);
}

/// Resolves the current checkout commit without starting an external Git process.
std::string gitCommit() {
    auto gitDirectory = std::filesystem::current_path() / ".git";

    if (std::filesystem::is_regular_file(gitDirectory)) {
        const auto pointer = readFirstLine(gitDirectory);

        if (!pointer.starts_with("gitdir:")) {
            return {};
        }

        gitDirectory = trim(pointer.substr(std::string_view{"gitdir:"}.size()));

        if (gitDirectory.is_relative()) {
            gitDirectory = std::filesystem::current_path() / gitDirectory;
        }
    }

    const auto head = readFirstLine(gitDirectory / "HEAD");

    if (!head.starts_with("ref:")) {
        return head;
    }

    const auto reference = trim(head.substr(std::string_view{"ref:"}.size()));
    const auto looseReference = readFirstLine(gitDirectory / reference);

    if (!looseReference.empty()) {
        return looseReference;
    }

    std::ifstream packedReferences{gitDirectory / "packed-refs"};
    std::string line;

    while (std::getline(packedReferences, line)) {
        const auto separator = line.find(' ');

        if (separator != std::string::npos && line.substr(separator + 1) == reference) {
            return line.substr(0, separator);
        }
    }

    return {};
}

/// Owns a redirected child process and guarantees termination during cleanup.
class ChildProcess {
#ifdef _WIN32
    HANDLE process_{};
    HANDLE job_{};
#else
    pid_t process_{-1};
#endif
    std::optional<int> exitCode_;

    public:
    /// Creates an empty process owner.
    ChildProcess() = default;

    /// Process ownership cannot be copied.
    ChildProcess(const ChildProcess &) = delete;

    /// Process ownership cannot be copy-assigned.
    ChildProcess &operator=(const ChildProcess &) = delete;

    /// Transfers process ownership from another instance.
    ChildProcess(ChildProcess &&other) noexcept {
        *this = std::move(other);
    }

    /// Terminates the currently owned process, then transfers ownership from another instance.
    ChildProcess &operator=(ChildProcess &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        terminate();

#ifdef _WIN32
        process_ = std::exchange(other.process_, nullptr);
        job_ = std::exchange(other.job_, nullptr);
#else
        process_ = std::exchange(other.process_, -1);
#endif
        exitCode_ = std::exchange(other.exitCode_, std::nullopt);

        return *this;
    }

    /// Terminates the owned process tree when it is still running.
    ~ChildProcess() {
        terminate();
    }

    /// Starts a child process with standard output and error redirected to one log.
    static std::expected<ChildProcess, std::string> start(const std::filesystem::path &executable,
                                                          const std::vector<std::string> &arguments,
                                                          const std::filesystem::path &logPath);

    /// Reports whether the child process is still running.
    bool running();

    /// Waits for process completion and returns its normalized exit code.
    int wait();

    /// Terminates the process tree and waits for cleanup.
    void terminate();
};

#ifdef _WIN32
/// Converts a UTF-8 command-line argument to UTF-16.
std::wstring widen(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);

    return result;
}

/// Quotes one argument according to the Windows C command-line parsing rules.
std::wstring quoteWindows(std::wstring_view value) {
    if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring{value};
    }

    std::wstring result{L'"'};
    std::size_t backslashes{};

    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }

        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }

        backslashes = 0;
    }

    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');

    return result;
}

std::expected<ChildProcess, std::string> ChildProcess::start(const std::filesystem::path &executable,
                                                             const std::vector<std::string> &arguments,
                                                             const std::filesystem::path &logPath) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (log == INVALID_HANDLE_VALUE) {
        return std::unexpected{std::format("Unable to create log {}", logPath.string())};
    }

    std::wstring command = quoteWindows(executable.wstring());

    for (const auto &argument : arguments) {
        command += L' ';
        command += quoteWindows(widen(argument));
    }

    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION information{};
    const auto created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &information);
    CloseHandle(log);

    if (!created) {
        return std::unexpected{
            std::format("Unable to start {} (Windows error {})", executable.string(), GetLastError())};
    }

    CloseHandle(information.hThread);
    ChildProcess result;
    result.process_ = information.hProcess;
    result.job_ = CreateJobObjectW(nullptr, nullptr);

    if (result.job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(result.job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(result.job_, result.process_);
    }

    return result;
}

bool ChildProcess::running() {
    if (!process_ || exitCode_) {
        return false;
    }

    DWORD code{};

    if (!GetExitCodeProcess(process_, &code) || code != STILL_ACTIVE) {
        exitCode_ = static_cast<int>(code);

        return false;
    }

    return true;
}

int ChildProcess::wait() {
    if (!process_) {
        return exitCode_.value_or(-1);
    }

    WaitForSingleObject(process_, INFINITE);
    DWORD code{};
    GetExitCodeProcess(process_, &code);
    exitCode_ = static_cast<int>(code);
    CloseHandle(process_);
    process_ = nullptr;

    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }

    return *exitCode_;
}

void ChildProcess::terminate() {
    if (running()) {
        if (job_) {
            TerminateJobObject(job_, 1);
        } else {
            TerminateProcess(process_, 1);
        }
    }

    if (process_) {
        wait();
    } else if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
}
#else
std::expected<ChildProcess, std::string> ChildProcess::start(const std::filesystem::path &executable,
                                                             const std::vector<std::string> &arguments,
                                                             const std::filesystem::path &logPath) {
    const auto log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (log < 0) {
        return std::unexpected{std::format("Unable to create log {}", logPath.string())};
    }

    const auto process = fork();

    if (process < 0) {
        close(log);

        return std::unexpected{std::format("Unable to start {}", executable.string())};
    }

    if (process == 0) {
        setpgid(0, 0);
        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
        close(log);

        std::vector<std::string> values;
        values.reserve(arguments.size() + 1);
        values.push_back(executable.string());
        values.insert(values.end(), arguments.begin(), arguments.end());
        std::vector<char *> pointers;
        pointers.reserve(values.size() + 1);

        for (auto &value : values) {
            pointers.push_back(value.data());
        }

        pointers.push_back(nullptr);
        execv(executable.c_str(), pointers.data());
        _exit(127);
    }

    close(log);
    setpgid(process, process);
    ChildProcess result;
    result.process_ = process;

    return result;
}

bool ChildProcess::running() {
    if (process_ < 0 || exitCode_) {
        return false;
    }

    int status{};
    const auto result = waitpid(process_, &status, WNOHANG);

    if (result == 0) {
        return true;
    }

    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    process_ = -1;

    return false;
}

int ChildProcess::wait() {
    if (process_ < 0) {
        return exitCode_.value_or(-1);
    }

    int status{};
    waitpid(process_, &status, 0);
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    process_ = -1;

    return *exitCode_;
}

void ChildProcess::terminate() {
    if (running()) {
        kill(-process_, SIGTERM);

        for (auto attempt = 0; attempt < 20 && running(); ++attempt) {
            std::this_thread::sleep_for(50ms);
        }

        if (running()) {
            kill(-process_, SIGKILL);
        }
    }

    if (process_ >= 0) {
        wait();
    }
}
#endif

/// Runs a redirected child process to completion or terminates it on interruption.
std::expected<int, std::string> runAndWait(const std::filesystem::path &executable,
                                           const std::vector<std::string> &arguments,
                                           const std::filesystem::path &logPath) {
    auto process = ChildProcess::start(executable, arguments, logPath);

    if (!process) {
        return std::unexpected{process.error()};
    }

    while (!interrupted && process->running()) {
        std::this_thread::sleep_for(100ms);
    }

    if (interrupted) {
        process->terminate();

        return 130;
    }

    return process->wait();
}

/// Renames incomplete client CSV files so the analyzer ignores them.
void makePartial(const std::filesystem::path &prefix) {
    for (const auto suffix : {"-summary.csv", "-outliers.csv", "-callbacks.csv", "-delivery.csv"}) {
        auto source = prefix;
        source += suffix;

        if (!std::filesystem::exists(source)) {
            continue;
        }

        auto partialSuffix = std::string{suffix};
        partialSuffix.replace(partialSuffix.find(".csv"), 4, ".partial.csv");
        auto destination = prefix;
        destination += partialSuffix;
        std::error_code error;
        std::filesystem::rename(source, destination, error);
    }
}

/// Waits for a log marker while also detecting process exit and interruption.
bool waitForText(ChildProcess &process, const std::filesystem::path &path, std::string_view text,
                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!interrupted && std::chrono::steady_clock::now() < deadline) {
        if (!process.running()) {
            return false;
        }

        if (containsText(path, text)) {
            return true;
        }

        std::this_thread::sleep_for(100ms);
    }

    return false;
}

} // namespace

std::expected<BenchmarkSuite, std::string> parseBenchmarkSuite(std::istream &input) {
    std::map<std::string, std::string, std::less<>> settings;
    BenchmarkSuite suite;
    std::string rawLine;

    while (std::getline(input, rawLine)) {
        const auto line = trim(rawLine);

        if (line.empty() || line.starts_with('#')) {
            continue;
        }

        const auto separator = line.find('=');

        if (separator == std::string::npos) {
            return std::unexpected{std::format("Invalid suite configuration line: {}", rawLine)};
        }

        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);

        if (key == "PROFILE") {
            const auto parts = split(value, '|');

            if (parts.size() < 2 || parts.size() > 6 || parts[0].empty() || parts[1].empty()) {
                return std::unexpected{std::format("Invalid PROFILE line: {}", rawLine)};
            }

            suite.profiles.push_back({parts[0], parts[1],
                                      parts.size() >= 3 && !parts[2].empty() ? std::optional{parts[2]} : std::nullopt,
                                      parts.size() >= 4 && !parts[3].empty() ? std::optional{parts[3]} : std::nullopt,
                                      parts.size() >= 5 && !parts[4].empty() ? std::optional{parts[4]} : std::nullopt,
                                      parts.size() >= 6 && !parts[5].empty() ? std::optional{parts[5]} : std::nullopt});
        } else {
            settings[key] = value;
        }
    }

    const auto require = [&](std::string_view name) -> std::expected<std::string, std::string> {
        const auto found = settings.find(name);

        if (found == settings.end() || found->second.empty()) {
            return std::unexpected{std::format("Missing {} in suite configuration", name)};
        }

        return found->second;
    };

    const auto assign = [&](std::string_view name, std::string &target) -> std::expected<void, std::string> {
        auto value = require(name);

        if (!value) {
            return std::unexpected{value.error()};
        }

        target = std::move(*value);

        return {};
    };

    const auto repetitions = require("REPETITIONS");

    if (!repetitions) {
        return std::unexpected{repetitions.error()};
    }

    const auto repetitionsValue = parseSize(*repetitions, "REPETITIONS");

    if (!repetitionsValue) {
        return std::unexpected{repetitionsValue.error()};
    }

    suite.repetitions = *repetitionsValue;

    for (const auto [name, target] : {std::pair{"WARMUP", &suite.warmup},
                                      {"DURATION", &suite.duration},
                                      {"WINDOW", &suite.window},
                                      {"BATCH_TIMEOUT", &suite.batchTimeout},
                                      {"STARTUP_TIMEOUT", &suite.startupTimeout},
                                      {"MONITORING_PERIOD", &suite.monitoringPeriod},
                                      {"CLIENT_ROLE", &suite.clientRole},
                                      {"ADDRESS", &suite.address},
                                      {"LISTEN_ADDRESS", &suite.listenAddress}}) {
        if (auto assigned = assign(name, *target); !assigned) {
            return std::unexpected{assigned.error()};
        }
    }

    const auto cooldown = require("COOLDOWN_SECONDS");

    if (!cooldown) {
        return std::unexpected{cooldown.error()};
    }

    const auto cooldownValue = parseSize(*cooldown, "COOLDOWN_SECONDS", true);

    if (!cooldownValue) {
        return std::unexpected{cooldownValue.error()};
    }

    suite.cooldownSeconds = *cooldownValue;

    if (const auto found = settings.find("LISTENER_DELAY"); found != settings.end()) {
        suite.listenerDelay = found->second;
    }

    if (const auto found = settings.find("EVENTS_BATCH_LIMIT"); found != settings.end()) {
        suite.eventsBatchLimit = found->second;
    }

    if (const auto found = settings.find("AGGREGATION_PERIOD"); found != settings.end()) {
        suite.aggregationPeriod = found->second;
    }

    if (const auto found = settings.find("TIME_SERIES_PREFILL"); found != settings.end()) {
        suite.timeSeriesPrefill = found->second;
    }

    if (const auto found = settings.find("TIME_SERIES_HISTORY"); found != settings.end()) {
        const auto historyLimit = parseSize(found->second, "TIME_SERIES_HISTORY");

        if (!historyLimit) {
            return std::unexpected{historyLimit.error()};
        }

        suite.timeSeriesHistoryLimit = *historyLimit;
    }

    const std::array experimentSettings{
        std::pair{"EXPERIMENT_TITLE", &suite.experiment.title},
        std::pair{"EXPERIMENT_OBJECTIVE", &suite.experiment.objective},
        std::pair{"EXPERIMENT_VARIABLE", &suite.experiment.variable},
        std::pair{"EXPERIMENT_CONTROLS", &suite.experiment.controls},
        std::pair{"EXPERIMENT_SUCCESS_CRITERIA", &suite.experiment.successCriteria},
        std::pair{"EXPERIMENT_LIMITATIONS", &suite.experiment.limitations},
    };
    std::size_t experimentSettingCount{};

    for (const auto &[name, target] : experimentSettings) {
        if (const auto found = settings.find(name); found != settings.end()) {
            if (found->second.empty()) {
                return std::unexpected{std::format("{} must not be empty", name)};
            }

            *target = found->second;
            ++experimentSettingCount;
        }
    }

    if (experimentSettingCount != 0 && experimentSettingCount != experimentSettings.size()) {
        return std::unexpected{
            "Experiment metadata must define title, objective, variable, controls, success criteria, and limitations"};
    }

    const std::vector<std::string_view> known{"REPETITIONS",
                                              "WARMUP",
                                              "DURATION",
                                              "WINDOW",
                                              "BATCH_TIMEOUT",
                                              "STARTUP_TIMEOUT",
                                              "MONITORING_PERIOD",
                                              "TIME_SERIES_PREFILL",
                                              "TIME_SERIES_HISTORY",
                                              "CLIENT_ROLE",
                                              "LISTENER_DELAY",
                                              "EVENTS_BATCH_LIMIT",
                                              "AGGREGATION_PERIOD",
                                              "COOLDOWN_SECONDS",
                                              "ADDRESS",
                                              "LISTEN_ADDRESS",
                                              "EXPERIMENT_TITLE",
                                              "EXPERIMENT_OBJECTIVE",
                                              "EXPERIMENT_VARIABLE",
                                              "EXPERIMENT_CONTROLS",
                                              "EXPERIMENT_SUCCESS_CRITERIA",
                                              "EXPERIMENT_LIMITATIONS"};

    for (const auto &[key, value] : settings) {
        if (std::ranges::find(known, key) == known.end()) {
            return std::unexpected{std::format("Unknown suite setting: {}", key)};
        }
    }

    if (!validRole(suite.clientRole)) {
        return std::unexpected{"CLIENT_ROLE must be feed or stream-feed"};
    }

    if (suite.profiles.empty()) {
        return std::unexpected{"No PROFILE entries in suite configuration"};
    }

    for (const auto &profile : suite.profiles) {
        if (profile.clientRole && !validRole(*profile.clientRole)) {
            return std::unexpected{
                std::format("Invalid client role for profile {}: {}", profile.name, *profile.clientRole)};
        }

        if (profile.clientImplementation && !validClientImplementation(*profile.clientImplementation)) {
            return std::unexpected{std::format("Invalid client implementation for profile {}: {}", profile.name,
                                               *profile.clientImplementation)};
        }

        const auto task = parseTask(profile.task);

        if (!task) {
            return std::unexpected{std::format("Invalid task for profile {} at {}: {}", profile.name,
                                               task.error().position, task.error().message)};
        }

        if (task->quantity(EventKind::TIME_AND_SALE).value_or(0)) {
            const auto role = profile.clientRole.value_or(suite.clientRole);
            const auto implementation = profile.clientImplementation.value_or("graal");

            if (role != "feed") {
                return std::unexpected{
                    std::format("TimeAndSale profile {} requires the feed client role", profile.name)};
            }

            if (implementation == "legacy") {
                return std::unexpected{
                    std::format("TimeAndSale profile {} is not supported by the legacy client", profile.name)};
            }
        }
    }

    return suite;
}

std::expected<BenchmarkSuite, std::string> readBenchmarkSuite(const std::filesystem::path &path) {
    std::ifstream input{path};

    if (!input) {
        return std::unexpected{std::format("Unable to read suite configuration: {}", path.string())};
    }

    return parseBenchmarkSuite(input);
}

std::vector<BenchmarkRun> buildBenchmarkPlan(const BenchmarkSuite &suite, const BenchmarkOverrides &overrides) {
    std::vector<BenchmarkRun> result;
    result.reserve(suite.repetitions * suite.profiles.size());
    const auto defaultRole = overrides.clientRole.value_or(suite.clientRole);
    const auto listenerDelay = overrides.listenerDelay.value_or(suite.listenerDelay);
    const auto defaultBatchLimit = overrides.eventsBatchLimit.value_or(suite.eventsBatchLimit);
    const auto defaultAggregationPeriod = overrides.aggregationPeriod.value_or(suite.aggregationPeriod);

    for (std::size_t repetition = 1; repetition <= suite.repetitions; ++repetition) {
        for (std::size_t position = 0; position < suite.profiles.size(); ++position) {
            const auto &profile = suite.profiles[(position + repetition - 1) % suite.profiles.size()];
            result.push_back({profile.name, repetition, std::format("{}-r{:02}", profile.name, repetition),
                              profile.task, profile.clientRole.value_or(defaultRole), listenerDelay,
                              profile.eventsBatchLimit.value_or(defaultBatchLimit),
                              profile.aggregationPeriod.value_or(defaultAggregationPeriod),
                              profile.clientImplementation.value_or("graal")});
        }
    }

    return result;
}

int runBenchmarkSuite(const std::filesystem::path &binaryDirectory, const std::filesystem::path &outputRoot,
                      const std::filesystem::path &configPath, const BenchmarkOverrides &overrides, bool dryRun) {
    auto suiteResult = readBenchmarkSuite(configPath);

    if (!suiteResult) {
        std::cerr << suiteResult.error() << '\n';

        return 2;
    }

    const auto &suite = *suiteResult;

    if (overrides.clientRole && !validRole(*overrides.clientRole)) {
        std::cerr << "--client-role must be feed or stream-feed\n";

        return 2;
    }

    const auto plan = buildBenchmarkPlan(suite, overrides);
    const auto server = binaryDirectory / executableName("latency_server");
    const auto client = binaryDirectory / executableName("latency_client");
    const auto legacyClient = binaryDirectory / executableName("latency_legacy_client");
    const auto analyzer = binaryDirectory / executableName("latency_analyzer");

    for (const auto &binary : {server, analyzer}) {
        if (!std::filesystem::exists(binary)) {
            std::cerr << "Missing benchmark binary: " << binary << '\n';

            return 2;
        }
    }

    const auto needsGraalClient = std::ranges::any_of(plan, [](const BenchmarkRun &run) {
        return run.clientImplementation == "graal";
    });
    const auto needsLegacyClient = std::ranges::any_of(plan, [](const BenchmarkRun &run) {
        return run.clientImplementation == "legacy";
    });

    if (needsGraalClient && !std::filesystem::exists(client)) {
        std::cerr << "Missing benchmark binary: " << client << '\n';

        return 2;
    }

    if (needsLegacyClient && !std::filesystem::exists(legacyClient)) {
        std::cerr << "Missing benchmark binary: " << legacyClient
                  << " (configure with LATENCY_BUILD_LEGACY_CLIENT=ON)\n";

        return 2;
    }

    if (dryRun) {
        for (const auto &run : plan) {
            std::cout << std::format("{} : {} ; client={} role={} events-batch-limit={} aggregation-period={} "
                                     "listener-delay={} "
                                     "startup-timeout={} time-series-prefill={} time-series-history={} warmup={} "
                                     "duration={}\n",
                                     run.prefix, run.task, run.clientImplementation, run.clientRole,
                                     run.eventsBatchLimit, run.aggregationPeriod, run.listenerDelay,
                                     suite.startupTimeout, suite.timeSeriesPrefill, suite.timeSeriesHistoryLimit,
                                     suite.warmup, suite.duration);
        }

        std::cout << std::format("Analyzer: {} --monitoring-period {}\n", analyzer.string(), suite.monitoringPeriod);

        return 0;
    }

    interrupted = false;
    const auto previousInterrupt = std::signal(SIGINT, handleSignal);
    const auto previousTerminate = std::signal(SIGTERM, handleSignal);
    const auto runDirectory = std::filesystem::absolute(outputRoot / timestamp(true));
    std::error_code filesystemError;
    std::filesystem::create_directories(runDirectory, filesystemError);

    if (filesystemError) {
        std::cerr << "Unable to create result directory: " << filesystemError.message() << '\n';

        return 1;
    }

    std::filesystem::copy_file(configPath, runDirectory / "suite.conf",
                               std::filesystem::copy_options::overwrite_existing, filesystemError);

    if (filesystemError) {
        std::cerr << "Unable to copy suite configuration: " << filesystemError.message() << '\n';

        return 1;
    }

    std::ofstream manifest{runDirectory / "run-manifest.csv"};
    manifest << "profile,repetition,task,client_implementation,client_role,events_batch_limit,aggregation_period,"
                "status,client_exit_code\n";
    std::ofstream environment{runDirectory / "environment.txt"};
    environment << "started_utc=" << timestamp(false) << '\n';
    environment << "git_commit=" << gitCommit() << '\n';
    environment << "cxx_compiler=" << LATENCY_CXX_COMPILER_ID << ' ' << LATENCY_CXX_COMPILER_VERSION << '\n';
    environment << "dxfeed_graal_cxx_api_version=" << LATENCY_DXFCXX_VERSION << '\n';
    environment << "dxfeed_graal_native_sdk_version=" << LATENCY_DXFG_VERSION << '\n';
    environment << "qd_version=" << LATENCY_QD_VERSION << '\n';
#ifdef _WIN32
    environment << "os=Windows\narchitecture="
#    if defined(_M_ARM64)
                << "arm64\n";
#    else
                << "x86_64\n";
#    endif
#elif defined(__APPLE__)
    environment << "os=macOS\narchitecture=";
#    if defined(__aarch64__) || defined(__arm64__)
    environment << "arm64\n";
#    else
    environment << "x86_64\n";
#    endif
#else
    environment << "os=Linux\narchitecture=";
#    if defined(__aarch64__)
    environment << "arm64\n";
#    else
    environment << "x86_64\n";
#    endif
#endif
    environment << "processor_count=" << std::thread::hardware_concurrency() << '\n'
                << "binary_directory=" << std::filesystem::absolute(binaryDirectory).string() << '\n'
                << "suite_config=" << std::filesystem::absolute(configPath).string() << '\n'
                << "client_role=" << overrides.clientRole.value_or(suite.clientRole) << '\n'
                << "listener_delay=" << overrides.listenerDelay.value_or(suite.listenerDelay) << '\n'
                << "events_batch_limit=" << overrides.eventsBatchLimit.value_or(suite.eventsBatchLimit) << '\n'
                << "aggregation_period=" << overrides.aggregationPeriod.value_or(suite.aggregationPeriod) << '\n';
    environment << "time_series_prefill=" << suite.timeSeriesPrefill << '\n'
                << "time_series_history=" << suite.timeSeriesHistoryLimit << '\n';
    environment.flush();

    bool failed{};

    for (std::size_t index = 0; index < plan.size() && !interrupted; ++index) {
        const auto &run = plan[index];
        const auto prefix = runDirectory / run.prefix;
        auto serverLog = prefix;
        serverLog += "-server.log";
        auto clientLog = prefix;
        clientLog += "-client.log";
        std::cout << std::format("Starting {} with {} client ({})\n", run.prefix, run.clientImplementation, run.task)
                  << std::flush;
        std::vector<std::string> serverArguments{"--address",
                                                 suite.listenAddress,
                                                 "--monitoring-stat",
                                                 suite.monitoringPeriod,
                                                 "--time-series-history",
                                                 std::to_string(suite.timeSeriesHistoryLimit)};

        if (run.clientImplementation == "legacy") {
            serverArguments.insert(serverArguments.end(), {"--task", run.task});
        }

        auto serverProcess = ChildProcess::start(server, serverArguments, serverLog);
        int clientExit{-1};
        std::string status{"failed"};

        if (!serverProcess) {
            std::ofstream{clientLog} << "Runner error: " << serverProcess.error() << '\n';
        } else if (!waitForText(*serverProcess, serverLog, "Latency server listening on", 30s)) {
            std::ofstream{clientLog} << "Runner error: latency server did not become ready\n";
        } else {
            std::expected<int, std::string> clientResult;

            if (run.clientImplementation == "legacy") {
                clientResult = runAndWait(legacyClient,
                                          {"--address", suite.address, "--task", run.task, "--warmup", suite.warmup,
                                           "--duration", suite.duration, "--startup-timeout", suite.startupTimeout,
                                           "--output", prefix.string(), "--contract", "default", "--require-events"},
                                          clientLog);
            } else {
                clientResult = runAndWait(client,
                                          {"--address",
                                           suite.address,
                                           "--task",
                                           run.task,
                                           "--role",
                                           run.clientRole,
                                           "--listener-delay",
                                           run.listenerDelay,
                                           "--events-batch-limit",
                                           run.eventsBatchLimit,
                                           "--aggregation-period",
                                           run.aggregationPeriod,
                                           "--warmup",
                                           suite.warmup,
                                           "--duration",
                                           suite.duration,
                                           "--window",
                                           suite.window,
                                           "--batch-timeout",
                                           suite.batchTimeout,
                                           "--startup-timeout",
                                           suite.startupTimeout,
                                           "--time-series-prefill",
                                           suite.timeSeriesPrefill,
                                           "--monitoring-stat",
                                           suite.monitoringPeriod,
                                           "--output",
                                           prefix.string()},
                                          clientLog);
            }

            clientExit = clientResult.value_or(-1);
            auto resultFile = prefix;
            resultFile += run.clientImplementation == "legacy" ? "-delivery.csv" : "-summary.csv";

            if (clientResult && clientExit == 0 && std::filesystem::exists(resultFile)) {
                if (run.clientImplementation == "graal") {
                    const auto deadline = std::chrono::steady_clock::now() + 5s;

                    while (std::chrono::steady_clock::now() < deadline &&
                           !containsText(serverLog, "Generator summary")) {
                        std::this_thread::sleep_for(100ms);
                    }
                }

                status = "passed";
            } else if (!clientResult) {
                std::ofstream output{clientLog, std::ios::app};
                output << "Runner error: " << clientResult.error() << '\n';
            }
        }

        if (serverProcess) {
            serverProcess->terminate();
        }

        if (status != "passed") {
            failed = true;
            makePartial(prefix);
        }

        manifest << csv(run.profile) << ',' << run.repetition << ',' << csv(run.task) << ',' << run.clientImplementation
                 << ',' << run.clientRole << ',' << run.eventsBatchLimit << ',' << run.aggregationPeriod << ','
                 << status << ',' << clientExit << '\n';
        manifest.flush();

        if (index + 1 < plan.size()) {
            for (std::size_t second = 0; second < suite.cooldownSeconds && !interrupted; ++second) {
                std::this_thread::sleep_for(1s);
            }
        }
    }

    if (interrupted) {
        failed = true;
    }

    const auto hasResult =
        std::ranges::any_of(std::filesystem::directory_iterator{runDirectory}, [](const auto &entry) {
            const auto filename = entry.path().filename().string();

            return filename.ends_with("-summary.csv") || filename.ends_with("-delivery.csv");
        });

    if (interrupted) {
        std::ofstream{runDirectory / "analyzer.log"} << "Benchmark interrupted before analysis\n";
    } else if (hasResult) {
        const auto analyzerResult = runAndWait(
            analyzer, {"--run-directory", runDirectory.string(), "--monitoring-period", suite.monitoringPeriod},
            runDirectory / "analyzer.log");

        if (!analyzerResult || *analyzerResult != 0) {
            failed = true;
        }
    } else {
        std::ofstream{runDirectory / "analyzer.log"} << "No successful benchmark runs to analyze\n";
        failed = true;
    }

    std::signal(SIGINT, previousInterrupt);
    std::signal(SIGTERM, previousTerminate);
    std::cout << "Benchmark results: " << runDirectory << '\n';

    return failed ? 1 : 0;
}

} // namespace latency
