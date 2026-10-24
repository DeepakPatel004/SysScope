# SysScope

A compact, single-threaded Linux system monitor written in C++17. Reads real
kernel counters through `/proc`; no runtime dependencies beyond the C++ standard
library. The three commands cover live CPU/RAM monitoring, a process snapshot,
and timestamped CSV collection.

## Build and run

Requires Linux 3.14+ with `/proc` mounted, CMake 3.16+, and a C++17 compiler with
`std::filesystem` support (GCC 9+ or a recent Clang). On Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential cmake git
cd SysScope
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

./build/syscope monitor
./build/syscope processes
./build/syscope log --duration 30 --output metrics.csv
```

`monitor` refreshes once per second. On a terminal it redraws the display; when
redirected it writes successive snapshots without escape sequences. Ctrl+C stops
monitoring or logging. For a bounded run:

```bash
./build/syscope monitor --duration 5
./build/syscope --help
```

Duration is an integer from 1 to 86400 seconds. The monitor displays an initial
snapshot followed by that many measured intervals. Logging requires both options
and writes one row after each one-second interval: a 30-second run normally has
30 data rows plus a header. Slow I/O can extend the elapsed time; missed deadlines
do not cause bursts of catch-up samples. A sampling interval has no hard real-time
guarantee.

## Measurements

| Metric | Source and calculation |
| --- | --- |
| CPU | Aggregate `cpu` line in `/proc/stat`; `100 * (delta_total - delta_idle) / delta_total`, where idle includes `idle + iowait`. |
| RAM | `/proc/meminfo`; used = `MemTotal - MemAvailable`, percent = `100 * used / MemTotal`. |
| Swap | `SwapTotal - SwapFree`, when both fields exist. Zero total means no configured swap. |
| Processes | Numeric `/proc` entries, reading `Name`, `VmRSS`, and `VmSize` from each `status` file; sorted by PID. |

CPU totals include user, nice, system, idle, iowait, irq, softirq, and steal.
Guest counters are excluded because they are already included in user/nice.
The first CPU sample establishes a baseline and displays `N/A (collecting
interval)`. An interval with no new ticks or a decreasing counter also produces
N/A and establishes a fresh baseline. This handles resets and the kernel's
occasionally decreasing iowait accounting without unsigned underflow.

Linux's memory fields labeled `kB` represent KiB (1024 bytes). The monitor converts
them to MiB by dividing by 1024; process output and CSV retain KiB. RSS is resident
physical memory; virtual size includes address space that may not occupy RAM.
Missing process memory fields display `N/A`. Process names preserve spaces and
have control characters replaced before terminal output.

## CSV format and errors

```text
timestamp_utc,cpu_percent,mem_total_kib,mem_available_kib,mem_used_kib,mem_percent,swap_total_kib,swap_used_kib
```

Timestamps use UTC ISO 8601 (`YYYY-MM-DDTHH:MM:SSZ`), percentages use two decimal
places, and memory columns are integer KiB. Unavailable CPU or swap values are
empty fields. CSV formatting uses a fixed decimal point regardless of locale.
**The output file is overwritten if it exists.** Parent directories must exist.
System metrics are validated before opening the output file.

Unreadable/malformed system metrics and CSV open/write/close failures report an
error to stderr and exit with status 1. Rows are flushed as they are written.
On Ctrl+C or SIGTERM, logging closes the file and preserves completed rows
(exit status 130). An I/O failure can leave a partial final row. Process collection
skips individual unreadable, malformed, or vanished status files; failure to
enumerate `/proc` itself is reported as an error.

## Verification under Linux or WSL2

The CTest suite includes portable parser/calculation/file tests and Linux-only
checks that read live `/proc`, run the monitor/process commands, log two real
samples, and validate CSV row counts, CPU range, and memory arithmetic. It also
checks `/dev/full` write failure handling. Test fixture values are used only in
tests; application commands always read `/proc`.

Verified in the development environment: CMake build with GCC 15.2 on Windows,
with the configured warnings enabled and no warnings emitted; all three native
CTest checks passed. Live commands correctly reported unavailable `/proc`, and
logging did not create an output file. Live Linux tests and real-measurement CSV
collection have **not** been run here: the installed Docker WSL distribution has
no compiler and the Docker engine is stopped.

From an Ubuntu WSL2 shell, for this Windows checkout:

```bash
cd /mnt/c/Users/Deepak/Desktop/SysScope
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
./build-linux/syscope monitor --duration 3
./build-linux/syscope processes
./build-linux/syscope log --duration 3 --output metrics.csv
wc -l metrics.csv       # Expect 4 lines: header + 3 measurements.
cat metrics.csv
```

Use separate build directories for Windows and Linux. Native Windows can build
the code and run portable tests, but cannot collect Linux metrics. If no Ubuntu
WSL2 distribution is installed, run `wsl --install -d Ubuntu` from an elevated
Windows terminal, complete its setup, and install the packages listed above.

## Layout

```text
include/syscope.hpp      Metric types, parser/collector APIs, CPU sampler, CSV writer
src/syscope.cpp          Collection, parsing, calculations, CSV serialization
src/main.cpp             CLI validation, scheduling, terminal/process output
tests/tests.cpp          Portable tests and live Linux checks
tests/log_smoke.cmake    End-to-end CSV verification on Linux
```

## Limitations

- System-wide CPU and memory only; no per-core or per-process CPU accounting.
- `MemAvailable` is required; no approximate fallback for older kernels.
- `/proc` is a changing view, not an atomic snapshot. Processes can exit or reuse
  PIDs during collection; permission restrictions can hide entries. Names may be
  truncated by the kernel. RSS is an approximate kernel counter and shared pages
  can appear in multiple processes.
- Inside containers, `/proc` memory/CPU counters may describe the host or VM
  rather than cgroup limits. WSL2 reports its Linux VM, not Windows processes.
- Standard ANSI terminal support is assumed for interactive screen redraws.
- No GUI, worker threads, external libraries, or background daemon.

## References

The [reference project](https://github.com/nazarja/cpp-linux-system-monitor) was
reviewed for its `/proc` parsing and successive CPU sample approach. SysScope
uses its own implementation and compact organization, plain terminal output,
`MemAvailable` accounting, and a separate CSV command.
Field semantics follow the [Linux kernel /proc documentation](https://www.kernel.org/doc/html/latest/filesystems/proc.html).
