/*
 * macOS port: bandwidth measurer backed by traffic-generator software byte counters.
 * See include/measurement/bw_measurers/MacBandwidthMeasurer.h for the design notes.
 */
#include "measurement/bw_measurers/MacBandwidthMeasurer.h"
#include "measurement/BandwidthStabilizer.h"
#include "measurement/TheoreticalPeakCalculator.h"
#include "ProcessManager.h"
#include "Utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <signal.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace {
double env_double(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return fallback;
    char* end = nullptr;
    double d = std::strtod(v, &end);
    return (end != v && d > 0) ? d : fallback;
}
}  // namespace

std::string MacBandwidthMeasurer::counter_dir() {
    const char* d = std::getenv("MESS_SW_BW_DIR");
    return (d && d[0]) ? std::string(d) : std::string("/tmp/mess_sw_bw");
}

MacBandwidthMeasurer::Snapshot MacBandwidthMeasurer::snapshot(bool remove_dead_files) {
    Snapshot snap;
    const std::string dir_path = counter_dir();
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) return snap;
    while (dirent* e = readdir(dir)) {
        int pid = 0;
        if (std::sscanf(e->d_name, "tg_%d.cnt", &pid) != 1 || pid <= 0) continue;
        const std::string path = dir_path + "/" + e->d_name;
        if (::kill(pid, 0) != 0) {  // generator no longer running
            if (remove_dead_files) ::unlink(path.c_str());
            continue;
        }
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;
        unsigned long long v[2] = {0, 0};
        if (::pread(fd, v, sizeof(v), 0) == static_cast<ssize_t>(sizeof(v))) {
            snap[pid] = {v[0], v[1]};
        }
        ::close(fd);
    }
    closedir(dir);
    return snap;
}

void MacBandwidthMeasurer::delta(const Snapshot& before, const Snapshot& after,
                                 unsigned long long& rd_bytes, unsigned long long& wr_bytes) {
    rd_bytes = 0;
    wr_bytes = 0;
    // Per-process deltas, so generators that start or exit mid-window cannot skew the total.
    for (const auto& [pid, now] : after) {
        auto it = before.find(pid);
        if (it == before.end()) continue;
        if (now.first >= it->second.first) rd_bytes += now.first - it->second.first;
        if (now.second >= it->second.second) wr_bytes += now.second - it->second.second;
    }
}

double MacBandwidthMeasurer::window_ms() const {
    // Counters advance once per full pass over a generator's arrays, so a window must span
    // many passes. MESS_MAC_BW_WINDOW_MS overrides the default minimum of 500 ms.
    return std::max(sampling_interval_ms_, env_double("MESS_MAC_BW_WINDOW_MS", 500.0));
}

int MacBandwidthMeasurer::line_bytes() const {
    // Must match the executor's cache_line_size_ (TLBMeasurement::cache_line_size, which on macOS
    // is get_cache_line_size() = sysctl hw.cachelinesize), because the executor converts the counts
    // back to bytes with it. Read the same sysctl here so mess-profiler need not link TlbUtils.
    static const int line = []() {
#ifdef __APPLE__
        size_t v = 0;
        size_t len = sizeof(v);
        if (sysctlbyname("hw.cachelinesize", &v, &len, nullptr, 0) == 0 && v > 0) return static_cast<int>(v);
#endif
        return 64;
    }();
    return line;
}

bool MacBandwidthMeasurer::sample_window(unsigned long long& rd_bytes, unsigned long long& wr_bytes,
                                         double& seconds, int& live_generators) const {
    const Snapshot before = snapshot(false);
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(window_ms() * 1000.0)));
    const Snapshot after = snapshot(false);
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    delta(before, after, rd_bytes, wr_bytes);
    live_generators = static_cast<int>(after.size());
    return seconds > 0.0 && live_generators > 0;
}

bool MacBandwidthMeasurer::sample_bandwidth(long long& cas_rd, long long& cas_wr, double& elapsed,
                                            const std::vector<int>& mem_nodes) const {
    (void)mem_nodes;
    extra_perf_values_.clear();
    unsigned long long rd = 0, wr = 0;
    int live = 0;
    if (!sample_window(rd, wr, elapsed, live)) return false;
    cas_rd = static_cast<long long>(rd / static_cast<unsigned long long>(line_bytes()));
    cas_wr = static_cast<long long>(wr / static_cast<unsigned long long>(line_bytes()));
    return true;
}

bool MacBandwidthMeasurer::wait_for_stabilization(int& samples_taken, long long& last_cas_rd,
                                                  long long& last_cas_wr, double& last_elapsed,
                                                  int pause, int ratio, bool fast_resume,
                                                  std::function<void()> on_sample) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    extra_perf_values_.clear();
    const bool verbose2 = config_.verbosity >= 2;
    const bool verbose3 = config_.verbosity >= 3;

    auto relaunch_current_traffic_gen = [&]() -> bool {
        if (!this->relaunch_traffic_gen(ratio, pause, get_traffic_gen_cores())) return false;
        traffic_gen_manager_->wait_for_traffic_gen_ready(120);
        return true;
    };

    int relaunch_attempts = 0;
    if (!traffic_gen_manager_->is_traffic_gen_running(traffic_gen_manager_->active_traffic_gen_pid())) {
        if (verbose2) std::cout << "    TrafficGen not running, relaunching..." << '\n';
        if (!relaunch_current_traffic_gen()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        relaunch_attempts++;
    }

    snapshot(true);  // drop counter files left behind by generators from earlier points

    const int expected_cores = get_traffic_gen_cores();
    const int socket_cores = (sys_info_.socket_count > 0) ? sys_info_.sockets[0].core_count : 0;
    double peak_gb_s = TheoreticalPeakCalculator::calculate_achievable_peak(caps_, expected_cores, socket_cores, false);
    // macOS reports no channel info, so the calculator usually returns 0. MESS_MAC_PEAK_GBS lets you
    // supply your own theoretical peak (used only for the stabilizer's noise/warm-up thresholds).
    peak_gb_s = env_double("MESS_MAC_PEAK_GBS", peak_gb_s > 0 ? peak_gb_s : 300.0);

    // Health check via the counter files: /proc-based process discovery does not exist on macOS.
    TrafficGenHealthChecker health_checker;
    health_checker.is_pid_alive = [](int pid) -> bool { return pid > 0 && ::kill(pid, 0) == 0; };
    health_checker.count_running_instances = []() -> int { return static_cast<int>(snapshot(false).size()); };
    health_checker.expected_instance_count = expected_cores;

    const double target_s = sampling_interval_ms_ / 1000.0;
    const unsigned long long line = static_cast<unsigned long long>(line_bytes());

    if (verbose3) {
        std::cout << "      [MacBW] software byte counters in " << counter_dir()
                  << ", window " << std::fixed << std::setprecision(0) << window_ms() << " ms"
                  << ", expected generators " << expected_cores
                  << ", peak used for thresholds " << std::setprecision(1) << peak_gb_s << " GB/s" << '\n';
    }

    BandwidthStabilizer stabilizer(peak_gb_s, pause, health_checker, fast_resume ? 5 : 7, 0.05, config_.verbosity);

    const int OVERALL_TIMEOUT_SECONDS = 60;
    const auto loop_start = std::chrono::steady_clock::now();
    int total_samples = 0;
    bool success = false;
    long long final_rd = 0, final_wr = 0;

    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - loop_start).count() < OVERALL_TIMEOUT_SECONDS) {
        unsigned long long rd = 0, wr = 0;
        double secs = 0.0;
        int live = 0;
        sample_window(rd, wr, secs, live);
        if (secs <= 0.0) continue;

        // Normalize to MESS's nominal sampling interval, exactly as the perf backend does.
        const double norm = target_s / secs;
        const long long norm_rd = static_cast<long long>(static_cast<double>(rd / line) * norm);
        const long long norm_wr = static_cast<long long>(static_cast<double>(wr / line) * norm);
        const double bw_gb_s = static_cast<double>(rd + wr) / (secs * 1e9);

        total_samples++;
        if (on_sample) on_sample();

        if (total_samples % 20 == 0 &&
            !traffic_gen_manager_->is_traffic_gen_running(traffic_gen_manager_->active_traffic_gen_pid())) {
            if (relaunch_attempts < 2 && relaunch_current_traffic_gen()) {
                if (verbose2) std::cout << "    TrafficGen died during stabilization, relaunched" << '\n';
                relaunch_attempts++;
                stabilizer.reset();
                continue;
            }
            samples_taken = total_samples;
            return false;
        }

        final_rd = norm_rd;
        final_wr = norm_wr;

        StabilizationResult result = stabilizer.add_sample(norm_rd, norm_wr, bw_gb_s);
        if (result == StabilizationResult::ZOMBIE_DETECTED) {
            if (verbose2) std::cout << "    [ZOMBIE] TrafficGen died, aborting stabilization" << '\n';
            samples_taken = total_samples;
            return false;
        }

        if (verbose3) {
            const double read_ratio = (rd + wr) > 0 ? static_cast<double>(rd) / static_cast<double>(rd + wr) : 0.0;
            std::cout << "      [Sample " << total_samples << "] SW bytes: " << (rd + wr)
                      << " over " << std::fixed << std::setprecision(3) << secs << "s, live gens " << live;
            stabilizer.print_status(read_ratio, bw_gb_s, true);
        }

        if (stabilizer.is_stable()) {
            success = true;
            break;
        }
    }

    samples_taken = total_samples;

    if (success) {
        long long agg_rd = 0, agg_wr = 0;
        const auto& samples = stabilizer.get_samples();
        for (const auto& s : samples) {
            agg_rd += s.cas_rd;
            agg_wr += s.cas_wr;
        }
        last_cas_rd = agg_rd;
        last_cas_wr = agg_wr;
        last_elapsed = static_cast<double>(samples.size()) * target_s;
        if (verbose3) {
            std::cout << "      [BW AGGREGATE] Using " << samples.size() << " stable samples" << '\n';
        }
        return true;
    }

    if (config_.verbosity >= 1) {
        std::cout << "\n    ⚠ WARNING: BW did not stabilize after " << OVERALL_TIMEOUT_SECONDS
                  << "s; using the last sample" << '\n';
    }
    last_cas_rd = final_rd;
    last_cas_wr = final_wr;
    last_elapsed = target_s;
    return false;
}
