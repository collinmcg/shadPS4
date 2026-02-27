# PR4 Plan (Prewarm + rollout polish)

## Scope
1. Prewarm manifest schema + loader stub
2. Startup scheduler hook for low-priority prewarm queueing
3. Runtime tuning defaults and env knobs consolidation
4. Final rollout/go-no-go checklist

## Safety
- Feature remains opt-in via async flags.
- Default path unchanged.
