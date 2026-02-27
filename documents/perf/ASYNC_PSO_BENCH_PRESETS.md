# Async PSO Staged Rollout - Benchmark Presets

This document defines operator presets for measuring frametime improvements while keeping risk low.

## Preset A: Baseline (sync only)
```bash
unset SHADPS4_VK_PSO_ASYNC
unset SHADPS4_VK_PSO_WORKERS
unset SHADPS4_VK_PSO_BUDGET_US
```

## Preset B: Staged async telemetry (safe)
```bash
export SHADPS4_VK_PSO_ASYNC=1
export SHADPS4_VK_PSO_WORKERS=1
export SHADPS4_VK_PSO_BUDGET_US=2000
```

## Preset C: Aggressive staging
```bash
export SHADPS4_VK_PSO_ASYNC=1
export SHADPS4_VK_PSO_WORKERS=2
export SHADPS4_VK_PSO_BUDGET_US=3500
```

## Test Route Protocol (Bloodborne)
1. Cold run: first launch after cache clear.
2. Warm run: second run same route.
3. Same camera path + duration for all presets.

Capture:
- median frametime
- 1% low / 0.1% low
- count of >33ms spikes
- staged async stats line from log

## Rollback
Disable all vars (Preset A).
