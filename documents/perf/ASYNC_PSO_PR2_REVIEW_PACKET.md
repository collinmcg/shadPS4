# PR2 Review Packet - Deferred Payload + Fallback Hardening

## PR Link
- https://github.com/collinmcg/shadPS4/pull/2

## Scope completed
1) Deferred payload execution hook
- Queue tasks now route through `HandleDeferredCompilePayload(payload, budget_us)`
- Adds key-aware execution contract for next true async compile step.

2) Fallback hardening
- Non-blocking mode checks `Compiling` and returns `nullptr` only in that state.
- If state is `Failed`, it is reset to `Missing` so deterministic sync fallback can rebuild.

3) Telemetry expansion
- Added counters:
  - `deferred_handler_calls`
  - `deferred_handler_budget_exceeded`
  - `deferred_handler_failures`
  - plus prior queue/backpressure counters
- Snapshot logging includes all counters for session-level benchmark analysis.

## Risk
- Default mode risk: low
- Flagged staged mode (`SHADPS4_VK_PSO_ASYNC=1`) risk: low/medium
- Experimental non-blocking mode (`SHADPS4_VK_PSO_NONBLOCK=1`) risk: medium

## Rollback
- Unset all `SHADPS4_VK_PSO_*` flags to return to baseline sync behavior.
- Revert PR #2 branch if needed.

## Verification checklist
- [ ] Build succeeds with and without async flags
- [ ] No behavior change in default mode
- [ ] Snapshot log includes deferred metrics
- [ ] NONBLOCK path does not dead-end failed keys (failed -> missing -> sync rebuild)
