# PR3 Execution Plan (True Deferred Compile Path)

## Scope
1. Key-specific deferred compile payload execution (background)
2. Thread-safe ready handoff to render cache
3. Render-thread non-blocking retrieval of ready pipelines
4. Deterministic sync fallback on timeout/budget miss
5. Benchmark validation checklist update

## Safety
- Feature remains flag-gated (`SHADPS4_VK_PSO_ASYNC`)
- Fallback to sync path preserved
- Default behavior unchanged when flags unset
