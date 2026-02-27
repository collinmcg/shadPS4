# Async PSO Compile + Prewarm Plan (Bloodborne frametime stability)

## Goal
Reduce traversal/frame-time spikes caused by first-use Vulkan graphics/compute pipeline compilation.

## Evidence in current code
- `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`
  - `GetGraphicsPipeline()` compiles synchronously on cache miss.
  - `GetComputePipeline()` compiles synchronously on cache miss.
  - New pipeline creation increments `num_new_pipelines` on demand during rendering.

This behavior is correct functionally but can cause hitching when a scene hits unseen PSO keys.

## Proposed architecture

### 1) Async compile queue
- Introduce `PipelineCompileQueue` (bounded MPMC queue + worker threads).
- On cache miss, enqueue key for background compile.
- Render thread either:
  - uses already-compiled variant, or
  - marks key pending and retries next frame (short fallback path).

### 2) Pending/ready states in pipeline cache
- Track per key state: `Missing -> Queued -> Compiling -> Ready -> Failed`.
- Prevent duplicate compiles for same key with atomic state transitions.

### 3) Persistent prewarm set
- Persist frequently hit keys (per title/profile) to a local prewarm manifest.
- On boot/title launch, queue prewarm compiles at low priority.

### 4) Frame-budget aware scheduler
- If frame budget pressure is high, throttle low-priority compiles.
- Prioritize PSOs observed in last N frames over speculative prewarm items.

## Minimal first PR scope (safe)
1. Add compile queue + key state tracking.
2. Move compile work for *new* keys to worker thread.
3. Keep existing synchronous path behind a runtime toggle for fallback.
4. Add instrumentation counters:
   - compile queue depth
   - compile time histogram
   - frame misses due to pending PSO

## Rollback plan
- Runtime flag: `--vk-pso-async=false` (default false in first PR).
- Build-time guard to fully disable async path.
- If regressions appear, force synchronous behavior with no data migration needed.

## Benchmark protocol (required before claiming improvement)
- Capture before/after on same scene route and shader cache state.
- Metrics:
  - frame-time median
  - 1% low / 0.1% low
  - count of >33ms spikes
  - compile queue backlog over time
- Report both cold-run and warm-run.

## Expected outcome
- Significant reduction in frametime spikes/hitches.
- Better 1% lows; smoother camera traversal.
- Average FPS may stay similar; frametime consistency should improve.
