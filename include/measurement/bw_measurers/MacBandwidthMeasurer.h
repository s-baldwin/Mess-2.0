/*
 * macOS port: bandwidth measurer backed by traffic-generator software byte counters.
 *
 * macOS has no perf / uncore CAS counters. Each traffic_gen_multiseq.x process
 * (built on __APPLE__) publishes the bytes it has read and written in a small
 * shared file: $MESS_SW_BW_DIR/tg_<pid>.cnt (default /tmp/mess_sw_bw), updated
 * once per full pass over its arrays. This measurer samples those counters over
 * a timed window and reports them to MESS as cache-line "CAS" counts so the rest
 * of the benchmark (stabilizer, executor, results) is unchanged.
 *
 * NOTE: these are bytes ISSUED by the traffic generators, not DRAM CAS commands.
 */
#pragma once

#include "Measurement.h"

#include <map>
#include <string>
#include <utility>

class MacBandwidthMeasurer : public BandwidthMeasurer {
public:
    using BandwidthMeasurer::BandwidthMeasurer;

    bool sample_bandwidth(long long& cas_rd, long long& cas_wr, double& elapsed,
                          const std::vector<int>& mem_nodes) const override;

    bool wait_for_stabilization(int& samples_collected, long long& bw_cas_rd, long long& bw_cas_wr,
                                double& bw_elapsed, int pause, int ratio_pct,
                                bool reuse_existing_traffic_gen,
                                std::function<void()> on_sample_callback = nullptr) override;

private:
    // pid -> (read bytes, write bytes)
    using Snapshot = std::map<int, std::pair<unsigned long long, unsigned long long>>;

    static std::string counter_dir();
    static Snapshot snapshot(bool remove_dead_files);
    static void delta(const Snapshot& before, const Snapshot& after,
                      unsigned long long& rd_bytes, unsigned long long& wr_bytes);

    // One timed sample: bytes moved by live traffic generators during the window.
    bool sample_window(unsigned long long& rd_bytes, unsigned long long& wr_bytes,
                       double& seconds, int& live_generators) const;

    double window_ms() const;
    int line_bytes() const;
};
