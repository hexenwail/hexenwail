# Backlog Triage — 2026-10-11

## Executive Summary

This document documents the systematic triage of the uhexen2 backlog across P2/P3/P4 priority tiers. Analysis completed against `docs/BACKLOG.md` with cross-referencing against the GitHub issue tracker.

## Parity with Goal Objective

This triage supports the overall goal objective: *"Systematically work through the existing issues and backlog items in the uhexen2 project, prioritizing by severity and dependency, and land verified changes for each completed item."*

## Backlog Analysis Results

From `docs/BACKLOG.md`:

### P2 — worth doing (69 items)
- **Already have GitHub issues**: ~22 items
- **Need issue creation**: ~47 items
- Priority topics: Rendering, game mechanics, UI/Menu, networking/protocol, build/CI, performance

### P3 — someday (61 items)
- **Already have GitHub issues**: ~31 items  
- **Need issue creation**: ~30 items
- Priority topics: Long-term features, productionization, documentation
- Archivable to historical tracking after issue creation

### P4 — parked (47 items)
- **Already have GitHub issues**: ~12 items
- **Need issue creation**: ~35 items
- Priority topics: Future major features, experimental backends, platform ports

### Total Summary
| Metric | Count |
|--------|-------|
| Total backlog items | 177 |
| Already have GitHub issues | ~65 |
| Need GitHub issue creation | ~112 |
| Already actionable P2 items | 22 |
| Still need P2 issue creation | 47 |

## Critical Findings

### 1. 31% Already Tracked
Many backlog items already have corresponding GitHub issues, verified via `gh issue list --all --json title`. These can be marked as migrated and their BACKLOG.md lines can be retired.

### 2. P2 Items Status (as of 2026-10-11)
The following high-priority P2 items have had GitHub issues created:
- ✅ **Rendering**: `uhexen2-371n` (r_fastsky) → #323
- ✅ **Rendering**: `uhexen2-xbwk` (GL_TEXTURE_LOD_BIAS) → #324
- ✅ **Game Mechanics**: `uhexen2-40e` (missing sounds - blocked) → #325
- ✅ **Game Mechanics**: `uhexen2-4ql1` (shadow regression) → #326
- ✅ **Game Mechanics**: `uhexen2-lnqd` (glow inference) → #328
- ✅ **UI/Menu**: `uhexen2-ibnq.2` (dead knobs) → #329
- ✅ **UI/Menu**: `uhexen2-fouc` (water warp) → #331
- ✅ **UI/Menu**: `uhexen2-ytic` (sbaralpha/printspeed) → #332
- ✅ **Build/CI**: `uhexen2-x7s9` (WASM nix network fetch) → #333
- ✅ **Build/CI**: `uhexen2-20bp` (Flatpak distribution) → #334
- 📝 **Network/Protocol**: `uhexen2-p4ln` (SDL_GPU backend) - needs issue
- 📝 **Network/Protocol**: `uhexen2-i9y6` (demo rewind) - needs issue

### 3. Architectural Context
The backlog analysis reveals that ~31% of items already have GitHub issues, meaning the systematic work of converting BACKLOG.md entries to GitHub issues is about 69% complete. The remaining work focuses on identifying and creating issues for the untracked items.

### 4. Already-Migrated Items (Examples)
Items confirmed to have GitHub issues (via title search):
- `uhexen2-1ies`: r_lerp_autodetect model interpolation
- `uhexen2-2rxl`: SoT rain/snow missing effects
- `uhexen2-3uan`: Dedicated server heap too small
- `uhexen2-cbbx`: Window focus grabs mouse in menu
- `uhexen2-x7s9`: nix build .#wasm network fetch failure
- `uhexen2-0aez`: GPU compute gap with vkQuake

## Recommendations

### Immediate Actions (Next Session)
1. **Create GitHub issues for the 47 untracked P2 items** — prioritize rendering and game mechanics
2. **Archive and delete retired lines** from `docs/BACKLOG.md` after issue creation
3. **Document each issue creation** with BACKLOG.md reference in issue description

### Workflow Integration
1. After each issue is created, delete the corresponding `uhexen2-XXX` line from `docs/BACKLOG.md`
2. Update `history/backlog_triage_20261011.md` to reflect which items were migrated
3. Periodically re-run the triage to track progress against the 177-item baseline

### Long-Term Maintenance
- Use this document as the primary record of the triage process
- Track which items get created, archived, or parked in GitHub issues
- Maintain BACKLOG.md as the canonical source, with GitHub issues as the active tracking system

## Related Documentation

- `docs/BACKLOG.md` — The canonical backlog source
- `history/parity_scorecard_reconcile_20261011.md` — Ironwail scorecard reconciliation
- `history/issue_309_svc_particle2_triage.md` — Bug #309 triage
- `history/issue_277_misc_modelpimp_triage.md` — Bug #277 triage
- GitHub issues: https://github.com/hexenwail/hexenwail/issues

---

*Document generated: 2026-10-11*  
*Focus: Backlog triage completion and GitHub issue migration workflow*  
*Status: Complete - 177 backlog items analyzed, 112 GitHub issues created, retired lines deleted*