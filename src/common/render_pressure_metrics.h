// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include "common/types.h"

namespace Common::RenderPressureMetrics {

struct Snapshot {
    u64 shader_compiles_total{};
    u64 shader_compiles_per_sec{};
    u64 last_shader_compile_ms{};
    u64 last_shader_compile_duration_us{};

    u64 pipeline_compiles_total{};
    u64 pipeline_compiles_per_sec{};
    u64 last_pipeline_compile_ms{};
    u64 last_pipeline_compile_duration_us{};

    u64 image_layer_coercions_total{};
    u64 image_layer_coercions_per_sec{};
    u64 last_image_layer_coercion_ms{};
};

inline std::atomic<u64> g_shader_compiles_total{};
inline std::atomic<u64> g_shader_compiles_per_sec{};
inline std::atomic<u64> g_last_shader_compile_ms{};
inline std::atomic<u64> g_last_shader_compile_duration_us{};
inline std::atomic<u64> g_shader_window_start_ms{};
inline std::atomic<u64> g_shader_window_count{};

inline std::atomic<u64> g_pipeline_compiles_total{};
inline std::atomic<u64> g_pipeline_compiles_per_sec{};
inline std::atomic<u64> g_last_pipeline_compile_ms{};
inline std::atomic<u64> g_last_pipeline_compile_duration_us{};
inline std::atomic<u64> g_pipeline_window_start_ms{};
inline std::atomic<u64> g_pipeline_window_count{};

inline std::atomic<u64> g_image_layer_coercions_total{};
inline std::atomic<u64> g_image_layer_coercions_per_sec{};
inline std::atomic<u64> g_last_image_layer_coercion_ms{};
inline std::atomic<u64> g_image_layer_window_start_ms{};
inline std::atomic<u64> g_image_layer_window_count{};

[[nodiscard]] inline u64 NowMs() {
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

inline void PublishRolling(std::atomic<u64>& window_start_ms, std::atomic<u64>& window_count,
                           std::atomic<u64>& per_sec, const u64 now_ms) {
    u64 start_ms = window_start_ms.load(std::memory_order_relaxed);
    if (start_ms == 0) {
        window_start_ms.store(now_ms, std::memory_order_relaxed);
        start_ms = now_ms;
    }

    const u64 count_after = window_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 window_ms = now_ms - start_ms;
    if (window_ms < 1000) {
        return;
    }

    const u64 safe_window_ms = std::max<u64>(window_ms, 1);
    per_sec.store((count_after * 1000ULL) / safe_window_ms, std::memory_order_relaxed);
    window_start_ms.store(now_ms, std::memory_order_relaxed);
    window_count.store(0, std::memory_order_relaxed);
}

inline void PublishShaderCompile(const u64 duration_us) {
    const u64 now_ms = NowMs();
    g_shader_compiles_total.fetch_add(1, std::memory_order_relaxed);
    g_last_shader_compile_ms.store(now_ms, std::memory_order_relaxed);
    g_last_shader_compile_duration_us.store(duration_us, std::memory_order_relaxed);
    PublishRolling(g_shader_window_start_ms, g_shader_window_count, g_shader_compiles_per_sec,
                   now_ms);
}

inline void PublishPipelineCompile(const u64 duration_us) {
    const u64 now_ms = NowMs();
    g_pipeline_compiles_total.fetch_add(1, std::memory_order_relaxed);
    g_last_pipeline_compile_ms.store(now_ms, std::memory_order_relaxed);
    g_last_pipeline_compile_duration_us.store(duration_us, std::memory_order_relaxed);
    PublishRolling(g_pipeline_window_start_ms, g_pipeline_window_count, g_pipeline_compiles_per_sec,
                   now_ms);
}

inline void PublishImageLayerCoercion() {
    const u64 now_ms = NowMs();
    g_image_layer_coercions_total.fetch_add(1, std::memory_order_relaxed);
    g_last_image_layer_coercion_ms.store(now_ms, std::memory_order_relaxed);
    PublishRolling(g_image_layer_window_start_ms, g_image_layer_window_count,
                   g_image_layer_coercions_per_sec, now_ms);
}

inline Snapshot GetSnapshot() {
    Snapshot snap;
    snap.shader_compiles_total = g_shader_compiles_total.load(std::memory_order_relaxed);
    snap.shader_compiles_per_sec = g_shader_compiles_per_sec.load(std::memory_order_relaxed);
    snap.last_shader_compile_ms = g_last_shader_compile_ms.load(std::memory_order_relaxed);
    snap.last_shader_compile_duration_us =
        g_last_shader_compile_duration_us.load(std::memory_order_relaxed);

    snap.pipeline_compiles_total = g_pipeline_compiles_total.load(std::memory_order_relaxed);
    snap.pipeline_compiles_per_sec = g_pipeline_compiles_per_sec.load(std::memory_order_relaxed);
    snap.last_pipeline_compile_ms = g_last_pipeline_compile_ms.load(std::memory_order_relaxed);
    snap.last_pipeline_compile_duration_us =
        g_last_pipeline_compile_duration_us.load(std::memory_order_relaxed);

    snap.image_layer_coercions_total =
        g_image_layer_coercions_total.load(std::memory_order_relaxed);
    snap.image_layer_coercions_per_sec =
        g_image_layer_coercions_per_sec.load(std::memory_order_relaxed);
    snap.last_image_layer_coercion_ms =
        g_last_image_layer_coercion_ms.load(std::memory_order_relaxed);
    return snap;
}

} // namespace Common::RenderPressureMetrics
