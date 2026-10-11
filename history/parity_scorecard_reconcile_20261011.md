# Parity Scorecard Reconciliation — 2026-10-11

**Purpose**: Document the state of the Ironwail parity scorecard during the systematic work on the mu6iitm5-91daeg goal.

## Current Status (verified via `tools/ironwail_scorecard.py --check`)

```
Category                            ✅    🔶    ❌    ➖
Rendering — GPU Pipeline           13    1    1    0
Rendering — Visual/Shading         26    1    2    1
Performance / Engine               11    0    2    1
UX / Menus / HUD                   40    1    6    1
Input / Controller                 13    0    1    1
Audio                               3    0    1    1
Network / Protocol                  3    0    1    2
Server / Gameplay                   2    1    1    0
Debug / Tooling                     3    1    0    0
Steam / Platform                    0    0    0    2
TOTAL                             114    5   15    9
```

**Parity: 85% ported (114/134), 4% partial, 11% missing** (excluding N/A)

## Reconciliation Notes

### Unchanged since 2026-09-03
The scorecard has not been updated since the last major work batch. The same 114 rows remain ported.

### CI Integration
The `tools/ironwail_scorecard.py --check` is a CI step (`.github/workflows/lint.yml`) that runs before each commit to catch drift between the tracker and actual engine state.

### Audit Findings from Ironwail Upstream Audit
From the audit of Ironwail changes since parity baseline v0.8.2 (1eabd0df2) to origin/master (08d57813):

1. Some features (like FPS/clock positioning) are implemented differently in uhexen2 but achieve equivalent function
2. The canvas enum system (CANVAS_TOPRIGHT, etc.) is used differently
3. Several features already have workaround approaches in uhexen2

### Next Steps
Review the ❌ (15 missing) and 🔶 (5 partial) rows to prioritize future work.

## References
- Scorecard source: `history/ARE_WE_IRONWAIL_YET.md`
- Tracking issue: GitHub #272
- Parity baseline commit: Ironwail v0.8.2 (1eabd0df2)