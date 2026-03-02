# Bloodborne base folder review + novel patch proposal

Date: 2026-03-02
Path reviewed: `/media/sf_vbshare/base`

## Review coverage
- Enumerated full tree (game data + modules + metadata).
- Verified game metadata in `sce_sys/param.sfo` indicates AppVer `01.09`.
- Confirmed payload scale: ~30G total, with map-heavy footprint (`dvdroot_ps4/map` ~20G).
- Major heavy folders observed:
  - `dvdroot_ps4/map` (~20G)
  - `dvdroot_ps4/sound` (~3.6G)
  - `dvdroot_ps4/parts` (~2.4G)
  - `dvdroot_ps4/chr` (~1.9G)
  - `dvdroot_ps4/obj` (~1.2G)

## Important limitation (honest)
A true claim of "massive 4K60 uplift with no noticeable visual drop" requires runtime frametime capture in actual gameplay zones.
This environment can do static binary/data analysis and patch safety checks, but cannot run Bloodborne gameplay loops and collect 1% lows/frametime variance directly.

## One new, novel experimental patch
Added to `documents/perf/Bloodborne.experimental.xml`:

### `4K60 Balanced Light-Grid (Experimental)`
- Address `0x02695CB6` -> width `0x00000C00` (3072)
- Address `0x02695CC0` -> height `0x000006C0` (1728)

### Why this is novel
- Existing presets jump from 1440p-grid (`2560x1440`) to 4k-grid (`3840x2160`).
- This introduces a midpoint grid (`3072x1728`) intended to reduce lighting-grid cost versus full 4k while preserving more lighting detail than 1440p-grid.

### Static validation completed
- XML parse valid.
- Both patch addresses are in-range for provided eboot.
- Patch lines are non-overlapping with each other.

## Recommended test protocol (to validate claims)
1. Use same scene route for all runs (Central Yharnam + one VFX-heavy boss + one indoor dense area).
2. Fixed settings per run, same driver and emulator commit.
3. Compare three configs only:
   - `4k Light Grid`
   - `4K60 Balanced Light-Grid (Experimental)`
   - `1440p Light Grid`
4. Capture:
   - average FPS
   - 1% low / 0.1% low
   - frametime stddev
   - subjective artifact report (shadow shimmer, lighting pop).
5. Accept only if midpoint profile improves 1% lows materially while artifact score stays close to 4k grid.

## Current confidence
- Safety confidence: high (address/range/format checks)
- Performance uplift confidence: medium (strongly plausible)
- "Massive uplift" confidence: unproven until runtime benchmarking
