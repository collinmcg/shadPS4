# PR3 Review Packet - Deferred Execution Wiring

## PR
- https://github.com/collinmcg/shadPS4/pull/3

## Implemented in PR3
1) Key-aware deferred payload wiring
- Queue payload now carries concrete key context (graphics/compute key optional fields).
- Queue tasks route through `HandleDeferredCompilePayload`.

2) Deferred outcome handling
- Handler now tracks budget age and failure outcomes.
- On budget breach/failure, marks key state `Failed`.
- On normal staged path, keeps key in `Queued` until sync fallback compiles to `Ready`.

3) Fallback determinism
- Existing non-blocking fetch path already resets `Failed -> Missing`, enabling deterministic sync rebuild.

## Safety
- No default behavior change when async flags unset.
- True Vulkan background compile still intentionally deferred to next PR.

## Rollback
- Unset `SHADPS4_VK_PSO_*` env vars.
- Revert PR3 branch if needed.

## Verify
- [ ] Build succeeds
- [ ] No default-mode regressions
- [ ] Async snapshot logs include deferred counters
- [ ] Non-blocking path eventually recovers via Failed->Missing->sync rebuild
