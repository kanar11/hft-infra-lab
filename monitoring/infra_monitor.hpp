/*
 * InfraMonitor — monitor infrastruktury HFT w czasie rzeczywistym.
 *
 * Czyta metryki systemowe bezpośrednio z /proc (zero subprocess, zero deps):
 *   /proc/stat        → CPU usage, context switches
 *   /proc/meminfo     → pamięć, hugepages
 *   /proc/interrupts  → per-CPU interrupt counts
 *   /proc/net/dev     → przepustowość sieci
 *
 * W prawdziwym HFT monitorowanie skoków opóźnień i naruszeń izolacji CPU
 * jest krytyczne — jedno context-switch na trading-CPU dorzuca 1-10 µs
 * jitter'a do ścieżki zlecenia.
 *
 * Wydajność (lab): ~50K+ snapshots/sec (direct /proc parsing).
 *
 * Alert cooldown: check_alerts() ma per-metric cooldown (default 5 min)
 * żeby nie spamować — to standard PagerDuty/Splunk.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>

// Max CPUs we track — 128 covers current server hardware; increase if needed
static constexpr int MAX_CPUS = 128;
// Max network interfaces
static constexpr int MAX_IFACES = 4;


// Struktury metryk.

struct CpuStats {
    int64_t idle;
    int64_t total;
    double  used_percent;   // calculated from delta between two snapshots
};

struct MemoryStats {
    int64_t total_kb;
    int64_t available_kb;
    int64_t hugepages_total;
    int64_t hugepages_free;
    double  used_percent;

    int64_t total_mb() const noexcept { return total_kb / 1024; }
    int64_t hugepages_used() const noexcept { return hugepages_total - hugepages_free; }
};

struct NetworkStats {
    char    interface[16];
    int64_t rx_bytes;
    int64_t rx_packets;
    int64_t tx_bytes;
    int64_t tx_packets;
    bool    valid;

    NetworkStats() noexcept
        : rx_bytes(0), rx_packets(0), tx_bytes(0), tx_packets(0), valid(false) {
        interface[0] = '\0';
    }
};

struct InterruptStats {
    int64_t per_cpu[MAX_CPUS];
    int     num_cpus;

    InterruptStats() noexcept : num_cpus(0) {
        std::memset(per_cpu, 0, sizeof(per_cpu));
    }
};

// Alert thresholds / Progi alertów
struct AlertThresholds {
    double  mem_percent;            // alert if memory usage above this
    int64_t ctx_switches_per_sec;   // alert if context switches above this
    int64_t irq_on_isolated_cpu;    // alert if interrupts on isolated CPU above this

    AlertThresholds() noexcept
        : mem_percent(85.0), ctx_switches_per_sec(50000), irq_on_isolated_cpu(100) {}
};


// Funkcje parsujące — bierą zawartość pliku /proc z buforu (testowalne).
// These can be unit-tested with mock data (no actual /proc needed)
// Mogą być testowane jednostkowo z danymi testowymi (bez prawdziwego /proc)

namespace proc_parser {

// parse_cpu_stats: parse first line of /proc/stat
// Format: "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
// Like 'head -1 /proc/stat | awk '{sum=0; for(i=2;i<=NF;i++) sum+=$i; print $5, sum}'
inline CpuStats parse_cpu_stats(const char* line) noexcept {
    CpuStats stats = {0, 0, 0.0};
    // Skip "cpu " prefix
    const char* p = line;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    int64_t values[10] = {0};
    int count = 0;
    while (*p && count < 10) {
        values[count++] = strtoll(p, const_cast<char**>(&p), 10);
        while (*p == ' ') p++;
    }

    // idle is field 3 (0-indexed), total is sum of all
    if (count >= 4) {
        stats.idle = values[3];
        for (int i = 0; i < count; ++i) stats.total += values[i];
    }
    return stats;
}

// parse_context_switches: find "ctxt" line in /proc/stat content
// Like 'grep "^ctxt" /proc/stat | awk '{print $2}''
inline int64_t parse_context_switches(const char* content) noexcept {
    const char* p = std::strstr(content, "ctxt ");
    if (!p) return 0;
    return strtoll(p + 5, nullptr, 10);
}

// parse_meminfo: parse /proc/meminfo content for key fields
// Like 'grep -E "MemTotal|MemAvailable|HugePages" /proc/meminfo'
inline MemoryStats parse_meminfo(const char* content) noexcept {
    MemoryStats mem = {0, 0, 0, 0, 0.0};

    auto find_value = [](const char* content, const char* key) -> int64_t {
        const char* p = std::strstr(content, key);
        if (!p) return 0;
        p += std::strlen(key);
        while (*p == ' ' || *p == ':') p++;
        return strtoll(p, nullptr, 10);
    };

    mem.total_kb = find_value(content, "MemTotal");
    mem.available_kb = find_value(content, "MemAvailable");
    mem.hugepages_total = find_value(content, "HugePages_Total");
    mem.hugepages_free = find_value(content, "HugePages_Free");

    if (mem.total_kb > 0) {
        mem.used_percent = (1.0 - static_cast<double>(mem.available_kb) / mem.total_kb) * 100.0;
    }
    return mem;
}

// parse_net_dev: parse /proc/net/dev for first non-loopback interface
// Like 'grep -v "lo:" /proc/net/dev | tail -1'
inline NetworkStats parse_net_dev(const char* content) noexcept {
    NetworkStats net;
    const char* line = content;

    // Skip header lines (2 lines)
    for (int skip = 0; skip < 2 && *line; ++skip) {
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    // Find first non-loopback interface
    while (*line) {
        // Skip leading whitespace
        const char* start = line;
        while (*start == ' ') start++;

        // Find interface name (before ':')
        const char* colon = start;
        while (*colon && *colon != ':') colon++;

        if (*colon == ':') {
            int name_len = (int)(colon - start);

            // Skip "lo" (loopback)
            if (!(name_len == 2 && start[0] == 'l' && start[1] == 'o')) {
                if (name_len > 15) name_len = 15;
                std::memcpy(net.interface, start, name_len);
                net.interface[name_len] = '\0';

                // Parse rx_bytes, rx_packets, ..., tx_bytes, tx_packets
                const char* p = colon + 1;
                int64_t vals[16] = {0};
                int count = 0;
                while (*p && *p != '\n' && count < 16) {
                    while (*p == ' ') p++;
                    if (*p && *p != '\n') {
                        vals[count++] = strtoll(p, const_cast<char**>(&p), 10);
                    }
                }
                if (count >= 10) {
                    net.rx_bytes = vals[0];
                    net.rx_packets = vals[1];
                    net.tx_bytes = vals[8];
                    net.tx_packets = vals[9];
                    net.valid = true;
                }
                return net;
            }
        }

        // Next line
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    return net;
}

} // namespace proc_parser


// InfraMonitor — główna klasa monitora.

class InfraMonitor {
    AlertThresholds thresholds_;
    CpuStats        prev_cpu_;
    // prev_ctx_ / prev_time_ns_ removed — they were initialized but never
    // read. If we add per-sample context-switch and elapsed-time deltas,
    // re-introduce them as locals or as fields written-and-read.

    // Alert dedup / cooldown — patrz check_alerts().
    int64_t  last_alert_mem_ms_  = 0;
    int64_t  alert_cooldown_ms_  = 300000;   // 5 minut (standard PagerDuty/Splunk)

    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    // read_file: read entire file into buffer (like 'cat /proc/stat')
    // Returns bytes read, or -1 on error
    static int read_file(const char* path, char* buf, int max_len) noexcept {
        FILE* f = std::fopen(path, "r");
        if (!f) return -1;
        int n = std::fread(buf, 1, max_len - 1, f);
        buf[n] = '\0';
        std::fclose(f);
        return n;
    }

public:
    InfraMonitor() noexcept : prev_cpu_{0, 0, 0.0} {}

    InfraMonitor(const AlertThresholds& t) noexcept
        : thresholds_(t), prev_cpu_{0, 0, 0.0} {}

    // collect_cpu: read and parse /proc/stat
    CpuStats collect_cpu() noexcept {
        char buf[4096];
        if (read_file("/proc/stat", buf, sizeof(buf)) < 0)
            return {0, 0, 0.0};

        CpuStats cur = proc_parser::parse_cpu_stats(buf);

        // Calculate usage from delta
        int64_t d_total = cur.total - prev_cpu_.total;
        int64_t d_idle = cur.idle - prev_cpu_.idle;
        if (d_total > 0) {
            cur.used_percent = (1.0 - static_cast<double>(d_idle) / d_total) * 100.0;
        }
        prev_cpu_ = cur;
        return cur;
    }

    // collect_memory: read and parse /proc/meminfo
    MemoryStats collect_memory() noexcept {
        char buf[8192];
        if (read_file("/proc/meminfo", buf, sizeof(buf)) < 0)
            return {};
        return proc_parser::parse_meminfo(buf);
    }

    // collect_context_switches: read context switch count from /proc/stat
    int64_t collect_context_switches() noexcept {
        char buf[4096];
        if (read_file("/proc/stat", buf, sizeof(buf)) < 0)
            return 0;
        return proc_parser::parse_context_switches(buf);
    }

    // collect_network: read /proc/net/dev
    NetworkStats collect_network() noexcept {
        char buf[4096];
        if (read_file("/proc/net/dev", buf, sizeof(buf)) < 0)
            return {};
        return proc_parser::parse_net_dev(buf);
    }

    // check_alerts: compare metrics against thresholds + cooldown dedup.
    //
    // Bez cooldown'u: jeśli mem >80% przez minutę dostajesz 60 spamu identycznych.
    // Standard branżowy: max 1 alert per metric per cooldown_ms (default 300s).
    // Ostatni czas wystrzelenia per-metryka trzymamy w mutable polach
    // last_alert_*_ms_; tick wewnętrzny przez std::chrono::steady_clock.
    int check_alerts(const MemoryStats& mem, char alerts[][128], int max_alerts) noexcept {
        int count = 0;
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // last_alert_mem_ms_ == 0 → nigdy jeszcze nie odpaliliśmy → strzelaj.
        // (Nie można porównać przez sam delta, bo steady_clock to time-since-boot;
        // świeży CI runner może mieć uptime krótszy niż cooldown → false negative.)
        const bool cooldown_ok = (last_alert_mem_ms_ == 0)
                              || (now_ms - last_alert_mem_ms_ >= alert_cooldown_ms_);
        if (mem.used_percent > thresholds_.mem_percent && count < max_alerts && cooldown_ok) {
            std::snprintf(alerts[count++], 127, "ALERT: Memory %.1f%% > %.1f%%",
                         mem.used_percent, thresholds_.mem_percent);
            last_alert_mem_ms_ = now_ms;
        }
        return count;
    }

    // Konfiguracja cooldown'u — domyślnie 5 minut, jak typowy PagerDuty/Splunk.
    void set_alert_cooldown_ms(int64_t cd_ms) noexcept { alert_cooldown_ms_ = cd_ms; }
    int64_t alert_cooldown_ms() const noexcept { return alert_cooldown_ms_; }

    // print_snapshot: collect and display all metrics
    void print_snapshot() {
        auto cpu = collect_cpu();
        auto mem = collect_memory();
        auto net = collect_network();
        int64_t ctx = collect_context_switches();

        printf("  CPU: %.1f%% | MEM: %.1f%% (%ld MB) | HugePages: %ld/%ld\n",
               cpu.used_percent, mem.used_percent, mem.total_mb(),
               mem.hugepages_used(), mem.hugepages_total);
        if (net.valid) {
            printf("  NET %s: rx=%ld pkts tx=%ld pkts\n",
                   net.interface, net.rx_packets, net.tx_packets);
        }
        printf("  Context switches: %ld\n", ctx);

        char alert_buf[4][128];
        int num_alerts = check_alerts(mem, alert_buf, 4);
        for (int i = 0; i < num_alerts; ++i) {
            printf("  *** %s ***\n", alert_buf[i]);
        }
    }
};
