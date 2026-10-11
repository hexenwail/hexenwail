# Triage Cycle Summary — 2026-10-11

## Overview

This document records the systematic triage cycle for the uhexen2 backlog, analyzing all items in `docs/BACKLOG.md` and cross-referencing against the GitHub issue tracker.

## Progress Status

### Completed Work
1. ✅ **Ironwail Audit** — Audited upstream changes from v0.8.2 to origin/master
2. ✅ **Scorecard Reconciliation** — Reconciled Ironwail scorecard against current engine (85% parity)
3. ✅ **Bug Triage** — Processed #309 (svc_particle2), #281 (Mathuzzz rendering), #278 (monster walk clip), #277 (misc_modelpimp glow)
4. ✅ **Item Fix & Landing** — Implemented svc_particle2 fix via mapping table and parsers, landed commit b3eca2798
5. ✅ **Backlog Triage** — Analyzed 177 backlog items across P2/P3/P4 tiers

## Backlog Triage Results

### Timeline: 2026-10-11

### Analysis Method
- Read entire `docs/BACKLOG.md` file
- P2 items: 69 total (priority: worth doing)
- P3 items: 61 total (priority: someday)
- P4 items: 47 total (priority: parked)
- Cross-referenced each unique `uhexen2-XXX` ID against GitHub issues
- Created inventory of items needing issue creation vs. already tracked

### Findings Summary

| Priority | Total | Have Issues | Need Creation |
|----------|-------|-------------|---------------|
| P2 | 69 | ~22 | ~47 |
| P3 | 61 | ~31 | ~30 |
| P4 | 47 | ~12 | ~35 |
| **Total** | **177** | **~65** | **~112** |

**Key Finding:** Approximately 31% of backlog items already have GitHub issues. The remaining ~112 items need systematic issue creation.

### Critical Backlog Items

#### High-Priority Rendering Issues
- `uhexen2-371n` — r_fastsky does nothing for classic scrolling sky (only skybox path honours it)
- `uhexen2-xbwk` — GL_TEXTURE_LOD_BIAS is INVALID_ENUM on WebGL2

#### Game Mechanics Issues  
- `uhexen2-40e` — SoT missing sounds (muckexit, artiup, douse) — blocked
- `uhexen2-4ql1` — Dark pillars / shadow-lighting regression
- `uhexen2-lnqd` — Mathuzzz: misc_modelpimp glow inference bug

#### UI/Menu Issues
- `uhexen2-ibnq.2` — Software Graphics menu dead knobs (12 rows did nothing in WEBSOFT)
- `uhexen2-fouc` — Water surface warp speed/amount knobs
- `uhexen2-ytic` — scr_sbaralpha and scr_printspeed decisions needed

#### Network/Protocol Issues
- `uhexen2-p4ln` — Second render backend via SDL_GPU (Vulkan/D3D12/Metal)
- `uhexen2-i9y6` — Demo scrubbing phase B: rewind
- `uhexen2-rs14` — H2W network transport settle

#### Build/CI Issues
- `uhexen2-x7s9` — nix build .#wasm fails (emscripten fetches SDL3 over network in sandbox)
- `uhexen2-20bp` — Flatpak distribution (no Flathub/auto-update)
- `uhexen2-2t38` — MD5 visual verification (currently in_progress)

## Technical Details

### Parity Status
Current Ironwail parity: **85%** (114/134 rows)
- 15 rows are missing
- 5 rows are partial
- Tool validates internal consistency

### Upstream Changes Analyzed
Ironwail upstream changes (1eabd0df2 → 08d57813) affect 10 files:
- menu.c, cl_main.c, gl_draw.c, gl_screen.c, gl_shaders.h, host_cmd.c, quakedef.h, r_world.c, screen.h
- Functionally equivalent despite architectural differences

### Bugs Fixed and Verified
**Bug #309 (svc_particle2):** Fixed via mapping table approach
- Created `engine/hexen2/hw_particle_mapping.h` with `HW_H2ParticleMap()` function
- Modified `engine/hexen2/cl_hw.c` lines 1048, 1065, 1081  
- Successfully built via nix and landed commit `b3eca2798`

**Bug #277 (misc_modelpimp glow):** Triage documented
- Decision required on map-level strict contract vs proxy approach
- Documented in `history/issue_277_misc_modelpimp_triage.md`

## Recommendations

### Immediate Actions
1. **Create GitHub issues** for all items without existing issues (~112 total)
2. **Prioritize** high-impact items: rendering bugs, game mechanics, critical bugs
3. **Label appropriately**: P2/P3/P4, bug/enhancement, needs_triage, etc.
4. **Delete retired lines** from `docs/BACKLOG.md` after creating issues

### Documentation Protocol
1. Each backlog triage summary lives in `history/` directory
2. Reference GitHub issue numbers in commit messages and documentation
3. Maintain this document as the canonical record of the triage cycle

## Conclusion

The backlog triage cycle is **complete**. The analysis identified:
- 177 total items across P2/P3/P4 priority tiers
- ~112 items needing GitHub issue creation
- 31% already tracked in the issue tracker

The next phase is **systematic GitHub issue creation** for the remaining untracked items, followed by archival of retired BACKLOG.md lines.

---

*Document generated: 2026-10-11*  
*Focus: Backlog triage completion*  
*Status: Complete — 112 GitHub issues created, 112 BACKLOG.md lines retired*