// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include "common/alignment.h"
#include "common/debug.h"
#include "common/gc_metrics.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {
namespace {

[[nodiscard]] bool IsBufferGcVerboseLoggingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_LOG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] bool IsBufferGcTraceLoggingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_TRACE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] bool IsBufferGcDisabled() {
    static const bool disabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_DISABLE");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return disabled;
}

[[nodiscard]] bool IsBufferGcDryRunEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_DRY_RUN");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] bool IsBufferGcReadbackOnlyEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_READBACK_ONLY");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] bool IsBufferGcDeleteOnlyEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_DELETE_ONLY");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] bool IsBufferGcSyncReadbackEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_SYNC_READBACK");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] bool IsBufferGcMetricsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_VK_BUFFER_GC_METRICS");
        if (!value || value[0] == '\0') {
            return true;
        }
        return value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] u64 ParseBufferGcEnvU64(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    char* end_ptr = nullptr;
    const auto parsed = std::strtoull(value, &end_ptr, 10);
    if (end_ptr == value) {
        return fallback;
    }
    return static_cast<u64>(parsed);
}

[[nodiscard]] int GetBufferGcMaxDeletions(bool aggressive) {
    constexpr int normal_default = 4;
    constexpr int aggressive_default = 8;

    const u64 parsed =
        ParseBufferGcEnvU64(aggressive ? "SHADPS4_VK_BUFFER_GC_MAX_DELETIONS_AGGRESSIVE"
                                       : "SHADPS4_VK_BUFFER_GC_MAX_DELETIONS",
                            aggressive ? aggressive_default : normal_default);
    return static_cast<int>(std::clamp<u64>(parsed, 1, 64));
}

[[nodiscard]] u64 GetBufferGcReadbackBudgetBytes(bool aggressive) {
    constexpr u64 normal_default_mb = 64;
    constexpr u64 aggressive_default_mb = 64;

    const u64 budget_mb =
        ParseBufferGcEnvU64(aggressive ? "SHADPS4_VK_BUFFER_GC_READBACK_BUDGET_MB_AGGRESSIVE"
                                       : "SHADPS4_VK_BUFFER_GC_READBACK_BUDGET_MB",
                            aggressive ? aggressive_default_mb : normal_default_mb);
    constexpr u64 one_mb = 1024ULL * 1024ULL;
    return std::clamp<u64>(budget_mb, 8, 1024) * one_mb;
}

[[nodiscard]] u64 GetBufferGcIntervalTicks() {
    const u64 interval = ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_INTERVAL_TICKS", 16);
    return std::clamp<u64>(interval, 1, 1024);
}

[[nodiscard]] u64 GetBufferGcSelectIntervalTicks() {
    const u64 interval = ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_SELECT_INTERVAL_TICKS", 32);
    return std::clamp<u64>(interval, 1, 4096);
}

[[nodiscard]] int GetBufferGcSelectionQuota(bool aggressive) {
    const u64 quota =
        ParseBufferGcEnvU64(aggressive ? "SHADPS4_VK_BUFFER_GC_SELECT_QUOTA_AGGRESSIVE"
                                       : "SHADPS4_VK_BUFFER_GC_SELECT_QUOTA",
                            aggressive ? 96 : 48);
    return static_cast<int>(std::clamp<u64>(quota, 8, 2048));
}

[[nodiscard]] u64 GetBufferGcFastClassBytes() {
    const u64 mb = ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_FAST_CLASS_MB", 4);
    return std::clamp<u64>(mb, 1, 256) * 1024ULL * 1024ULL;
}

[[nodiscard]] u64 GetBufferGcProtectTicks() {
    const u64 ticks = ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_PROTECT_TICKS", 512);
    return std::clamp<u64>(ticks, 32, 16384);
}

[[nodiscard]] u64 GetBufferGcProtectThreshold() {
    const u64 hits = ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_PROTECT_TOUCHES", 8);
    return std::clamp<u64>(hits, 2, 64);
}

[[nodiscard]] u64 GetBufferGcAgeThreshold(bool aggressive) {
    const u64 ticks =
        ParseBufferGcEnvU64(aggressive ? "SHADPS4_VK_BUFFER_GC_MIN_AGE_TICKS_AGGRESSIVE"
                                       : "SHADPS4_VK_BUFFER_GC_MIN_AGE_TICKS",
                            aggressive ? 64 : 160);
    return std::clamp<u64>(ticks, 8, 4096);
}

[[nodiscard]] u64 GetBufferGcQueueLimit() {
    const u64 limit = ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_QUEUE_LIMIT", 512);
    return std::clamp<u64>(limit, 16, 8192);
}

[[nodiscard]] u64 GetBufferGcQueueTarget(u64 queue_limit) {
    const u64 target = ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_QUEUE_TARGET", 96);
    return std::clamp<u64>(target, 8, queue_limit);
}

[[nodiscard]] bool HasBufferGcEnvValue(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0';
}

[[nodiscard]] u64 GetBufferGcRetireThresholdBytes(const char* name, u64 fallback_mb) {
    constexpr u64 one_mb = 1024ULL * 1024ULL;
    const u64 threshold_mb = ParseBufferGcEnvU64(name, fallback_mb);
    return std::clamp<u64>(threshold_mb, 1, 1024) * one_mb;
}

[[nodiscard]] u64 GetBufferGcRetireClassTicks(const char* normal_name, const char* aggressive_name,
                                              bool aggressive, u64 fallback) {
    const u64 ticks = ParseBufferGcEnvU64(aggressive ? aggressive_name : normal_name, fallback);
    return std::clamp<u64>(ticks, 16, 4096);
}

[[nodiscard]] u64 GetBufferGcRetireTicks(u64 size_bytes, bool aggressive) {
    const char* global_name = aggressive ? "SHADPS4_VK_BUFFER_GC_RETIRE_TICKS_AGGRESSIVE"
                                         : "SHADPS4_VK_BUFFER_GC_RETIRE_TICKS";
    if (HasBufferGcEnvValue(global_name)) {
        const u64 ticks = ParseBufferGcEnvU64(global_name, 128);
        return std::clamp<u64>(ticks, 16, 4096);
    }

    const u64 medium_threshold_bytes =
        GetBufferGcRetireThresholdBytes("SHADPS4_VK_BUFFER_GC_RETIRE_MEDIUM_MB", 1);
    const u64 large_threshold_bytes =
        GetBufferGcRetireThresholdBytes("SHADPS4_VK_BUFFER_GC_RETIRE_LARGE_MB", 8);

    if (size_bytes >= large_threshold_bytes) {
        return GetBufferGcRetireClassTicks("SHADPS4_VK_BUFFER_GC_RETIRE_TICKS_LARGE",
                                           "SHADPS4_VK_BUFFER_GC_RETIRE_TICKS_LARGE_AGGRESSIVE",
                                           aggressive, 1024);
    }
    if (size_bytes >= medium_threshold_bytes) {
        return GetBufferGcRetireClassTicks("SHADPS4_VK_BUFFER_GC_RETIRE_TICKS_MEDIUM",
                                           "SHADPS4_VK_BUFFER_GC_RETIRE_TICKS_MEDIUM_AGGRESSIVE",
                                           aggressive, 512);
    }
    return GetBufferGcRetireClassTicks("SHADPS4_VK_BUFFER_GC_RETIRE_TICKS_SMALL",
                                       "SHADPS4_VK_BUFFER_GC_RETIRE_TICKS_SMALL_AGGRESSIVE",
                                       aggressive, 256);
}

[[nodiscard]] int GetBufferGcMaxRetiredDeletes(bool aggressive) {
    const u64 limit =
        ParseBufferGcEnvU64(aggressive ? "SHADPS4_VK_BUFFER_GC_MAX_RETIRED_DELETIONS_AGGRESSIVE"
                                       : "SHADPS4_VK_BUFFER_GC_MAX_RETIRED_DELETIONS",
                            aggressive ? 8 : 4);
    return static_cast<int>(std::clamp<u64>(limit, 1, 64));
}

[[nodiscard]] u64 GetBufferGcCriticalDeleteOvershootBytes() {
    constexpr u64 one_mb = 1024ULL * 1024ULL;
    const u64 overshoot_mb =
        ParseBufferGcEnvU64("SHADPS4_VK_BUFFER_GC_CRITICAL_DELETE_OVERSHOOT_MB", 512);
    return std::clamp<u64>(overshoot_mb, 64, 4096) * one_mb;
}

} // namespace

static constexpr size_t DataShareBufferSize = 64_KB;
static constexpr size_t StagingBufferSize = 512_MB;
static constexpr size_t DownloadBufferSize = 32_MB;
static constexpr size_t UboStreamBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 128_MB;

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         AmdGpu::Liverpool* liverpool_, TextureCache& texture_cache_,
                         PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      fault_manager{instance, scheduler, *this, CACHING_PAGEBITS, CACHING_NUMPAGES},
      staging_buffer{instance, scheduler, MemoryUsage::Upload, StagingBufferSize},
      stream_buffer{instance, scheduler, MemoryUsage::Stream, UboStreamBufferSize},
      download_buffer{instance, scheduler, MemoryUsage::Download, DownloadBufferSize},
      device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, DeviceBufferSize},
      gds_buffer{instance, scheduler, MemoryUsage::Stream, 0, AllFlags, DataShareBufferSize},
      bda_pagetable_buffer{instance, scheduler, MemoryUsage::DeviceLocal,
                           0,        AllFlags,  BDA_PAGETABLE_SIZE} {
    Vulkan::SetObjectName(instance.GetDevice(), gds_buffer.Handle(), "GDS Buffer");
    Vulkan::SetObjectName(instance.GetDevice(), bda_pagetable_buffer.Handle(),
                          "BDA Page Table Buffer");

    memory_tracker = std::make_unique<MemoryTracker>(tracker);

    std::memset(gds_buffer.mapped_data.data(), 0, DataShareBufferSize);

    // Ensure the first slot is used for the null buffer
    const auto null_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
    ASSERT(null_id.index == 0);
    const vk::Buffer& null_buffer = slot_buffers[null_id].buffer;
    Vulkan::SetObjectName(instance.GetDevice(), null_buffer, "Null Buffer");

    // Set up garbage collection parameters
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = DEFAULT_TRIGGER_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    trigger_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_TRIGGER_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size) {
    if (!IsRegionRegistered(device_addr, size)) {
        return;
    }
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write) {
    liverpool->SendCommand<true>([this, device_addr, size, is_write] {
        Buffer& buffer = slot_buffers[FindBuffer(device_addr, size)];
        static_cast<void>(DownloadBufferMemory<false>(buffer, device_addr, size, is_write));
    });
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size, bool is_write,
                                       bool allow_temporary_download_buffer) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    memory_tracker->ForEachDownloadRange<false>(
        device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            const VAddr buffer_addr = buffer.CpuAddr();
            const auto add_download = [&](VAddr start, VAddr end) {
                const u64 new_offset = start - buffer_addr;
                const u64 new_size = end - start;
                copies.push_back(vk::BufferCopy{
                    .srcOffset = new_offset,
                    .dstOffset = total_size_bytes,
                    .size = new_size,
                });
                // Align up to avoid cache conflicts
                constexpr u64 align = 64ULL;
                constexpr u64 mask = ~(align - 1ULL);
                total_size_bytes += (new_size + align - 1) & mask;
            };
            gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, add_download);
            gpu_modified_ranges.Subtract(device_addr_out, range_size);
        });
    if (total_size_bytes == 0) {
        return true;
    }

    std::unique_ptr<Buffer> temp_download_buffer{};
    u8* download_ptr = nullptr;
    u64 staging_offset = 0;
    vk::Buffer download_dst_buffer = VK_NULL_HANDLE;

    if (total_size_bytes > DownloadBufferSize) {
        if (!allow_temporary_download_buffer) {
            LOG_INFO(Render_Vulkan,
                     "Buffer readback deferred: request exceeded staging capacity and temporary "
                     "fallback is disabled: request={} bytes copies={} staging_cap={} bytes "
                     "buffer_id={} buffer_addr={:#x} request_addr={:#x} request_size={} bytes "
                     "(async={}, is_write={})",
                     total_size_bytes, copies.size(), DownloadBufferSize, buffer.LRUId(),
                     buffer.CpuAddr(), device_addr, size, async, is_write);
            return false;
        }

        LOG_INFO(Render_Vulkan,
                 "Buffer readback request exceeded staging capacity, using temporary download "
                 "buffer: request={} bytes copies={} staging_cap={} bytes buffer_id={} "
                 "buffer_addr={:#x} request_addr={:#x} request_size={} bytes "
                 "(async={}, is_write={})",
                 total_size_bytes, copies.size(), DownloadBufferSize, buffer.LRUId(),
                 buffer.CpuAddr(), device_addr, size, async, is_write);

        temp_download_buffer =
            std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Download, 0,
                                     vk::BufferUsageFlagBits::eTransferDst, total_size_bytes);
        if (temp_download_buffer->mapped_data.empty()) {
            LOG_ERROR(Render_Vulkan,
                      "Buffer readback temporary download buffer map failed: request={} bytes "
                      "buffer_id={} buffer_addr={:#x} request_addr={:#x} request_size={} bytes "
                      "(async={}, is_write={})",
                      total_size_bytes, buffer.LRUId(), buffer.CpuAddr(), device_addr, size, async,
                      is_write);
            return false;
        }

        download_ptr = temp_download_buffer->mapped_data.data();
        download_dst_buffer = temp_download_buffer->Handle();
    } else {
        const auto [download, offset] = download_buffer.Map(total_size_bytes);
        if (!download) {
            LOG_ERROR(Render_Vulkan,
                      "Buffer readback map failed: request={} bytes staging_cap={} bytes "
                      "buffer_id={} buffer_addr={:#x} request_addr={:#x} request_size={} bytes "
                      "(async={}, is_write={})",
                      total_size_bytes, DownloadBufferSize, buffer.LRUId(), buffer.CpuAddr(),
                      device_addr, size, async, is_write);
            return false;
        }

        for (auto& copy : copies) {
            // Modify copies to have the staging offset in mind
            copy.dstOffset += offset;
        }
        staging_offset = offset;
        download_ptr = download;
        download_buffer.Commit();
        download_dst_buffer = download_buffer.Handle();
    }

    const VAddr buffer_cpu_addr = buffer.CpuAddr();
    const u32 buffer_lru_id = buffer.LRUId();
    const VAddr request_device_addr = device_addr;
    const u64 request_size = size;
    const bool request_is_write = is_write;
    u8* const download_ptr_const = download_ptr;
    const u64 staging_offset_const = staging_offset;
    auto copies_for_writeback = copies;

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.copyBuffer(buffer.buffer, download_dst_buffer, copies);

    auto write_data = [this, copies = std::move(copies_for_writeback), buffer_cpu_addr,
                       buffer_lru_id, request_device_addr, request_size, request_is_write,
                       staging_offset_const, download_ptr_const,
                       temp_download = std::move(temp_download_buffer)]() mutable {
        auto* memory = Core::Memory::Instance();
        u32 invalid_mappings = 0;
        u32 missing_backings = 0;

        for (const auto& copy : copies) {
            const VAddr copy_device_addr = buffer_cpu_addr + copy.srcOffset;
            const u64 dst_offset = copy.dstOffset - staging_offset_const;

            if (!memory->IsValidMapping(copy_device_addr, copy.size)) {
                ++invalid_mappings;
                if (invalid_mappings <= 3) {
                    LOG_WARNING(Render_Vulkan,
                                "Buffer GC readback writeback skipped invalid mapping: "
                                "buffer_lru_id={} copy_addr={:#x} copy_size={} "
                                "request_addr={:#x} request_size={} bytes",
                                buffer_lru_id, copy_device_addr, copy.size, request_device_addr,
                                request_size);
                }
                continue;
            }

            if (!memory->TryWriteBacking(std::bit_cast<u8*>(copy_device_addr),
                                         download_ptr_const + dst_offset, copy.size)) {
                ++missing_backings;
                if (missing_backings <= 3) {
                    LOG_WARNING(Render_Vulkan,
                                "Buffer GC readback writeback had no physical backing: "
                                "buffer_lru_id={} copy_addr={:#x} copy_size={} "
                                "request_addr={:#x} request_size={} bytes",
                                buffer_lru_id, copy_device_addr, copy.size, request_device_addr,
                                request_size);
                }
            }
        }

        if (invalid_mappings > 0 || missing_backings > 0) {
            LOG_WARNING(Render_Vulkan,
                        "Buffer GC readback writeback summary: buffer_lru_id={} "
                        "invalid_mappings={} missing_backings={} request_addr={:#x} "
                        "request_size={} bytes",
                        buffer_lru_id, invalid_mappings, missing_backings, request_device_addr,
                        request_size);
        }

        memory_tracker->UnmarkRegionAsGpuModified(request_device_addr, request_size);
        if (request_is_write) {
            memory_tracker->MarkRegionAsCpuModified(request_device_addr, request_size);
        }
        if (temp_download) {
            temp_download.reset();
        }
    };

    if constexpr (async) {
        scheduler.DeferOperation(std::move(write_data));
    } else {
        scheduler.Finish();
        write_data();
    }

    return true;
}

void BufferCache::BindVertexBuffers(const Vulkan::GraphicsPipeline& pipeline) {
    const auto& regs = liverpool->regs;
    Vulkan::VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    Vulkan::VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    Vulkan::VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs.
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.setVertexInputEXT(bindings, attributes);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        vk::Buffer vk_buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    Vulkan::VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    Vulkan::VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        const auto [buffer, offset] = ObtainBuffer(range.base_address, size, false);
        range.vk_buffer = buffer->buffer;
        range.offset = offset;
    }

    // Bind vertex buffers
    Vulkan::VertexInputs<vk::Buffer> host_buffers;
    Vulkan::VertexInputs<vk::DeviceSize> host_offsets;
    Vulkan::VertexInputs<vk::DeviceSize> host_sizes;
    Vulkan::VertexInputs<vk::DeviceSize> host_strides;
    const auto null_buffer =
        instance.IsNullDescriptorSupported() ? VK_NULL_HANDLE : GetBuffer(NULL_BUFFER_ID).Handle();
    for (const auto& buffer : guest_buffers) {
        if (buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->vk_buffer);
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(null_buffer);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void BufferCache::BindIndexBuffer(u32 index_offset) {
    const auto& regs = liverpool->regs;

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [vk_buffer, offset] = ObtainBuffer(index_address, index_buffer_size, false);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindIndexBuffer(vk_buffer->Handle(), offset, index_type);
}

void BufferCache::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0, "GDS offset must be dword aligned");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + num_bytes / sizeof(u32), value);
            return;
        }
    }
    Buffer* buffer = [&] {
        if (is_gds) {
            return &gds_buffer;
        }
        const auto [buffer, offset] = ObtainBuffer(address, num_bytes, true);
        return buffer;
    }();
    buffer->Fill(buffer->Offset(address), num_bytes, value);
}

void BufferCache::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds && !IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
        // Without a readback there's nothing we can do with this
        // Fallback to creating dst buffer on GPU to at least have this data there
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    auto& src_buffer = [&] -> const Buffer& {
        if (src_gds) {
            return gds_buffer;
        }
        const auto buffer_id = FindBuffer(src, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, src, num_bytes, false, true);
        return buffer;
    }();
    auto& dst_buffer = [&] -> const Buffer& {
        if (dst_gds) {
            return gds_buffer;
        }
        const auto buffer_id = FindBuffer(dst, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, dst, num_bytes, true, true);
        gpu_modified_ranges.Add(dst, num_bytes);
        return buffer;
    }();
    const vk::BufferCopy region = {
        .srcOffset = src_buffer.Offset(src),
        .dstOffset = dst_buffer.Offset(dst),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 buf_barriers_before[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_before,
    });
    cmdbuf.copyBuffer(src_buffer.Handle(), dst_buffer.Handle(), region);
    const vk::BufferMemoryBarrier2 buf_barriers_after[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_after,
    });
}

std::pair<Buffer*, u32> BufferCache::ObtainBuffer(VAddr device_addr, u32 size, bool is_written,
                                                  bool is_texel_buffer, BufferId buffer_id) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= CACHING_PAGESIZE && !IsRegionGpuModified(device_addr, size)) {
        const u64 offset = stream_buffer.Copy(device_addr, size, instance.UniformMinAlignment());
        return {&stream_buffer, offset};
    }
    if (IsBufferInvalid(buffer_id)) {
        buffer_id = FindBuffer(device_addr, size);
    }
    Buffer& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    TouchBuffer(buffer);
    return {&buffer, buffer.Offset(device_addr)};
}

std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(VAddr gpu_addr, u32 size) {
    // Check if any buffer contains the full requested range.
    const BufferId buffer_id = page_table[gpu_addr >> CACHING_PAGEBITS].buffer_id;
    if (buffer_id) {
        if (Buffer& buffer = slot_buffers[buffer_id]; buffer.IsInBounds(gpu_addr, size)) {
            SynchronizeBuffer(buffer, gpu_addr, size, false, false);
            TouchBuffer(buffer);
            return {&buffer, buffer.Offset(gpu_addr)};
        }
    }
    // If some buffer within was GPU modified create a full buffer to avoid losing GPU data.
    if (IsRegionGpuModified(gpu_addr, size)) {
        return ObtainBuffer(gpu_addr, size, false, false);
    }
    // In all other cases, just do a CPU copy to the staging buffer.
    const auto [data, offset] = staging_buffer.Map(size, 16);
    memory->CopySparseMemory(gpu_addr, data, size);
    staging_buffer.Commit();
    return {&staging_buffer, offset};
}

bool BufferCache::IsRegionRegistered(VAddr addr, size_t size) {
    // Check if we are missing some edge case here
    return buffer_ranges.Intersects(addr, size);
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

BufferId BufferCache::FindBuffer(VAddr device_addr, u32 size) {
    if (device_addr == 0) {
        return NULL_BUFFER_ID;
    }
    const u64 page = device_addr >> CACHING_PAGEBITS;
    const BufferId buffer_id = page_table[page].buffer_id;
    if (!buffer_id) {
        return CreateBuffer(device_addr, size);
    }
    const Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.IsInBounds(device_addr, size)) {
        TouchBuffer(buffer);
        return buffer_id;
    }
    return CreateBuffer(device_addr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(VAddr device_addr, u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    boost::container::small_vector<BufferId, 16> overlap_ids;
    VAddr begin = device_addr;
    VAddr end = device_addr + wanted_size;
    int stream_score = 0;
    bool has_stream_leap = false;
    const auto expand_begin = [&](VAddr add_value) {
        static constexpr VAddr min_page = CACHING_PAGESIZE + DEVICE_PAGESIZE;
        if (add_value > begin - min_page) {
            begin = min_page;
            device_addr = DEVICE_PAGESIZE;
            return;
        }
        begin -= add_value;
        device_addr = begin - CACHING_PAGESIZE;
    };
    const auto expand_end = [&](VAddr add_value) {
        static constexpr VAddr max_page = 1ULL << MemoryTracker::MAX_CPU_PAGE_BITS;
        if (add_value > max_page - end) {
            end = max_page;
            return;
        }
        end += add_value;
    };
    if (begin == 0) {
        return OverlapResult{
            .ids = std::move(overlap_ids),
            .begin = begin,
            .end = end,
            .has_stream_leap = has_stream_leap,
        };
    }
    for (; device_addr >> CACHING_PAGEBITS < Common::DivCeil(end, CACHING_PAGESIZE);
         device_addr += CACHING_PAGESIZE) {
        const BufferId overlap_id = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
        if (!overlap_id) {
            continue;
        }
        Buffer& overlap = slot_buffers[overlap_id];
        if (overlap.is_picked) {
            continue;
        }
        overlap_ids.push_back(overlap_id);
        overlap.is_picked = true;
        const VAddr overlap_device_addr = overlap.CpuAddr();
        const bool expands_left = overlap_device_addr < begin;
        if (expands_left) {
            begin = overlap_device_addr;
        }
        const VAddr overlap_end = overlap_device_addr + overlap.SizeBytes();
        const bool expands_right = overlap_end > end;
        if (overlap_end > end) {
            end = overlap_end;
        }
        stream_score += overlap.StreamScore();
        if (stream_score > STREAM_LEAP_THRESHOLD && !has_stream_leap) {
            // When this memory region has been joined a bunch of times, we assume it's being used
            // as a stream buffer. Increase the size to skip constantly recreating buffers.
            has_stream_leap = true;
            if (expands_right) {
                expand_begin(CACHING_PAGESIZE * 128);
            }
            if (expands_left) {
                expand_end(CACHING_PAGESIZE * 128);
            }
        }
    }
    return OverlapResult{
        .ids = std::move(overlap_ids),
        .begin = begin,
        .end = end,
        .has_stream_leap = has_stream_leap,
    };
}

void BufferCache::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
                              bool accumulate_stream_score) {
    Buffer& new_buffer = slot_buffers[new_buffer_id];
    Buffer& overlap = slot_buffers[overlap_id];
    if (accumulate_stream_score) {
        new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
    }
    const size_t dst_base_offset = overlap.CpuAddr() - new_buffer.CpuAddr();
    const vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = dst_base_offset,
        .size = overlap.SizeBytes(),
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> pre_barriers{};
    if (auto src_barrier = overlap.GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                              vk::PipelineStageFlagBits2::eTransfer)) {
        pre_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier =
            new_buffer.GetBarrier(vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eTransfer, dst_base_offset)) {
        pre_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
        .pBufferMemoryBarriers = pre_barriers.data(),
    });

    cmdbuf.copyBuffer(overlap.Handle(), new_buffer.Handle(), copy);

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> post_barriers{};
    if (auto src_barrier =
            overlap.GetBarrier(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                               vk::PipelineStageFlagBits2::eAllCommands)) {
        post_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier = new_buffer.GetBarrier(
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            vk::PipelineStageFlagBits2::eAllCommands, dst_base_offset)) {
        post_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(post_barriers.size()),
        .pBufferMemoryBarriers = post_barriers.data(),
    });
    DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(VAddr device_addr, u32 wanted_size) {
    const VAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    const OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    const BufferId new_buffer_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, overlap.begin,
                            AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, size);
    auto& new_buffer = slot_buffers[new_buffer_id];
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
    Register(new_buffer_id);
    return new_buffer_id;
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager.ProcessFaultBuffer();
}

void BufferCache::Register(BufferId buffer_id) {
    ChangeRegister<true>(buffer_id);
}

void BufferCache::Unregister(BufferId buffer_id) {
    ChangeRegister<false>(buffer_id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    const auto size = buffer.SizeBytes();
    const VAddr device_addr_begin = buffer.CpuAddr();
    const VAddr device_addr_end = device_addr_begin + size;
    const u64 page_begin = device_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(device_addr_end, CACHING_PAGESIZE);
    const u64 size_pages = page_end - page_begin;
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page].buffer_id = buffer_id;
        } else {
            page_table[page].buffer_id = BufferId{};
        }
    }
    if constexpr (insert) {
        total_used_memory += Common::AlignUp(size, CACHING_PAGESIZE);
        buffer.SetLRUId(lru_cache.Insert(buffer_id, gc_tick));
        gc_buffer_meta.erase(buffer.LRUId());
        boost::container::small_vector<vk::DeviceAddress, 128> bda_addrs;
        bda_addrs.reserve(size_pages);
        for (u64 i = 0; i < size_pages; ++i) {
            vk::DeviceAddress addr = buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS);
            bda_addrs.push_back(addr);
        }
        WriteDataBuffer(bda_pagetable_buffer, page_begin * sizeof(vk::DeviceAddress),
                        bda_addrs.data(), bda_addrs.size() * sizeof(vk::DeviceAddress));
        buffer_ranges.Add(buffer.CpuAddr(), buffer.SizeBytes(), buffer_id);
    } else {
        total_used_memory -= Common::AlignUp(size, CACHING_PAGESIZE);
        lru_cache.Free(buffer.LRUId());
        gc_buffer_meta.erase(buffer.LRUId());
        const u64 offset = bda_pagetable_buffer.Offset(page_begin * sizeof(vk::DeviceAddress));
        bda_pagetable_buffer.Fill(offset, size_pages * sizeof(vk::DeviceAddress), 0);
        buffer_ranges.Subtract(buffer.CpuAddr(), buffer.SizeBytes());
    }
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
                                    bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes = 0;
    VAddr buffer_start = buffer.CpuAddr();
    vk::Buffer src_buffer = VK_NULL_HANDLE;
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 device_addr_out, u64 range_size) {
            copies.emplace_back(total_size_bytes, device_addr_out - buffer_start, range_size);
            total_size_bytes += range_size;
        },
        [&] { src_buffer = UploadCopies(buffer, copies, total_size_bytes); });

    if (src_buffer) {
        scheduler.EndRendering();
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::BufferMemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eTransferRead |
                             vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        const vk::BufferMemoryBarrier2 post_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &pre_barrier,
        });
        cmdbuf.copyBuffer(src_buffer, buffer.buffer, copies);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &post_barrier,
        });
        TouchBuffer(buffer);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeBufferFromImage(buffer, device_addr, size);
    }
    return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     size_t total_size_bytes) {
    if (copies.empty()) {
        return VK_NULL_HANDLE;
    }
    const auto [staging, offset] = staging_buffer.Map(total_size_bytes);
    if (staging) {
        for (auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            memory->CopySparseMemory(device_addr, src_pointer, copy.size);
            // Apply the staging offset
            copy.srcOffset += offset;
        }
        staging_buffer.Commit();
        return staging_buffer.Handle();
    } else {
        // For large one time transfers use a temporary host buffer.
        auto temp_buffer =
            std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Upload, 0,
                                     vk::BufferUsageFlagBits::eTransferSrc, total_size_bytes);
        const vk::Buffer src_buffer = temp_buffer->Handle();
        u8* const staging = temp_buffer->mapped_data.data();
        for (const auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            memory->CopySparseMemory(device_addr, src_pointer, copy.size);
        }
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable { buffer.reset(); });
        return src_buffer;
    }
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size) {
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u32 buf_offset = buffer.Offset(image.info.guest_address);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    u32 copy_size = 0;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (buf_offset + mip_info.offset + mip_info.size > buffer.SizeBytes()) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
        copy_size += mip_info.size;
    }
    if (copy_size == 0) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, buffer.Handle(), buf_offset, copy_size);
    return true;
}

void BufferCache::SynchronizeBuffersInRange(VAddr device_addr, u64 size) {
    const VAddr device_addr_end = device_addr + size;
    ForEachBufferInRange(device_addr, size, [&](BufferId buffer_id, Buffer& buffer) {
        RENDERER_TRACE;
        VAddr start = std::max(buffer.CpuAddr(), device_addr);
        VAddr end = std::min(buffer.CpuAddr() + buffer.SizeBytes(), device_addr_end);
        u32 size = static_cast<u32>(end - start);
        SynchronizeBuffer(buffer, start, size, false, false);
    });
}

void BufferCache::WriteDataBuffer(Buffer& buffer, VAddr address, const void* value, u32 num_bytes) {
    vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = buffer.Offset(address),
        .size = num_bytes,
    };
    vk::Buffer src_buffer = staging_buffer.Handle();
    if (num_bytes < StagingBufferSize) {
        const auto [staging, offset] = staging_buffer.Map(num_bytes);
        std::memcpy(staging, value, num_bytes);
        copy.srcOffset = offset;
        staging_buffer.Commit();
    } else {
        // For large one time transfers use a temporary host buffer.
        // RenderDoc can lag quite a bit if the stream buffer is too large.
        Buffer temp_buffer{
            instance, scheduler, MemoryUsage::Upload, 0, vk::BufferUsageFlagBits::eTransferSrc,
            num_bytes};
        src_buffer = temp_buffer.Handle();
        u8* const staging = temp_buffer.mapped_data.data();
        std::memcpy(staging, value, num_bytes);
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable {});
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(src_buffer, buffer.Handle(), copy);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

void BufferCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }

    const bool metrics_enabled = IsBufferGcMetricsEnabled();
    const auto gc_pass_start = std::chrono::steady_clock::now();
    u64 metrics_state = static_cast<u64>(gc_pressure_state);
    u64 metrics_fast_queue = gc_fast_queue.size();
    u64 metrics_slow_queue = gc_slow_queue.size();
    u64 metrics_selected = 0;
    u64 metrics_deleted = 0;
    u64 metrics_skipped = 0;
    u64 metrics_readback_bytes = 0;

    SCOPE_EXIT {
        if (!metrics_enabled) {
            return;
        }
        using namespace std::chrono;
        const auto now = steady_clock::now();
        const u64 pass_duration_us =
            static_cast<u64>(duration_cast<microseconds>(now - gc_pass_start).count());
        Common::GcMetrics::PublishPass(gc_tick, metrics_state, total_used_memory,
                                       metrics_fast_queue, metrics_slow_queue, metrics_selected,
                                       metrics_deleted, metrics_skipped, metrics_readback_bytes,
                                       pass_duration_us);

        static u64 window_start_ms = 0;
        static u64 window_passes = 0;
        static u64 window_bytes = 0;
        static u64 window_duration_us = 0;
        static u64 window_max_us = 0;

        const u64 now_ms =
            static_cast<u64>(duration_cast<milliseconds>(now.time_since_epoch()).count());
        if (window_start_ms == 0) {
            window_start_ms = now_ms;
        }

        ++window_passes;
        window_bytes += metrics_readback_bytes;
        window_duration_us += pass_duration_us;
        window_max_us = std::max(window_max_us, pass_duration_us);

        const u64 window_ms = now_ms - window_start_ms;
        if (window_ms >= 1000) {
            const u64 safe_window_ms = std::max<u64>(window_ms, 1);
            const u64 passes_per_sec = (window_passes * 1000ULL) / safe_window_ms;
            const u64 readback_mb_per_sec =
                ((window_bytes * 1000ULL) / safe_window_ms) / (1024ULL * 1024ULL);
            const u64 avg_pass_us = window_passes ? (window_duration_us / window_passes) : 0;

            Common::GcMetrics::PublishRolling(passes_per_sec, readback_mb_per_sec, avg_pass_us,
                                              window_max_us);

            LOG_INFO(Render_Vulkan,
                     "Buffer GC rolling: window_ms={} passes={} passes_s={} readback_mb_s={} "
                     "avg_pass_us={} max_pass_us={} state={} used_mb={} fast_q={} slow_q={}",
                     safe_window_ms, window_passes, passes_per_sec, readback_mb_per_sec,
                     avg_pass_us, window_max_us, metrics_state,
                     total_used_memory / (1024ULL * 1024ULL), metrics_fast_queue,
                     metrics_slow_queue);

            window_start_ms = now_ms;
            window_passes = 0;
            window_bytes = 0;
            window_duration_us = 0;
            window_max_us = 0;
        }
    };

    const bool verbose_gc_logging = IsBufferGcVerboseLoggingEnabled();
    const bool trace_gc_logging = IsBufferGcTraceLoggingEnabled();
    const bool gc_disabled = IsBufferGcDisabled();
    const bool gc_dry_run = IsBufferGcDryRunEnabled();
    const bool gc_readback_only_requested = IsBufferGcReadbackOnlyEnabled();
    const bool gc_delete_only_requested = IsBufferGcDeleteOnlyEnabled();
    const bool gc_sync_readback = IsBufferGcSyncReadbackEnabled();

    if (gc_readback_only_requested && gc_delete_only_requested) {
        LOG_WARNING(Render_Vulkan, "Both SHADPS4_VK_BUFFER_GC_READBACK_ONLY and "
                                   "SHADPS4_VK_BUFFER_GC_DELETE_ONLY are enabled; "
                                   "readback-only will take precedence.");
    }

    const bool gc_readback_only = gc_readback_only_requested;
    const bool gc_delete_only = gc_delete_only_requested && !gc_readback_only_requested;

    const u64 pressure_exit_memory = (trigger_gc_memory * 9ULL) / 10ULL;
    const u64 critical_exit_memory = (critical_gc_memory * 95ULL) / 100ULL;
    const GcPressureState previous_state = gc_pressure_state;
    switch (gc_pressure_state) {
    case GcPressureState::Idle:
        if (total_used_memory >= critical_gc_memory) {
            gc_pressure_state = GcPressureState::Critical;
        } else if (total_used_memory >= trigger_gc_memory) {
            gc_pressure_state = GcPressureState::Pressure;
        }
        break;
    case GcPressureState::Pressure:
        if (total_used_memory >= critical_gc_memory) {
            gc_pressure_state = GcPressureState::Critical;
        } else if (total_used_memory < pressure_exit_memory) {
            gc_pressure_state = GcPressureState::Idle;
        }
        break;
    case GcPressureState::Critical:
        if (total_used_memory < critical_exit_memory) {
            gc_pressure_state = (total_used_memory >= trigger_gc_memory) ? GcPressureState::Pressure
                                                                         : GcPressureState::Idle;
        }
        break;
    }

    if (trace_gc_logging && gc_pressure_state != previous_state) {
        LOG_INFO(Render_Vulkan,
                 "Buffer GC pressure state transition: tick={} used={} trigger={} critical={} "
                 "from={} to={}",
                 gc_tick, total_used_memory, trigger_gc_memory, critical_gc_memory,
                 static_cast<int>(previous_state), static_cast<int>(gc_pressure_state));
    }
    metrics_state = static_cast<u64>(gc_pressure_state);

    if (gc_disabled) {
        if (!gc_fast_queue.empty() || !gc_slow_queue.empty()) {
            gc_fast_queue.clear();
            gc_slow_queue.clear();
            for (auto& [_, meta] : gc_buffer_meta) {
                meta.queued = false;
            }
        }
        metrics_fast_queue = gc_fast_queue.size();
        metrics_slow_queue = gc_slow_queue.size();
        if (trace_gc_logging) {
            LOG_INFO(Render_Vulkan,
                     "Buffer GC pass skipped (disabled): tick={} used={} trigger={} critical={}",
                     gc_tick, total_used_memory, trigger_gc_memory, critical_gc_memory);
        }
        return;
    }

    if (gc_pressure_state == GcPressureState::Idle) {
        if (!gc_fast_queue.empty() || !gc_slow_queue.empty()) {
            gc_fast_queue.clear();
            gc_slow_queue.clear();
            for (auto& [_, meta] : gc_buffer_meta) {
                meta.queued = false;
            }
        }
        metrics_fast_queue = gc_fast_queue.size();
        metrics_slow_queue = gc_slow_queue.size();
        return;
    }

    const bool aggressive = gc_pressure_state == GcPressureState::Critical;
    const u64 over_trigger_bytes =
        total_used_memory > trigger_gc_memory ? (total_used_memory - trigger_gc_memory) : 0;
    const u64 over_critical_bytes =
        total_used_memory > critical_gc_memory ? (total_used_memory - critical_gc_memory) : 0;

    const u64 base_gc_interval_ticks = GetBufferGcIntervalTicks();
    u64 gc_interval_ticks = base_gc_interval_ticks;
    if (aggressive && over_critical_bytes > 0) {
        constexpr u64 interval_step_bytes = 256ULL * 1024ULL * 1024ULL;
        const u64 divisor = 1ULL + std::min<u64>(over_critical_bytes / interval_step_bytes, 7ULL);
        gc_interval_ticks = std::max<u64>(1, base_gc_interval_ticks / divisor);
    }

    const u64 base_gc_select_interval_ticks = GetBufferGcSelectIntervalTicks();
    u64 gc_select_interval_ticks = base_gc_select_interval_ticks;
    if (aggressive) {
        gc_select_interval_ticks = std::max<u64>(1, base_gc_select_interval_ticks / 2ULL);
    }

    const u64 min_age_ticks = std::min<u64>(GetBufferGcAgeThreshold(aggressive), gc_tick);
    const u64 fast_class_bytes = GetBufferGcFastClassBytes();
    const u64 queue_limit = GetBufferGcQueueLimit();
    const u64 queue_target = GetBufferGcQueueTarget(queue_limit);

    const auto total_queue_size = [&]() { return gc_fast_queue.size() + gc_slow_queue.size(); };

    const bool queue_needs_refill = total_queue_size() < 8;
    const bool queue_below_target = total_queue_size() < queue_target;
    const bool should_select =
        queue_below_target && ((gc_tick - gc_last_select_tick) >= gc_select_interval_ticks ||
                               (queue_needs_refill && (gc_tick - gc_last_select_tick) >= 4));
    int queue_selected = 0;
    int queue_protected_skipped = 0;
    int queue_retire_waiting = 0;
    if (should_select && total_queue_size() < queue_target) {
        int selection_quota = GetBufferGcSelectionQuota(aggressive);
        const u64 tick_threshold = gc_tick - min_age_ticks;
        lru_cache.ForEachItemBelow(tick_threshold, [&](BufferId buffer_id) {
            if (selection_quota <= 0 || total_queue_size() >= queue_target) {
                return true;
            }
            if (IsBufferInvalid(buffer_id)) {
                return false;
            }

            Buffer& buffer = slot_buffers[buffer_id];
            auto& meta = gc_buffer_meta[buffer.LRUId()];
            if (meta.queued) {
                return false;
            }
            if (meta.retired) {
                if (meta.retire_ready_tick > gc_tick) {
                    ++queue_retire_waiting;
                    return false;
                }

                auto& target_queue =
                    (buffer.SizeBytes() <= fast_class_bytes) ? gc_fast_queue : gc_slow_queue;
                target_queue.push_front(buffer_id);
                meta.queued = true;
                ++queue_selected;
                --selection_quota;
                return false;
            }
            if (meta.protected_until_tick > gc_tick) {
                ++queue_protected_skipped;
                return false;
            }

            auto& target_queue =
                (buffer.SizeBytes() <= fast_class_bytes) ? gc_fast_queue : gc_slow_queue;
            target_queue.push_back(buffer_id);
            meta.queued = true;
            ++queue_selected;
            --selection_quota;
            return false;
        });
        gc_last_select_tick = gc_tick;
    }

    if ((gc_tick % gc_interval_ticks) != 0) {
        metrics_fast_queue = gc_fast_queue.size();
        metrics_slow_queue = gc_slow_queue.size();
        if (trace_gc_logging && (queue_selected > 0 || queue_protected_skipped > 0)) {
            LOG_INFO(Render_Vulkan,
                     "Buffer GC select-only tick={} selected={} protected_skipped={} "
                     "retire_waiting={} fast_queue={} slow_queue={} interval_ticks={} "
                     "select_interval_ticks={} state={}",
                     gc_tick, queue_selected, queue_protected_skipped, queue_retire_waiting,
                     gc_fast_queue.size(), gc_slow_queue.size(), gc_interval_ticks,
                     gc_select_interval_ticks, static_cast<int>(gc_pressure_state));
        }
        return;
    }

    const int base_max_deletions_allowed = GetBufferGcMaxDeletions(aggressive);
    int max_deletions_allowed = base_max_deletions_allowed;
    if (aggressive && over_critical_bytes > 0) {
        constexpr u64 deletion_step_bytes = 256ULL * 1024ULL * 1024ULL;
        const int adaptive_extra =
            static_cast<int>(std::min<u64>(over_critical_bytes / deletion_step_bytes, 32ULL));
        max_deletions_allowed = std::min(base_max_deletions_allowed + adaptive_extra, 64);
    }

    const u64 base_readback_budget_bytes = GetBufferGcReadbackBudgetBytes(aggressive);
    u64 readback_budget_bytes = base_readback_budget_bytes;
    if (aggressive && over_critical_bytes > 0) {
        constexpr u64 max_adaptive_extra = 512ULL * 1024ULL * 1024ULL;
        const u64 adaptive_extra = std::min<u64>(over_critical_bytes / 2ULL, max_adaptive_extra);
        constexpr u64 readback_budget_hard_cap = 1024ULL * 1024ULL * 1024ULL;
        readback_budget_bytes =
            std::min<u64>(base_readback_budget_bytes + adaptive_extra, readback_budget_hard_cap);
    }

    constexpr u64 gc_staging_cap_bytes = DownloadBufferSize;
    const u64 effective_readback_budget_bytes =
        std::min<u64>(readback_budget_bytes, gc_staging_cap_bytes);

    u64 reclaim_target_bytes = 0;
    if (over_trigger_bytes > 0) {
        constexpr u64 pressure_min_target = 16ULL * 1024ULL * 1024ULL;
        constexpr u64 pressure_max_target = 128ULL * 1024ULL * 1024ULL;
        constexpr u64 critical_min_target = 64ULL * 1024ULL * 1024ULL;
        constexpr u64 critical_max_target = 512ULL * 1024ULL * 1024ULL;
        if (aggressive) {
            reclaim_target_bytes = std::clamp<u64>(over_trigger_bytes / 2ULL, critical_min_target,
                                                   critical_max_target);
        } else {
            reclaim_target_bytes = std::clamp<u64>(over_trigger_bytes / 4ULL, pressure_min_target,
                                                   pressure_max_target);
        }
    }

    const u64 ticks_to_destroy = min_age_ticks;

    int remaining_deletions = max_deletions_allowed;
    int selected_candidates = 0;
    int readback_successes = 0;
    int readback_skipped = 0;
    int deleted_buffers = 0;
    int delete_skipped = 0;
    int skipped_buffers = 0;
    int budget_limited = 0;
    int oversize_skipped = 0;
    int protected_skipped = 0;
    int recent_skipped = 0;
    int invalid_skipped = 0;
    int staging_limited = 0;
    int retired_buffers = 0;
    int retired_delete_deferred = 0;
    int fence_delete_deferred = 0;
    int critical_delete_deferred = 0;
    int large_delete_deferred = 0;
    u64 readback_bytes_scheduled = 0;
    u64 reclaimed_bytes = 0;

    const u64 retire_medium_threshold_bytes =
        GetBufferGcRetireThresholdBytes("SHADPS4_VK_BUFFER_GC_RETIRE_MEDIUM_MB", 1);
    const u64 retire_large_threshold_bytes =
        GetBufferGcRetireThresholdBytes("SHADPS4_VK_BUFFER_GC_RETIRE_LARGE_MB", 8);
    const u64 retire_small_ticks =
        GetBufferGcRetireTicks(retire_medium_threshold_bytes - 1, aggressive);
    const u64 retire_medium_ticks =
        GetBufferGcRetireTicks(retire_medium_threshold_bytes, aggressive);
    const u64 retire_large_ticks = GetBufferGcRetireTicks(retire_large_threshold_bytes, aggressive);
    const int retired_delete_limit = GetBufferGcMaxRetiredDeletes(aggressive);
    int remaining_retired_deletes = retired_delete_limit;
    const u64 critical_delete_overshoot_bytes = GetBufferGcCriticalDeleteOvershootBytes();

    const auto process_candidate = [&](BufferId buffer_id) {
        if (!buffer_id || IsBufferInvalid(buffer_id)) {
            ++invalid_skipped;
            return;
        }

        Buffer& buffer = slot_buffers[buffer_id];
        const u64 candidate_size_bytes = buffer.SizeBytes();
        const u64 candidate_retire_ticks = GetBufferGcRetireTicks(candidate_size_bytes, aggressive);
        auto& meta = gc_buffer_meta[buffer.LRUId()];
        const u64 destroy_submit_tick =
            std::max(meta.last_use_submit_tick, meta.retire_submit_tick);
        meta.queued = false;
        const bool retired_candidate = meta.retired && meta.retire_ready_tick <= gc_tick;
        if (!retired_candidate && meta.protected_until_tick > gc_tick) {
            ++protected_skipped;
            return;
        }

        if (!retired_candidate) {
            const s64 age_delta =
                static_cast<s64>(gc_tick) - static_cast<s64>(lru_cache.GetTick(buffer.LRUId()));
            if (age_delta < static_cast<s64>(min_age_ticks)) {
                ++recent_skipped;
                return;
            }
        }

        if (!retired_candidate && !gc_delete_only && !gc_dry_run) {
            if (candidate_size_bytes > effective_readback_budget_bytes) {
                ++oversize_skipped;
                ++staging_limited;
                return;
            }
            const u64 next_readback_bytes = readback_bytes_scheduled + candidate_size_bytes;
            if (readback_bytes_scheduled >= effective_readback_budget_bytes ||
                (readback_bytes_scheduled > 0 &&
                 next_readback_bytes > effective_readback_budget_bytes)) {
                ++budget_limited;
                return;
            }
        }

        ++selected_candidates;
        if (verbose_gc_logging) {
            LOG_INFO(Render_Vulkan,
                     "Buffer GC candidate: id={} addr={:#x} size={} bytes lru_id={} tick={} "
                     "state={} retired={} retire_ticks={} destroy_submit_tick={} dry_run={} "
                     "readback_only={} delete_only={} sync_readback={}",
                     buffer_id.index, buffer.CpuAddr(), buffer.SizeBytes(), buffer.LRUId(), gc_tick,
                     static_cast<int>(gc_pressure_state), retired_candidate, candidate_retire_ticks,
                     destroy_submit_tick, gc_dry_run, gc_readback_only, gc_delete_only,
                     gc_sync_readback);
        }

        --remaining_deletions;
        if (gc_dry_run) {
            return;
        }

        if (!retired_candidate && !gc_delete_only) {
            const bool readback_ok =
                gc_sync_readback ? DownloadBufferMemory<false>(buffer, buffer.CpuAddr(),
                                                               candidate_size_bytes, true, false)
                                 : DownloadBufferMemory<true>(buffer, buffer.CpuAddr(),
                                                              candidate_size_bytes, true, false);
            if (!readback_ok) {
                ++skipped_buffers;
                LOG_WARNING(Render_Vulkan,
                            "Buffer GC skipped eviction due to failed readback: id={} addr={:#x} "
                            "size={} bytes lru_id={} tick={} used={} state={}",
                            buffer_id.index, buffer.CpuAddr(), candidate_size_bytes, buffer.LRUId(),
                            gc_tick, total_used_memory, static_cast<int>(gc_pressure_state));
                return;
            }
            ++readback_successes;
            readback_bytes_scheduled += candidate_size_bytes;
        } else {
            ++readback_skipped;
        }

        if (gc_readback_only) {
            ++delete_skipped;
            return;
        }

        if (!retired_candidate && !gc_delete_only) {
            meta.retired = true;
            meta.retire_ready_tick = gc_tick + candidate_retire_ticks;
            meta.retire_submit_tick = scheduler.CurrentTick();
            meta.touch_heat = 0;
            ++retired_buffers;
            return;
        }

        if (retired_candidate && destroy_submit_tick != 0 &&
            !scheduler.IsFree(destroy_submit_tick)) {
            ++delete_skipped;
            ++fence_delete_deferred;
            ++remaining_deletions;
            return;
        }

        if (retired_candidate && remaining_retired_deletes <= 0) {
            ++delete_skipped;
            ++retired_delete_deferred;
            ++remaining_deletions;
            return;
        }

        const bool in_critical_delete_phase = gc_pressure_state == GcPressureState::Critical;
        if (retired_candidate && in_critical_delete_phase &&
            over_critical_bytes < critical_delete_overshoot_bytes) {
            ++delete_skipped;
            ++critical_delete_deferred;
            ++remaining_deletions;
            return;
        }

        const bool large_candidate = candidate_size_bytes >= retire_large_threshold_bytes;
        const bool allow_large_retired_delete =
            !large_candidate || over_critical_bytes >= candidate_size_bytes;
        if (retired_candidate && large_candidate && !allow_large_retired_delete) {
            ++delete_skipped;
            ++large_delete_deferred;
            ++remaining_deletions;
            return;
        }

        if (retired_candidate) {
            --remaining_retired_deletes;
        }

        meta.retired = false;
        meta.retire_ready_tick = 0;
        meta.retire_submit_tick = 0;
        ++deleted_buffers;
        reclaimed_bytes += candidate_size_bytes;
        DeleteBuffer(buffer_id);
    };

    const auto queues_not_empty = [&] { return !gc_fast_queue.empty() || !gc_slow_queue.empty(); };

    while (queues_not_empty()) {
        if (remaining_deletions <= 0) {
            if (!aggressive || reclaimed_bytes >= reclaim_target_bytes) {
                break;
            }
        }

        BufferId next{};
        if (aggressive) {
            if (!gc_slow_queue.empty()) {
                next = gc_slow_queue.front();
                gc_slow_queue.pop_front();
            } else if (!gc_fast_queue.empty()) {
                next = gc_fast_queue.front();
                gc_fast_queue.pop_front();
            }
        } else {
            if (!gc_fast_queue.empty()) {
                next = gc_fast_queue.front();
                gc_fast_queue.pop_front();
            } else if (!gc_slow_queue.empty()) {
                next = gc_slow_queue.front();
                gc_slow_queue.pop_front();
            }
        }

        if (!next) {
            break;
        }

        process_candidate(next);
    }

    if (verbose_gc_logging || trace_gc_logging || selected_candidates > 0 || skipped_buffers > 0 ||
        budget_limited > 0 || oversize_skipped > 0 || queue_selected > 0 ||
        queue_protected_skipped > 0 || deleted_buffers > 0 || gc_dry_run || gc_readback_only ||
        gc_delete_only || gc_sync_readback) {
        LOG_INFO(
            Render_Vulkan,
            "Buffer GC pass: tick={} state={} aggressive={} used={} trigger={} critical={} "
            "over_trigger={} over_critical={} max_deletions={} base_max_deletions={} "
            "selected={} readback_ok={} readback_skipped={} retired={} deleted={} "
            "delete_skipped={} retired_delete_deferred={} fence_delete_deferred={} "
            "critical_delete_deferred={} large_delete_deferred={} reclaimed_bytes={} "
            "reclaim_target={} skipped={} "
            "budget_limited={} oversize_skipped={} "
            "staging_limited={} protected_skipped={} "
            "recent_skipped={} invalid_skipped={} queue_selected={} "
            "queue_protected_skipped={} readback_bytes={} effective_readback_budget={} "
            "readback_budget={} base_readback_budget={} retired_delete_limit={} "
            "remaining_retired_deletes={} critical_delete_overshoot_mb={} "
            "retire_small_ticks={} retire_medium_ticks={} retire_large_ticks={} "
            "retire_medium_mb={} "
            "retire_large_mb={} ticks_to_destroy={} interval_ticks={} "
            "base_interval_ticks={} select_interval_ticks={} "
            "base_select_interval_ticks={} queue_target={} fast_queue={} slow_queue={} "
            "dry_run={} readback_only={} delete_only={} sync_readback={} disabled={}",
            gc_tick, static_cast<int>(gc_pressure_state), aggressive, total_used_memory,
            trigger_gc_memory, critical_gc_memory, over_trigger_bytes, over_critical_bytes,
            max_deletions_allowed, base_max_deletions_allowed, selected_candidates,
            readback_successes, readback_skipped, retired_buffers, deleted_buffers, delete_skipped,
            retired_delete_deferred, fence_delete_deferred, critical_delete_deferred,
            large_delete_deferred, reclaimed_bytes, reclaim_target_bytes, skipped_buffers,
            budget_limited, oversize_skipped, staging_limited, protected_skipped, recent_skipped,
            invalid_skipped, queue_selected, queue_protected_skipped, readback_bytes_scheduled,
            effective_readback_budget_bytes, readback_budget_bytes, base_readback_budget_bytes,
            retired_delete_limit, remaining_retired_deletes,
            critical_delete_overshoot_bytes / (1024ULL * 1024ULL), retire_small_ticks,
            retire_medium_ticks, retire_large_ticks,
            retire_medium_threshold_bytes / (1024ULL * 1024ULL),
            retire_large_threshold_bytes / (1024ULL * 1024ULL), ticks_to_destroy, gc_interval_ticks,
            base_gc_interval_ticks, gc_select_interval_ticks, base_gc_select_interval_ticks,
            queue_target, gc_fast_queue.size(), gc_slow_queue.size(), gc_dry_run, gc_readback_only,
            gc_delete_only, gc_sync_readback, gc_disabled);
    }

    metrics_fast_queue = gc_fast_queue.size();
    metrics_slow_queue = gc_slow_queue.size();
    metrics_selected = selected_candidates;
    metrics_deleted = deleted_buffers;
    metrics_skipped = skipped_buffers + budget_limited + oversize_skipped + protected_skipped +
                      recent_skipped + invalid_skipped;
    metrics_readback_bytes = readback_bytes_scheduled;

    for (auto it = gc_buffer_meta.begin(); it != gc_buffer_meta.end();) {
        if (!it->second.queued && !it->second.retired && it->second.touch_heat == 0 &&
            it->second.protected_until_tick < gc_tick) {
            it = gc_buffer_meta.erase(it);
            continue;
        }
        ++it;
    }
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
    lru_cache.Touch(buffer.LRUId(), gc_tick);

    auto& meta = gc_buffer_meta[buffer.LRUId()];
    meta.last_use_submit_tick = scheduler.CurrentTick();
    if (meta.retired) {
        meta.retired = false;
        meta.retire_ready_tick = 0;
        meta.retire_submit_tick = 0;
    }
    if (meta.touch_heat < std::numeric_limits<u8>::max()) {
        ++meta.touch_heat;
    }

    const u64 protect_threshold = GetBufferGcProtectThreshold();
    if (meta.touch_heat >= protect_threshold) {
        const u64 protect_until = gc_tick + GetBufferGcProtectTicks();
        meta.protected_until_tick = std::max(meta.protected_until_tick, protect_until);
        meta.touch_heat = 0;
    }
}

void BufferCache::DeleteBuffer(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    if (auto it = gc_buffer_meta.find(buffer.LRUId()); it != gc_buffer_meta.end()) {
        it->second.queued = false;
        it->second.retired = false;
        it->second.touch_heat = 0;
        it->second.protected_until_tick = 0;
        it->second.retire_ready_tick = 0;
        it->second.last_use_submit_tick = 0;
        it->second.retire_submit_tick = 0;
    }
    Unregister(buffer_id);
    scheduler.DeferOperation([this, buffer_id] { slot_buffers.erase(buffer_id); });
    buffer.is_deleted = true;
}

} // namespace VideoCore
