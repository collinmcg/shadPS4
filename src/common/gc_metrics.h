// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include "common/types.h"

namespace Common::GcMetrics {

struct Snapshot {
    u64 tick{};
    u64 state{};
    u64 used_memory_bytes{};
    u64 fast_queue{};
    u64 slow_queue{};
    u64 selected{};
    u64 deleted{};
    u64 skipped{};
    u64 readback_bytes{};
    u64 pass_duration_us{};
    u64 rolling_passes_per_sec{};
    u64 rolling_readback_mb_per_sec{};
    u64 rolling_avg_pass_us{};
    u64 rolling_max_pass_us{};
};

inline std::atomic<u64> g_tick{};
inline std::atomic<u64> g_state{};
inline std::atomic<u64> g_used_memory_bytes{};
inline std::atomic<u64> g_fast_queue{};
inline std::atomic<u64> g_slow_queue{};
inline std::atomic<u64> g_selected{};
inline std::atomic<u64> g_deleted{};
inline std::atomic<u64> g_skipped{};
inline std::atomic<u64> g_readback_bytes{};
inline std::atomic<u64> g_pass_duration_us{};
inline std::atomic<u64> g_rolling_passes_per_sec{};
inline std::atomic<u64> g_rolling_readback_mb_per_sec{};
inline std::atomic<u64> g_rolling_avg_pass_us{};
inline std::atomic<u64> g_rolling_max_pass_us{};

inline void PublishPass(u64 tick, u64 state, u64 used_memory_bytes, u64 fast_queue, u64 slow_queue,
                        u64 selected, u64 deleted, u64 skipped, u64 readback_bytes,
                        u64 pass_duration_us) {
    g_tick.store(tick, std::memory_order_relaxed);
    g_state.store(state, std::memory_order_relaxed);
    g_used_memory_bytes.store(used_memory_bytes, std::memory_order_relaxed);
    g_fast_queue.store(fast_queue, std::memory_order_relaxed);
    g_slow_queue.store(slow_queue, std::memory_order_relaxed);
    g_selected.store(selected, std::memory_order_relaxed);
    g_deleted.store(deleted, std::memory_order_relaxed);
    g_skipped.store(skipped, std::memory_order_relaxed);
    g_readback_bytes.store(readback_bytes, std::memory_order_relaxed);
    g_pass_duration_us.store(pass_duration_us, std::memory_order_relaxed);
}

inline void PublishRolling(u64 passes_per_sec, u64 readback_mb_per_sec, u64 avg_pass_us,
                           u64 max_pass_us) {
    g_rolling_passes_per_sec.store(passes_per_sec, std::memory_order_relaxed);
    g_rolling_readback_mb_per_sec.store(readback_mb_per_sec, std::memory_order_relaxed);
    g_rolling_avg_pass_us.store(avg_pass_us, std::memory_order_relaxed);
    g_rolling_max_pass_us.store(max_pass_us, std::memory_order_relaxed);
}

inline Snapshot GetSnapshot() {
    Snapshot snap;
    snap.tick = g_tick.load(std::memory_order_relaxed);
    snap.state = g_state.load(std::memory_order_relaxed);
    snap.used_memory_bytes = g_used_memory_bytes.load(std::memory_order_relaxed);
    snap.fast_queue = g_fast_queue.load(std::memory_order_relaxed);
    snap.slow_queue = g_slow_queue.load(std::memory_order_relaxed);
    snap.selected = g_selected.load(std::memory_order_relaxed);
    snap.deleted = g_deleted.load(std::memory_order_relaxed);
    snap.skipped = g_skipped.load(std::memory_order_relaxed);
    snap.readback_bytes = g_readback_bytes.load(std::memory_order_relaxed);
    snap.pass_duration_us = g_pass_duration_us.load(std::memory_order_relaxed);
    snap.rolling_passes_per_sec = g_rolling_passes_per_sec.load(std::memory_order_relaxed);
    snap.rolling_readback_mb_per_sec =
        g_rolling_readback_mb_per_sec.load(std::memory_order_relaxed);
    snap.rolling_avg_pass_us = g_rolling_avg_pass_us.load(std::memory_order_relaxed);
    snap.rolling_max_pass_us = g_rolling_max_pass_us.load(std::memory_order_relaxed);
    return snap;
}

} // namespace Common::GcMetrics
