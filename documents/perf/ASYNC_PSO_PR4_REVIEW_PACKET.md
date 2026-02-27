# PR4 Review Packet - Prewarm + Rollout Polish

## PR
- https://github.com/collinmcg/shadPS4/pull/4

## What this PR adds
1) Prewarm manifest scaffolding
- `LoadPrewarmManifest()` reads optional text manifest
- tolerant parsing; comments/empty lines ignored

2) Startup scheduling hook
- `SchedulePrewarmEntries()` enqueues bounded low-priority payloads (cap 128)
- sorted by priority

3) Runtime gating
- `SHADPS4_VK_PSO_PREWARM=1` enables prewarm flow
- default behavior unchanged when unset

4) Operational docs
- go/no-go checklist
- manifest example format

## Risk
- Low in default mode
- Low/medium in staged prewarm mode

## Rollback
- unset prewarm/async env vars
- revert PR4 if required
