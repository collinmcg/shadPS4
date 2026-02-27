# Async PSO - Next Implementation Slices

## Current state
- Compile queue scaffold exists.
- Pipeline build state machine exists.
- Sync fallback remains active for safety.

## Next code slices
1. Key-specific deferred compile payloads in queue tasks.
2. ✅ Non-blocking retrieval path when key state is `Compiling` (flag: `SHADPS4_VK_PSO_NONBLOCK`).
3. Frame-budget aware sync fallback policy.
4. Optional title prewarm manifest loader.

## Exit criteria before default-on discussion
- Hitch count reduction proven on cold route
- No new crash signatures
- No major average FPS regression
