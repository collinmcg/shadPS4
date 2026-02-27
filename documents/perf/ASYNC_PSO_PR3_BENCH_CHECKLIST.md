# PR3 Benchmark Checklist (Execution Wiring)

## Goal
Validate that deferred payload execution wiring is safe and observable before true background compile behavior is enabled.

## Presets
- Baseline: no `SHADPS4_VK_PSO_*` vars
- Staged async: `SHADPS4_VK_PSO_ASYNC=1`
- Staged non-blocking: `SHADPS4_VK_PSO_ASYNC=1 SHADPS4_VK_PSO_NONBLOCK=1`

## Capture
1) Frametime metrics
- median
- 1% low / 0.1% low
- >33ms spike count

2) Async snapshot fields
- q_peak, q_done, q_skip, q_budget_warn, q_throttle
- d_calls, d_budget, d_fail

3) Recovery behavior checks
- In NONBLOCK mode, key reaching Failed state should recover via:
  Failed -> Missing -> sync rebuild -> Ready

## Pass conditions
- No crashes/regressions in baseline mode
- Staged modes produce telemetry consistently
- Recovery path works deterministically after failed deferred state
