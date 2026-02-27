# Async PSO Staged Rollout - Review Packet

## Scope implemented in PR #1
This PR intentionally avoids risky behavior flips. It introduces staged infrastructure and observability:

- async rollout env switches
- pipeline build-state tracking
- compile queue scaffold
- queue lifecycle management
- staged non-blocking checks (flag-gated)
- backpressure guards
- periodic/snapshot telemetry for benchmark capture

## Flags
- `SHADPS4_VK_PSO_ASYNC=1`
- `SHADPS4_VK_PSO_WORKERS=<n>`
- `SHADPS4_VK_PSO_BUDGET_US=<us>`
- `SHADPS4_VK_PSO_NONBLOCK=1` (experimental)

## Risk level
- **Low/Medium** in default mode (all flags unset)
- **Medium** in staged async telemetry mode
- **Higher** in NONBLOCK exploratory mode

## Rollback
- Unset all `SHADPS4_VK_PSO_*` vars (returns to baseline sync behavior)
- If needed, revert PR branch commit range

## Reviewer focus checklist
- [ ] No behavior change when flags are unset
- [ ] Build-state transitions are coherent
- [ ] Queue startup/teardown is safe
- [ ] Telemetry paths are low overhead
- [ ] Backpressure guard prevents queue explosion

## Benchmark checklist
- [ ] Cold route run (baseline vs staged)
- [ ] Warm route run (baseline vs staged)
- [ ] capture median frametime, 1%/0.1% lows, >33ms spike count
- [ ] capture periodic `Async PSO snapshot` lines

## Next PR candidate (separate)
- true key-specific deferred compile payload execution
- safe render-thread fallback policy under frame budget pressure
- optional title prewarm manifest loader
