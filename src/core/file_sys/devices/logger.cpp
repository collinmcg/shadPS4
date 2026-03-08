// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "common/gc_metrics.h"
#include "common/logging/log.h"
#include "common/render_pressure_metrics.h"
#include "core/file_sys/devices/logger.h"
#include "core/libraries/kernel/file_system.h"

namespace Core::Devices {

Logger::Logger(std::string prefix, bool is_err) : prefix(std::move(prefix)), is_err(is_err) {}

Logger::~Logger() = default;

s64 Logger::write(const void* buf, u64 nbytes) {
    log(static_cast<const char*>(buf), nbytes);
    return nbytes;
}

s64 Logger::writev(const Libraries::Kernel::OrbisKernelIovec* iov, s32 iovcnt) {
    u64 total_written = 0;
    for (int i = 0; i < iovcnt; i++) {
        log(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
        total_written += iov[i].iov_len;
    }
    return total_written;
}

s64 Logger::pwrite(const void* buf, u64 nbytes, s64 offset) {
    log(static_cast<const char*>(buf), nbytes);
    return nbytes;
}

s32 Logger::fsync() {
    log_flush();
    return 0;
}

void Logger::log(const char* buf, u64 nbytes) {
    std::scoped_lock lock{mtx};
    const char* end = buf + nbytes;
    for (const char* it = buf; it < end; ++it) {
        char c = *it;
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            log_flush();
            continue;
        }
        buffer.push_back(c);
    }
}

void Logger::log_flush() {
    std::scoped_lock lock{mtx};
    if (buffer.empty()) {
        return;
    }

    const std::string line{buffer.begin(), buffer.end()};
    if (is_err) {
        LOG_ERROR(Tty, "[{}] {}", prefix, line);
    } else {
        LOG_INFO(Tty, "[{}] {}", prefix, line);
        if (line.find("Stall during rendering at flush") != std::string::npos) {
            const auto gc_snap = Common::GcMetrics::GetSnapshot();
            const auto pressure_snap = Common::RenderPressureMetrics::GetSnapshot();
            const u64 now_ms = Common::RenderPressureMetrics::NowMs();
            const auto age_ms = [now_ms](u64 stamp_ms) {
                return stamp_ms > 0 ? (now_ms - stamp_ms) : std::numeric_limits<u64>::max();
            };

            LOG_INFO(Tty,
                     "[gc-correlation] stall_line='{}' gc_tick={} state={} used_mb={} "
                     "fast_q={} slow_q={} selected={} deleted={} skipped={} "
                     "readback_kb={} pass_us={} rolling_passes_s={} rolling_readback_mb_s={} "
                     "rolling_avg_pass_us={} rolling_max_pass_us={} shader_compiles_s={} "
                     "pipeline_compiles_s={} last_shader_compile_ms_ago={} "
                     "last_pipeline_compile_ms_ago={} last_shader_compile_us={} "
                     "last_pipeline_compile_us={} layer_coercions_s={} "
                     "last_layer_coercion_ms_ago={}",
                     line, gc_snap.tick, gc_snap.state,
                     gc_snap.used_memory_bytes / (1024ULL * 1024ULL), gc_snap.fast_queue,
                     gc_snap.slow_queue, gc_snap.selected, gc_snap.deleted, gc_snap.skipped,
                     gc_snap.readback_bytes / 1024ULL, gc_snap.pass_duration_us,
                     gc_snap.rolling_passes_per_sec, gc_snap.rolling_readback_mb_per_sec,
                     gc_snap.rolling_avg_pass_us, gc_snap.rolling_max_pass_us,
                     pressure_snap.shader_compiles_per_sec, pressure_snap.pipeline_compiles_per_sec,
                     age_ms(pressure_snap.last_shader_compile_ms),
                     age_ms(pressure_snap.last_pipeline_compile_ms),
                     pressure_snap.last_shader_compile_duration_us,
                     pressure_snap.last_pipeline_compile_duration_us,
                     pressure_snap.image_layer_coercions_per_sec,
                     age_ms(pressure_snap.last_image_layer_coercion_ms));
        }
    }
    buffer.clear();
}

} // namespace Core::Devices