#include "syscope.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}
template<typename Function>
void throws(Function function, const std::string& message) {
    bool caught = false;
    try { function(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}

syscope::CpuCounters cpu(const std::string& text) {
    std::istringstream input(text);
    return syscope::parse_cpu(input);
}
syscope::Memory memory(const std::string& text) {
    std::istringstream input(text);
    return syscope::parse_memory(input);
}

// Each test owns its scratch directory; nothing is written into the source tree.
struct Scratch {
    std::filesystem::path path;
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            path = base / ("syscope-test-" + std::to_string(stamp) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(path)) { return; }
        }
        throw std::runtime_error("Cannot create test scratch directory");
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << content;
    output.close();
}

void core_tests() {
    syscope::CpuUsage meter;
    check(!meter.update(cpu("cpu 100 0 100 800 0 0 0 0 25 0\n")), "First CPU sample must be absent");
    const auto measured = meter.update(cpu("cpu 150 0 150 900 0 0 0 0 75 0\n"));
    check(measured && std::abs(*measured - 50.0) < 0.0001, "CPU delta/guest exclusion failed");
    check(!meter.update(cpu("cpu 150 0 150 900")), "Zero CPU delta must be absent");
    check(!meter.update(cpu("cpu 1 0 1 8")), "Counter rollback must rebaseline");
    const auto idle = meter.update(cpu("cpu 1 0 1 18"));
    check(idle && *idle == 0.0, "Idle interval must report zero");
    const auto busy = meter.update(cpu("cpu 11 0 1 18"));
    check(busy && *busy == 100.0, "Busy interval must report 100");
    syscope::CpuUsage wait_meter;
    wait_meter.update(cpu("cpu 10 0 0 10 10"));
    check(!wait_meter.update(cpu("cpu 20 0 0 20 9")), "Decreasing iowait must rebaseline");
    syscope::CpuUsage large;
    large.update(cpu("cpu 9007199254740992 0 0 9007199254740992"));
    const auto precise = large.update(cpu("cpu 9007199254740993 0 0 9007199254740993"));
    check(precise && *precise == 50.0, "Large counters lost integer precision");
    throws([] { cpu("cpu 1 2 3"); }, "Truncated CPU line accepted");
    throws([] { cpu("cpu0 1 2 3 4"); }, "Per-core line accepted as aggregate");
    throws([] { cpu("cpu -1 0 0 0"); }, "Negative counter accepted");
    throws([] { cpu("cpu 18446744073709551616 0 0 0"); }, "Overflow accepted");
    throws([] { cpu("cpu 1x 0 0 0"); }, "Malformed integer accepted");

    const auto mem = memory("MemTotal: 8192 kB\nMemFree: 10 kB\nMemAvailable: 2048 kB\n"
                            "SwapTotal: 1024 kB\nSwapFree: 256 kB\nHugePages_Total: 0\n");
    check(mem.used_kib() == 6144 && mem.utilization() == 75.0, "Memory calculation failed");
    check(mem.swap_used_kib() == 768, "Swap calculation failed");
    check(!memory("MemTotal: 8 kB\nMemAvailable: 2 kB\n").swap_used_kib(), "Absent swap invented");
    check(memory("MemTotal: 8 kB\nMemAvailable: 2 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n")
              .swap_used_kib() == 0, "Zero swap must be available");
    throws([] { memory("MemTotal: 8 kB\n"); }, "Missing MemAvailable accepted");
    throws([] { memory("MemTotal: 0 kB\nMemAvailable: 0 kB\n"); }, "Zero total accepted");
    throws([] { memory("MemTotal: 8 kB\nMemAvailable: 9 kB\n"); }, "Memory underflow accepted");
    throws([] { memory("MemTotal: 8 MB\nMemAvailable: 2 kB\n"); }, "Wrong unit accepted");
    throws([] { memory("MemTotal: 8 kB\nMemAvailable: 2 kB\nSwapTotal: 1 kB\nSwapFree: 2 kB\n"); },
           "Swap underflow accepted");

    std::istringstream process_input("Name:\tworker with spaces\nVmRSS: 64 kB\nVmSize: 4096 kB\n");
    const auto process = syscope::parse_process(process_input, 42);
    check(process.pid == 42 && process.name == "worker with spaces" && process.rss_kib == 64 &&
          process.virtual_kib == 4096, "Process status parsing failed");
    std::istringstream kernel_input("Name:\tkworker\n");
    check(!syscope::parse_process(kernel_input, 1).rss_kib, "Unavailable RSS invented");
    std::istringstream escape_input("Name:\tbad\x1b[31m\n");
    check(syscope::parse_process(escape_input, 1).name == "bad?[31m", "Terminal escape not sanitized");

    Scratch scratch;
    const auto root = scratch.path / "proc";
    std::filesystem::create_directories(root / "42");
    std::filesystem::create_directories(root / "7");
    std::filesystem::create_directories(root / "99"); // Status disappeared after directory enumeration.
    std::filesystem::create_directories(root / "100");
    std::filesystem::create_directories(root / "self");
    write_file(root / "42" / "status", "Name:\tworker\nVmRSS: 64 kB\n");
    write_file(root / "7" / "status", "Name:\tkworker\n");
    write_file(root / "100" / "status", "VmRSS: broken\n");
    write_file(root / "stat", "cpu 1 2 3 4\n");
    write_file(root / "meminfo", "MemTotal: 8 kB\nMemAvailable: 2 kB\n");
    const syscope::ProcReader reader(root);
    const auto processes = reader.processes();
    check(processes.size() == 2 && processes[0].pid == 7 && processes[1].pid == 42,
          "Process sorting/disappearing-file handling failed");
    check(reader.cpu().ticks[3] == 4 && reader.memory().used_kib() == 6, "File collectors failed");
    throws([&] { syscope::ProcReader(scratch.path / "missing").cpu(); }, "Missing stat accepted");
    throws([&] { syscope::ProcReader(scratch.path / "missing").processes(); }, "Missing proc accepted");

    const auto csv_path = scratch.path / "metrics.csv";
    syscope::CsvWriter csv(csv_path);
    csv.write("2026-01-01T00:00:00Z", 50.0, mem);
    csv.write("2026-01-01T00:00:01Z", std::nullopt, memory("MemTotal: 8 kB\nMemAvailable: 2 kB\n"));
    csv.close();
    std::ifstream csv_input(csv_path);
    std::string header, first, second;
    std::getline(csv_input, header);
    std::getline(csv_input, first);
    std::getline(csv_input, second);
    check(header == "timestamp_utc,cpu_percent,mem_total_kib,mem_available_kib,mem_used_kib,"
                    "mem_percent,swap_total_kib,swap_used_kib", "CSV header incorrect");
    check(first == "2026-01-01T00:00:00Z,50.00,8192,2048,6144,75.00,1024,768", "CSV values incorrect");
    check(second == "2026-01-01T00:00:01Z,,8,2,6,75.00,,", "CSV missing values incorrect");
    throws([&] { syscope::CsvWriter bad(scratch.path / "missing" / "metrics.csv"); },
           "CSV open error ignored");
    throws([&] { csv.write("timestamp", 1.0, mem); }, "CSV write error ignored");
    const auto stamp = syscope::utc_timestamp();
    check(stamp.size() == 20 && stamp[10] == 'T' && stamp.back() == 'Z', "UTC timestamp format incorrect");
}

void live_tests() {
#ifdef __linux__
    const syscope::ProcReader reader;
    syscope::CpuUsage meter;
    check(!meter.update(reader.cpu()), "Live first sample present");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto cpu_percent = meter.update(reader.cpu());
    check(cpu_percent && *cpu_percent >= 0.0 && *cpu_percent <= 100.0, "Invalid live CPU measurement");
    const auto mem = reader.memory();
    check(mem.total_kib > 0 && mem.available_kib <= mem.total_kib, "Invalid live memory");
    const auto processes = reader.processes();
    check(!processes.empty(), "No live processes found");
    // /dev/full opens successfully but rejects writes; exercises disk-full handling.
    throws([] { syscope::CsvWriter full("/dev/full"); }, "Disk-full error ignored");
    std::cout << "Live /proc: CPU " << *cpu_percent << "%, memory " << mem.total_kib
              << " KiB, " << processes.size() << " processes\n";
#else
    throw std::runtime_error("Live tests require Linux");
#endif
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--live") { live_tests(); }
        else { core_tests(); }
        std::cout << "Tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
