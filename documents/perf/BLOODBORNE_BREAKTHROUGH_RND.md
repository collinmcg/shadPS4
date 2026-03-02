# Bloodborne (AppVer 01.09) – Performance Breakthrough R&D for shadPS4

Date: 2026-03-02  
Target: `CUSA00207/208/299/900/1363/3023/3173`, `eboot.bin`, AppVer `01.09`

## Scope and inputs used
- Patch source inspected from `shadps4-emu/ps4_cheats/PATCHES/Bloodborne.xml` (via `gh api`).
- shadPS4 codebase inspected at `/home/cmcgowan/.openclaw/workspace/rnd/shadPS4-nightly`.
- Key implementation detail: patch loader only applies `<Metadata isEnabled="true">` entries (`src/common/memory_patcher.cpp`), and writes bytes directly at `g_eboot_address + (Address - 0x400000)`.

---

## 1) Conflict-safe patch profiles

## A. Profile: **30 FPS Stability (lowest stutter / best frametime consistency)**

### Include
- `Better 30 FPS [READ NOTES]`
- `Disable Motion Blur`
- `Disable Chromatic Aberration`
- Exactly one: `Model LOD 1 (Lower)` **or** `Model LOD 2 (Lowest)`
- Exactly one matching desktop/window target: `1080p Light Grid` / `1440p Light Grid` / `4k Light Grid`
- Optional for CPU spikes: `Disable HTTP Requests`

### Exclude (hard conflicts)
- `60 FPS (no deltatime)`
- `Uncap FPS + (Deltatime)`
- `Disable vsync`
- `Performance Patch [READ NOTE]` (overlaps at least `0x01972A7D`, and also collides with LOD + Light Grid addresses)

### Why this is conflict-safe
- Avoids all high-overlap timing packs (30 vs 60/uncap overlaps on ~43 addresses).
- Avoids double-writing `0x0216FC09` (LOD variants conflict with each other and with Performance Patch).
- Avoids double-writing `0x02695CB6` + `0x02695CC0` (Light Grid variants conflict with each other and with Performance Patch).

---

## B. Profile: **60 FPS Target (balanced for strong CPU/GPU, reduced game-speed issues)**

### Include
- `Uncap FPS + (Deltatime)` **(preferred over no-deltatime for frame-pacing behavior when FPS fluctuates)**
- `Disable Motion Blur`
- `Disable Chromatic Aberration`
- Exactly one: `Model LOD 1` or `Model LOD 2`
- Exactly one: matching `Light Grid` patch for output resolution
- Exactly one low render resolution patch if needed (e.g., `720p` or `900p`)
- Optional: `Increased Graphics Heap Sizes` when using >1080p output or high-res texture mods

### Exclude
- `60 FPS (no deltatime)` (mutually overlapping with Uncap on ~86 addresses)
- `Better 30 FPS [READ NOTES]`
- `Disable vsync` unless intentionally pushing >60 with external cap strategy
- `Performance Patch [READ NOTE]` in first pass (too broad/opaque; retest only in isolated branch)

### Why this is conflict-safe
- Uses one timing framework only (Uncap+Deltatime).
- Keeps non-overlapping visual-cost reducers.
- Avoids duplicate line writes among fps packs.

---

## C. Profile: **Low-End Rescue (maximize playability over fidelity)**

### Include
- `Better 30 FPS [READ NOTES]`
- `Resolution Patch (560p)` (or `720p` as quality fallback)
- `Model LOD 2 (Lowest)`
- `1080p Light Grid` (if display output is 1080p; choose matching variant otherwise)
- `Disable Motion Blur`
- `Disable Chromatic Aberration`
- `Disable HTTP Requests`

### Optional (high-risk)
- `Performance Patch [READ NOTE]` **only as separate A/B run**, not layered into baseline rescue

### Exclude
- Any 60/Uncap pack
- Any second resolution patch simultaneously
- Any second light-grid variant simultaneously

---

## Fast conflict matrix (critical collisions)
- `Better 30 FPS` ↔ `60 FPS (no deltatime)`: heavy overlap (~43 addresses)
- `Better 30 FPS` ↔ `Uncap FPS + (Deltatime)`: heavy overlap (~43)
- `60 FPS (no deltatime)` ↔ `Uncap FPS + (Deltatime)`: very heavy overlap (~86)
- Any FPS pack above ↔ `Disable vsync`: overlap at `0x025B3271` and `0x0243487E`
- `Performance Patch` ↔ `Light Grid`: overlap at `0x02695CB6`, `0x02695CC0`
- `Performance Patch` ↔ `Model LOD`: overlap at `0x0216FC09`

---

## 2) Novel patch hypotheses (outside current patch set)

These are **not** in the current XML and need binary/disasm validation before shipping.

### Hypothesis 1 — **Shadow cascade clamp + update decimation**
- Idea: identify and clamp dynamic shadow cascade distance/count and reduce shadow-map update cadence (e.g., every N frames for far cascades).
- Rationale: Bloodborne is heavy in shadowed outdoor scenes; reducing far-cascade churn can cut both CPU draw prep and GPU raster cost.
- Risk: obvious pop-in, crawling shadows, unstable bias artifacts.
- Validation required: locate shadow quality globals/functions in `eboot` (likely debug vars adjacent to existing render-tuning patches), verify no race with cutscene camera paths.

### Hypothesis 2 — **Volumetric/fog half-rate rendering path**
- Idea: force fog/volumetric passes to execute every 2nd frame or render at half resolution and reuse history.
- Rationale: can significantly improve GPU frametime in heavy atmospheric zones.
- Risk: ghosting/trailing, temporal instability, possible mismatch with post chain.
- Validation required: identify volumetric pass toggles or dispatch dimensions in disassembly; confirm no crash from stale resource assumptions.

### Hypothesis 3 — **Particle/alpha overdraw governor**
- Idea: lower max particles per emitter and clamp alpha-heavy effect density (blood splashes, sparks, mist, boss VFX).
- Rationale: overdraw/fragment pressure is a known bottleneck at higher resolutions.
- Risk: gameplay readability changes (telegraph clarity), “missing effects” perception.
- Validation required: identify emitter budget constants and ensure deterministic behavior in scripted boss fights.

### Hypothesis 4 — **AI/update LOD throttling outside combat radius**
- Idea: lower update frequency for distant/offscreen actors (animation + behavior tick decimation).
- Rationale: CPU frametime stabilization on low-end systems during mob-dense traversal.
- Risk: AI desync edge-cases, delayed aggro transitions, scripting regressions.
- Validation required: locate actor manager tick scheduling in eboot; test heavily scripted encounters.

### Hypothesis 5 — **Asynchronous occlusion query leniency**
- Idea: increase occlusion hysteresis / reduce query strictness so objects aren’t rapidly toggled visible/invisible.
- Rationale: avoids CPU/GPU sync stalls and reduces culling oscillation micro-stutter.
- Risk: slight overdraw increase or occasional “late hide/show.”
- Validation required: identify culling thresholds and query wait points; compare frame-time variance, not just average FPS.

---

## 3) Proposed composite XML snippet

Goal: `Performance Breakthrough Experimental` with **non-overlapping lines only** (unique addresses, no internal collisions).

```xml
<Metadata Title="Bloodborne"
          Name="Performance Breakthrough Experimental"
          Note="Experimental composite: no overlapping addresses inside this profile. Built for AppVer 01.09. Validate stability per-zone before public use."
          Author="R&D Composite"
          PatchVer="0.1"
          AppVer="01.09"
          AppElf="eboot.bin"
          isEnabled="false">
    <PatchList>
        <!-- Timing base (from Better 30 FPS family, selected subset) -->
        <Line Type="bytes" Address="0x024347F3" Value="31D2"/>
        <Line Type="bytes" Address="0x024348A2" Value="EB3D"/>
        <Line Type="bytes" Address="0x02434D98" Value="9090"/>

        <!-- Lightweight visual cost cuts -->
        <Line Type="bytes" Address="0x0269FAA8" Value="C783AC000000000000009090"/>
        <Line Type="bytes" Address="0x026A057B" Value="EB16"/>

        <!-- Geometry/culling pressure -->
        <Line Type="bytes" Address="0x0216FC09" Value="B90200000090"/>

        <!-- Light grid tuned for 1080p output -->
        <Line Type="bytes" Address="0x02695CB6" Value="C783A878000080070000"/>
        <Line Type="bytes" Address="0x02695CC0" Value="C783AC78000038040000"/>

        <!-- Resolution downscale baseline -->
        <Line Type="bytes32" Address="0x055289F8" Value="0x00000500"/>
        <Line Type="bytes32" Address="0x055289FC" Value="0x000002D0"/>

        <!-- Network/background stability hygiene -->
        <Line Type="bytes" Address="0x0227E2F0" Value="9090909090"/>
    </PatchList>
</Metadata>
```

Notes:
- This snippet intentionally avoids `Performance Patch [READ NOTE]` to keep variable interactions narrow.
- Uses one LOD line, one Light Grid pair, one resolution pair, and a minimal timing subset.
- Even with non-overlap, functional interactions can still occur (same system via different callsites).

---

## 4) Implementation and test protocol

## Phase 0 — Baseline hygiene
1. Use a clean shader cache for each profile run.
2. Lock emulator settings per run:
   - Same GPU driver version
   - Same shadPS4 commit
   - Same `presentMode`, `vblankFrequency`, fullscreen mode
3. Disable Vulkan validation layers for perf runs (`vk validation` adds overhead).
4. Ensure only one target profile is enabled (`isEnabled="true"`) at a time.

## Phase 1 — Profile-level A/B
- Baseline: no gameplay/perf patches (only mandatory crash fix if needed).
- Test each profile for 20–30 minutes across representative zones:
  - Central Yharnam traversal (streaming + combat)
  - Boss arena with heavy VFX (particle/alpha stress)
  - Indoor dense geometry area (CPU culling stress)
- Collect:
  - Avg FPS
  - 1% low / 0.1% low
  - Frametime variance (stddev)
  - Crash/hang count

## Phase 2 — Address-level bisect (if unstable)
- Disable half the lines in current profile, retest, binary-search the culprit.
- Prioritize suspect groups:
  1) Timing writes (`0x02434xxx` family)
  2) LOD/light-grid writes
  3) Resolution writes

## Phase 3 — Experimental hypothesis validation
For each novel hypothesis:
1. Locate candidate function/constant in disassembly (Ghidra/IDA).
2. Build minimal patch (1–3 lines).
3. Validate:
   - Boot to gameplay
   - 30-min soak in stress zone
   - Boss/cutscene sanity checks
4. Only then merge into composite candidate.

## Phase 4 — Regression checklist
- Character creation cutscene
- Poison tick behavior
- Ebrietas/audio-heavy scenario
- Save/load cycle and area transitions
- UI integrity at selected aspect ratio/resolution

---

## Uncertainty and required binary/disassembly validation
- Current XML has large monolithic packs; intent of many constants is not self-documenting.
- `Performance Patch [READ NOTE]` likely toggles debug menu-backed runtime variables; side effects may depend on GUI init order as noted by the patch author.
- Any “novel” hypothesis above requires:
  - Symbol-less reverse engineering of AppVer 01.09 eboot
  - Validation that patched bytes align to instruction boundaries and calling convention expectations
  - Cross-check that no hidden overlap exists with other active patches at runtime (even if addresses differ)

## Practical recommendation
- For near-term reliability: ship curated profile presets based on existing known-good lines + strict conflict rules.
- For breakthrough gains: invest in targeted disassembly around shadow/volumetric/particle systems and iterate with single-variable experiments before composing larger packs.
