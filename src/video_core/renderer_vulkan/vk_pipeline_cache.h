// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <variant>
#include <memory>
#include <mutex>
#include <vector>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_compile_queue.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};

    Program() = default;
    Program(Shader::Stage stage, Shader::LogicalStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

class PipelineCache {
public:
    enum class PipelineBuildState : u8 {
        Missing,
        Queued,
        Compiling,
        Ready,
        Failed,
    };

    struct DeferredCompilePayload {
        u64 key_hash{};
        bool is_compute{};
        std::optional<GraphicsPipelineKey> graphics_key{};
        std::optional<ComputePipelineKey> compute_key{};
        u64 enqueued_ts_us{};
    };

    struct PrewarmEntry {
        u64 key_hash{};
        bool is_compute{};
        u32 priority{};
    };

    struct PerfCounters {
        u64 graphics_cache_misses{};
        u64 compute_cache_misses{};
        u64 graphics_compile_count{};
        u64 compute_compile_count{};
        u64 graphics_compile_time_us{};
        u64 compute_compile_time_us{};
        u64 graphics_async_queue_hits{};
        u64 compute_async_queue_hits{};
        u64 graphics_sync_fallbacks{};
        u64 compute_sync_fallbacks{};
        u64 async_queue_depth_peak{};
        u64 async_queue_tasks_completed{};
        u64 async_queue_budget_warnings{};
        u64 async_queue_enqueue_skips{};
        u64 async_throttle_hits{};
        u64 deferred_handler_calls{};
        u64 deferred_handler_budget_exceeded{};
        u64 deferred_handler_failures{};
    };

public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline();

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(Shader::Stage stage, Shader::LogicalStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::Stage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

    [[nodiscard]] const PerfCounters& GetPerfCounters() const {
        return perf_counters;
    }

    void LogStagedAsyncSnapshot(std::string_view reason) const;

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    void SetGraphicsBuildState(const GraphicsPipelineKey& key, PipelineBuildState state);
    void SetComputeBuildState(const ComputePipelineKey& key, PipelineBuildState state);
    [[nodiscard]] PipelineBuildState GetGraphicsBuildState(const GraphicsPipelineKey& key) const;
    [[nodiscard]] PipelineBuildState GetComputeBuildState(const ComputePipelineKey& key) const;
    [[nodiscard]] bool ShouldThrottleSyncFallback(u32 queue_depth) const;
    void HandleDeferredCompilePayload(const DeferredCompilePayload& payload, u32 budget_us);
    void LoadPrewarmManifest();
    void SchedulePrewarmEntries();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::Stage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::Stage stage, Shader::LogicalStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start
    PerfCounters perf_counters{};
    bool async_pso_requested{};
    bool async_pso_nonblock{};
    u32 async_pso_workers{1};
    u32 async_pso_soft_budget_us{2000};
    std::unique_ptr<PipelineCompileQueue> compile_queue;
    std::vector<PrewarmEntry> prewarm_entries;
    bool prewarm_enabled{};
    mutable std::mutex build_state_mutex;
    tsl::robin_map<GraphicsPipelineKey, PipelineBuildState> graphics_build_states;
    tsl::robin_map<ComputePipelineKey, PipelineBuildState> compute_build_states;

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
