# PR5 Review Packet - Queue Metrics Lock Contention Trim

## Scope
- Focus area: Vulkan PSO/frame-time stability telemetry path
- Change type: low-risk, behavior-preserving micro-optimization

## What changed
1) `PipelineCompileQueue::QueueDepth()` is now lock-free
- Replaced mutex + `tasks_.size()` read with an atomic depth counter (`queue_depth_`)

2) Queue depth accounting moved to enqueue/dequeue critical sections
- Update `queue_depth_` when tasks are pushed/popped
- Reset depth to `0` in `Stop()` after worker teardown

## Why this matters
- New PSO misses call `QueueDepth()` from the render path for throttle/telemetry.
- Before this patch, those reads took the queue mutex and could contend with worker enqueue/dequeue traffic.
- This patch removes that lock acquisition from the render thread while preserving existing throttle semantics.

## Risk
- Low.
- No change to compile decisions, queue ordering, or task execution.
- Affects only depth reporting path used by telemetry/throttle checks.

## Rollback
- Revert this PR commit.
- Or temporarily disable staged async path with `SHADPS4_VK_PSO_ASYNC` unset.

## Benchmark plan
1) Build with PR5 patch.
2) Run representative shader-stutter scene(s) with:
   - `SHADPS4_VK_PSO_ASYNC=1`
   - `SHADPS4_VK_PSO_WORKERS=2` and `=4`
3) Capture:
   - 1% low frametime and p95/p99 frametime
   - hitch count > 16.7 ms and > 33 ms
   - async counters from periodic snapshot logs (`q_peak`, `q_done`, `q_throttle`, `q_budget_warn`)
4) Compare against PR4 baseline on same game/build/driver.

Expected outcome: neutral or slightly improved frametime consistency during shader compilation bursts due to reduced render-thread lock contention.
