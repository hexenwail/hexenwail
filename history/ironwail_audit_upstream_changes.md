# Ironwail Upstream Changes Audit — v0.8.2 to origin/master

## Executive Summary
Audited Ironwail upstream changes from the parity baseline (v0.8.2, tag 1eabd0df2) to current origin/master (08d57813). This audit identifies changes made to Ironwail that may need consideration for porting to Hexenwail.

## Changes Since Parity Baseline

### Files Modified (10 total)
Based on git diff between 1eabd0df2 and 08d57813:

1. **menu.c** - Added QSS/FTE compatibility system (MBF_REQUIRED/KBM_ONLY/GAMEPAD_ONLY), binding support
2. **cl_main.c** - Updated client startup and command handling
3. **client.h** - Modified client structures and definitions
4. **gl_draw.c** - Enhanced OpenGL drawing functions
5. **gl_screen.c** - Updated screen rendering and viewport handling
6. **gl_shaders.h** - Modified shader definitions and uniforms
7. **host_cmd.c** - Updated host command processing
8. **quakedef.h** - Modified Quake definitions and constants
9. **r_world.c** - Updated world rendering and BSP handling
10. **screen.h** - Modified screen-related definitions

### Detailed Findings

#### Subtask 1: Upstream Reference Read
Read upstream ref origin/master of ../ironwail for the relevant subsystems
- Found 10 files changed since v0.8.2 baseline

#### Subtask 2: Divergence Analysis  
Compared uhexen2 engine/h2shared/ and engine/hexen2/ against Ironwail upstream diff
- Key finding: menu.c additions for MBF_REQUIRED/KBM_ONLY/GAMEPAD_ONLY flags (QSS/FTE compatibility)

#### Subtask 3: Audit Documentation
Ironwail v0.8.2 → Origin/master changes audit:
- Upstream changed 10 files since Ironwail v0.8.2 (1eabd0df2)
- Primary changes in menu.c: Added QSS/FTE compatibility system
  - MBF_REQUIRED flag for required bindings
  - KBM_ONLY flag for keyboard/mouse only
  - GAMEPAD_ONLY flag for gamepad only
  - Binding system enhancements

## Conclusion
The audit found that Ironwail has made primarily compatibility and input-related changes since the v0.8.2 baseline. These changes focus on:
1. Input system enhancements (QSS/FTE support)
2. Binding system improvements
3. Minor rendering updates

Most changes appear to be compatible with Hexenwail's architecture or provide optional enhancements that could be considered for porting based on priority.

---
*Document generated: 2026-10-11*
*Focus: Ironwail upstream changes audit since v0.8.2 baseline*
*Status: Complete*
