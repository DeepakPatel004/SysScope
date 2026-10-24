#include "syscope.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#ifdef __linux__
#include <unistd.h>
#endif

namespace {
static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
std::atomic<bool> stopped{false};
void stop(int) { stopped.store(true, std::memory_order_relaxed); }

void usage() {
    std::cout << "SysScope - Linux CPU, memory, and process monitor\n\n"
                 "Usage:\n"
                 "  syscope monitor [--duration SECONDS]\n"
                 "  syscope processes\n"
                 "  syscope log --duration SECONDS --output FILE\n"
                 "  syscope --help\n\n"
                 "Refresh: 1 second. Duration: integer 1..86400. Ctrl+C stops collection.\n"
                 "Logging overwrites FILE; timestamps are UTC and memory units are KiB.\n";
}

struct Options {
    std::string command;
    std::optional<int> duration;
    std::string output;
};

Options options(int argc, char** argv) {
    Options result;
    result.command = argv[1];
    if (result.command != "monitor" && result.command != "processes" && result.command != "log") {
        throw std::runtime_error("Unknown command: " + result.command);
    }
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (result.command == "processes" || (flag != "--duration" && flag != "--output")) {
            throw std::runtime_error("Unexpected option: " + flag);
        }
        if (++i >= argc) { throw std::runtime_error("Missing value for " + flag); }
        const std::string value = argv[i];
        if (flag == "--duration") {
            int seconds = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (result.duration || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() || seconds < 1 || seconds > 86400) {
                throw std::runtime_error("Duration must be specified once, as an integer 1..86400");
            }
            result.duration = seconds;
        } else {
            if (result.command != "log" || !result.output.empty() || value.empty()) {
                throw std::runtime_error("--output requires one nonempty path and the log command");
            }
            result.output = value;
        }
    }
    if (result.command == "log" && (!result.duration || result.output.empty())) {
        throw std::runtime_error("log requires --duration SECONDS and --output FILE");
    }
    return result;
}

bool terminal_output() {
#ifdef __linux__
    return ::isatty(STDOUT_FILENO) != 0;
#else
    return false;
#endif
}

void display(const syscope::Snapshot& sample, bool terminal) {
    const auto& memory = sample.memory;
    const auto cpu = sample.cpu_percent;
    if (terminal) { std::cout << "\033[2J\033[H"; }
    std::cout << "SysScope | " << sample.timestamp << " | Ctrl+C to stop\n"
              << std::fixed << std::setprecision(1) << "CPU: ";
    if (cpu) { std::cout << *cpu << "%\n"; }
    else { std::cout << "N/A (collecting interval)\n"; }
    const auto mib = [](std::uint64_t kib) { return static_cast<double>(kib) / 1024.0; };
    std::cout << "RAM (MiB): total " << mib(memory.total_kib)
              << " | available " << mib(memory.available_kib)
              << " | used " << mib(memory.used_kib())
              << " | " << memory.utilization() << "%\n";
    if (const auto used = memory.swap_used_kib()) {
        std::cout << "Swap (MiB): " << mib(*used) << " / " << mib(*memory.swap_total_kib) << '\n';
    } else { std::cout << "Swap: N/A\n"; }
    std::cout << '\n' << std::flush;
}

void processes(const syscope::ProcReader& reader) {
    std::cout << std::left << std::setw(10) << "PID" << std::setw(16) << "RSS (KiB)"
              << std::setw(18) << "Virtual (KiB)" << "Name\n";
    for (const auto& process : reader.processes()) {
        const auto value = [](std::optional<std::uint64_t> kib) {
            return kib ? std::to_string(*kib) : std::string("N/A");
        };
        std::cout << std::setw(10) << process.pid << std::setw(16) << value(process.rss_kib)
                  << std::setw(18) << value(process.virtual_kib) << process.name << '\n';
    }
}

void collect(const Options& config, const syscope::ProcReader& reader) {
    syscope::Sampler sampler(reader);
    std::optional<syscope::CsvWriter> csv;
    const bool terminal = terminal_output();
    bool initial = true;
    int collected = 0;
    while (!stopped && (!config.duration || collected < *config.duration)) {
        // Polling keeps signals responsive; worker cancellation uses the mutex.
        const auto sample = sampler.next(std::chrono::milliseconds(100));
        if (!sample) { continue; }
        if (initial) {
            // The worker validates /proc before the output file can be opened.
            if (config.command == "log") { csv.emplace(config.output); }
            else { display(*sample, terminal); }
            initial = false;
            continue;
        }
        if (csv) { csv->write(sample->timestamp, sample->cpu_percent, sample->memory); }
        else { display(*sample, terminal); }
        ++collected;
    }
    sampler.request_stop();
    if (csv) {
        csv->close();
        std::cout << (stopped ? "Stopped; measurements saved to " : "Measurements saved to ")
                  << config.output << '\n';
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
            usage();
            return 0;
        }
        const auto config = options(argc, argv);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        const syscope::ProcReader reader;
        if (config.command == "processes") { processes(reader); }
        else { collect(config, reader); }
        return stopped ? 130 : 0;
    } catch (const std::exception& error) {
        std::cerr << "syscope: " << error.what() << '\n';
        return 1;
    }
}
