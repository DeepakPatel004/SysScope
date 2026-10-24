#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <string>
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

class CsvWriter {
public:
    explicit CsvWriter(const std::filesystem::path& path);
    void write(const std::string& timestamp, std::optional<double> cpu, const Memory& memory);
    void close();
private:
    std::ofstream output_;
};

} // namespace syscope
