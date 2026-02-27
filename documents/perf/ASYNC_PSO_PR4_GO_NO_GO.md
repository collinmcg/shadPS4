# PR4 Go / No-Go Checklist

## Go conditions
- [ ] Baseline mode unchanged (no env flags)
- [ ] Async staged mode stable for 30+ min session
- [ ] Prewarm loader does not crash if manifest missing/invalid
- [ ] Snapshot logs show sensible queue/deferred counters

## No-Go triggers
- [ ] new crash signatures in Vulkan pipeline path
- [ ] rising `d_fail` without recovery
- [ ] repeated queue pressure saturation (`q_throttle`/`q_skip` growth)

## Rollback
1. unset all `SHADPS4_VK_PSO_*` env vars
2. relaunch emulator
3. if needed, revert PR4 branch
