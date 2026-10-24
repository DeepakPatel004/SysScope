#include "syscope.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace syscope {
namespace {

std::uint64_t unsigned_value(const std::string& text) {
    std::uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("Invalid unsigned integer: " + text);
    }
    return value;
}

std::uint64_t kib_value(std::istringstream& line) {
    std::string number, unit, extra;
    if (!(line >> number >> unit) || unit != "kB" || (line >> extra)) {
        throw std::runtime_error("Expected a memory value in kB (KiB)");
    }
    return unsigned_value(number);
}

std::ifstream open_input(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    return input;
}

} // namespace

CpuCounters parse_cpu(std::istream& input) {
    std::string text, label, token;
    if (!std::getline(input, text)) {
        throw std::runtime_error("Cannot read aggregate CPU counters");
    }
    std::istringstream line(text);
    line >> label;
    if (label != "cpu") {
        throw std::runtime_error("Missing aggregate cpu line in /proc/stat");
    }
    CpuCounters result;
    std::size_t count = 0;
    while (line >> token) {
        const auto value = unsigned_value(token);
        if (count < result.ticks.size()) {
            result.ticks[count] = value;
        }
        ++count;
    }
    if (count < 4) {
        throw std::runtime_error("Incomplete aggregate CPU counters");
    }
    return result;
}

std::optional<double> CpuUsage::update(const CpuCounters& current) {
    const auto previous = previous_;
    previous_ = current; // Rebaseline even after a reset or iowait counter decrease.
    if (!previous) {
        return std::nullopt;
    }
    long double total = 0;
    long double idle = 0;
    for (std::size_t i = 0; i < current.ticks.size(); ++i) {
        if (current.ticks[i] < previous->ticks[i]) {
            return std::nullopt;
        }
        const auto delta = current.ticks[i] - previous->ticks[i];
        total += static_cast<long double>(delta);
        if (i == 3 || i == 4) {
            idle += static_cast<long double>(delta);
        }
    }
    if (total == 0) {
        return std::nullopt;
    }
    return static_cast<double>(100.0L * (total - idle) / total);
}

std::uint64_t Memory::used_kib() const { return total_kib - available_kib; }
double Memory::utilization() const {
    return 100.0 * static_cast<double>(used_kib()) / static_cast<double>(total_kib);
}
std::optional<std::uint64_t> Memory::swap_used_kib() const {
    if (!swap_total_kib || !swap_free_kib) {
        return std::nullopt;
    }
    return *swap_total_kib - *swap_free_kib;
}

Memory parse_memory(std::istream& input) {
    std::map<std::string, std::uint64_t> values;
    std::string text;
    while (std::getline(input, text)) {
        std::istringstream line(text);
        std::string key;
        line >> key;
        if (key == "MemTotal:" || key == "MemAvailable:" ||
            key == "SwapTotal:" || key == "SwapFree:") {
            const auto value = kib_value(line);
            if (!values.emplace(key, value).second) {
                throw std::runtime_error("Duplicate memory field: " + key);
            }
        }
    }
    if (input.bad()) {
        throw std::runtime_error("Error reading /proc/meminfo");
    }
    if (values.count("MemTotal:") == 0 || values.count("MemAvailable:") == 0) {
        throw std::runtime_error("MemTotal or MemAvailable missing (Linux 3.14+ required)");
    }
    Memory result;
    result.total_kib = values.at("MemTotal:");
    result.available_kib = values.at("MemAvailable:");
    if (result.total_kib == 0 || result.available_kib > result.total_kib) {
        throw std::runtime_error("Inconsistent memory counters");
    }
    if (values.count("SwapTotal:") != 0 && values.count("SwapFree:") != 0) {
        result.swap_total_kib = values.at("SwapTotal:");
        result.swap_free_kib = values.at("SwapFree:");
        if (*result.swap_free_kib > *result.swap_total_kib) {
            throw std::runtime_error("Inconsistent swap counters");
        }
    }
    return result;
}

Process parse_process(std::istream& input, int pid) {
    Process result;
    result.pid = pid;
    std::string text;
    while (std::getline(input, text)) {
        std::istringstream line(text);
        std::string key;
        line >> key;
        if (key == "Name:") {
            std::getline(line >> std::ws, result.name);
            // A process controls its name. Prevent terminal escape/control injection.
            for (char& ch : result.name) {
                const auto byte = static_cast<unsigned char>(ch);
                if (byte < 32 || byte == 127) {
                    ch = '?';
                }
            }
        } else if (key == "VmRSS:") {
            result.rss_kib = kib_value(line);
        } else if (key == "VmSize:") {
            result.virtual_kib = kib_value(line);
        }
    }
    if (input.bad() || result.name.empty()) {
        throw std::runtime_error("Incomplete process status for PID " + std::to_string(pid));
    }
    return result;
}

ProcReader::ProcReader(std::filesystem::path root) : root_(std::move(root)) {}
CpuCounters ProcReader::cpu() const {
    auto input = open_input(root_ / "stat");
    return parse_cpu(input);
}
Memory ProcReader::memory() const {
    auto input = open_input(root_ / "meminfo");
    return parse_memory(input);
}
std::vector<Process> ProcReader::processes() const {
    std::vector<Process> result;
    std::error_code error;
    std::filesystem::directory_iterator it(root_, error), end;
    if (error) {
        throw std::runtime_error("Cannot enumerate " + root_.string() + ": " + error.message());
    }
    while (it != end) {
        const auto name = it->path().filename().string();
        int pid = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && pid > 0) {
            try {
                auto input = open_input(it->path() / "status");
                result.push_back(parse_process(input, pid));
            } catch (const std::runtime_error&) {
                // Exit, permissions, or a partial status: skip this process only.
            }
        }
        it.increment(error);
        if (error) {
            throw std::runtime_error("Error enumerating " + root_.string() + ": " + error.message());
        }
    }
    std::sort(result.begin(), result.end(), [](const Process& a, const Process& b) {
        return a.pid < b.pid;
    });
    return result;
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    static std::mutex time_mutex;
    std::tm utc{};
    {
        std::lock_guard<std::mutex> lock(time_mutex);
        const auto* converted = std::gmtime(&time);
        if (!converted) {
            throw std::runtime_error("Cannot convert timestamp to UTC");
        }
        utc = *converted;
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

CsvWriter::CsvWriter(const std::filesystem::path& path) {
    output_.exceptions(std::ios::failbit | std::ios::badbit);
    output_.imbue(std::locale::classic());
    output_.open(path, std::ios::out | std::ios::trunc);
    output_ << "timestamp_utc,cpu_percent,mem_total_kib,mem_available_kib,mem_used_kib,"
               "mem_percent,swap_total_kib,swap_used_kib\n";
    output_.flush();
}
void CsvWriter::write(const std::string& timestamp, std::optional<double> cpu, const Memory& memory) {
    output_ << timestamp << ',' << std::fixed << std::setprecision(2);
    if (cpu) { output_ << *cpu; }
    output_ << ',' << memory.total_kib << ',' << memory.available_kib << ','
            << memory.used_kib() << ',' << memory.utilization() << ',';
    if (memory.swap_total_kib) { output_ << *memory.swap_total_kib; }
    output_ << ',';
    if (const auto used = memory.swap_used_kib()) { output_ << *used; }
    output_ << '\n';
    output_.flush(); // Detect disk-full/write failures on every measurement.
}
void CsvWriter::close() { output_.close(); }

} // namespace syscope
