#pragma once

#include <array>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace syscope {

struct CpuCounters {
    // user, nice, system, idle, iowait, irq, softirq, steal; guest is already in user/nice.
    std::array<std::uint64_t, 8> ticks{};
};

class CpuUsage {
public:
    // Missing on first sample, counter rollback, or an interval with no new ticks.
    std::optional<double> update(const CpuCounters& current);
private:
    std::optional<CpuCounters> previous_;
};

struct Memory {
    std::uint64_t total_kib{};
    std::uint64_t available_kib{};
    std::optional<std::uint64_t> swap_total_kib;
    std::optional<std::uint64_t> swap_free_kib;
    std::uint64_t used_kib() const;
    double utilization() const;
    std::optional<std::uint64_t> swap_used_kib() const;
};

struct Process {
    int pid{};
    std::string name;
    std::optional<std::uint64_t> rss_kib;
    std::optional<std::uint64_t> virtual_kib;
};

CpuCounters parse_cpu(std::istream& input);
Memory parse_memory(std::istream& input);
Process parse_process(std::istream& input, int pid);

class ProcReader {
public:
    explicit ProcReader(std::filesystem::path root = "/proc");
    CpuCounters cpu() const;
    Memory memory() const;
    std::vector<Process> processes() const;
private:
    std::filesystem::path root_;
};

std::string utc_timestamp();

struct Snapshot {
    std::string timestamp;
    std::optional<double> cpu_percent;
    Memory memory;
};

// One sampling thread and a bounded, single-slot handoff to the output thread.
class Sampler {
public:
    explicit Sampler(ProcReader reader = ProcReader{},
                     std::chrono::milliseconds interval = std::chrono::seconds(1));
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    // A timeout returns nullopt; a collection failure is rethrown on this thread.
    std::optional<Snapshot> next(std::chrono::milliseconds timeout);
    void request_stop();
private:
    void run() noexcept;
    ProcReader reader_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Snapshot> pending_;
    std::exception_ptr error_;
    bool stopping_ = false;
    std::thread worker_;
};

class CsvWriter {
public:
    explicit CsvWriter(const std::filesystem::path& path);
    void write(const std::string& timestamp, std::optional<double> cpu, const Memory& memory);
    void close();
private:
    std::ofstream output_;
};

} // namespace syscope
