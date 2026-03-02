# Bloodborne 1.09 eboot reevaluation (CUSA03173 artifact)

Date: 2026-03-02

## Artifact reviewed
- Path: `/media/sf_vbshare/base/eboot.bin`
- Size: `93,825,704` bytes
- SHA256: `d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9`
- `sce_sys/param.sfo` signals AppVer `01.09`, Title IDs include `CUSA03173` and `CUSA00207` metadata strings.

## What was validated
- Parsed upstream patch source: `shadps4-emu/ps4_cheats/PATCHES/Bloodborne.xml`
- Checked all AppVer `01.09` line addresses against this eboot size using shadPS4 offset logic (`Address - 0x400000`).
- Recomputed overlap/conflict behavior from the patch set.

## Critical findings

### 1) Two lines in `Performance Patch [READ NOTE]` are out-of-range on this artifact
- `0x603AD1A`
- `0x603AC8D`

Given current patcher behavior in `src/common/memory_patcher.cpp` (no explicit eboot bounds check before `memcpy`), these addresses are unsafe for this dump and should not be enabled as-is.

### 2) Region/version packing is likely over-broad
The XML combines many CUSA IDs under one address set. This reevaluation suggests at least part of the monolithic patch pack is not universally valid across all included IDs/build variants.

### 3) Existing high-conflict groups remain the primary instability risk
- `Better 30 FPS` vs `60 FPS (no deltatime)` vs `Uncap FPS + (Deltatime)`
- Any of the above with `Disable vsync`
- `Performance Patch` with `Model LOD` and `Light Grid`
- Resolution patches are mutually exclusive

## Re-evaluated recommendation (for this artifact)

## Safe baseline
- `Disable Motion Blur`
- `Disable Chromatic Aberration`
- `FMOD Crash Fix`
- one model LOD patch (`LOD 1` preferred first)
- one light grid patch matching target output
- one resolution patch only

## Timing profile
Use exactly one:
- `Better 30 FPS [READ NOTES]` **or**
- `Uncap FPS + (Deltatime)`

Avoid layering with `Disable vsync` in first pass.

## High-risk pack handling
- Keep `Performance Patch [READ NOTE]` disabled until split/cleaned for this specific CUSA/build.
- Do not ship as default while out-of-range lines exist for this artifact.

## Experimental composite status
`documents/perf/Bloodborne.experimental.xml` remains an opt-in, disabled-by-default profile and all of its addresses are in-range for this artifact.

## Next actions for breakthrough work
1. Split Bloodborne XML into per-CUSA (or per hash) variants for 1.09.
2. Add bounds checks in patcher before memory writes.
3. Validate timing packs via 3-zone frametime protocol (Central Yharnam, VFX-heavy boss, indoor dense area).
4. Only then fold additional aggressive lines into a public default profile.
