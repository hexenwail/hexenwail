# Bindless Texture Testing Procedure

> **STATUS 2026-05-15 (uhexen2-mxx5.5)**: This procedure is currently **non-functional**. The bindless code path is a skeleton: shaders compile with `BINDLESS=0` regardless of the `+bindless` flag (init-order bug — `GL_Shaders_Init()` runs before `gl_bindless_able` is set), `TEX_BINDLESS` has zero callers, every texture's `bindless_handle` is 0, and the SSBO upload writes zero handles. `+bindless` will produce indistinguishable output from the fallback path. Do not run this procedure as a parity test until the underlying bugs are fixed.

Build: `nix build .#default` produces `/nix/store/.../bin/glhexen2` (~1.4 MB)

## Prerequisites
- Game data in `/home/josh/hexen2/` (data1 directory)
- Test maps: mill.bsp (confirms: ✓ present at sot/maps/mill.bsp)
- Display with OpenGL 4.3 + ARB_bindless_texture support

## Test 1: Fallback Rendering (Default, -nobindless)
```bash
cd /home/josh/hexen2
./glhexen2 -nobindless
> map mill
> r_speeds 2
```

**Observe:**
- r_speeds output shows GPU time breakdown (world bucket)
- Visual correctness: no missing/corrupted textures
- No crashes on complex scenes
- Record baseline GPU world time

## Test 2: Bindless Rendering (+bindless enabled)
```bash
cd /home/josh/hexen2
./glhexen2 +bindless
> map mill
> r_speeds 2
```

**Observe:**
- r_speeds GPU world bucket time (compare vs Test 1)
- Target: measurable reduction in texture bind overhead
- Visual correctness: identical to fallback
- No texture artifacts or black screens
- No crashes

## Test 3: Hardware Capability Detection
- Machine without ARB_bindless_texture: +bindless flag should silently fall back to uniform sampling
- Console should show capability status (check for debug output)

## Expected Results
- Bindless path: faster world bucket time due to elimination of per-surface glBindTexture calls
- Fallback path: baseline performance (current shipping behavior)
- Both paths: identical visual output
- No regressions on maps with many distinct textures (mill.bsp is good test)

## Known Implementation Details
- SSBO at shader storage binding point 0
- Macro injection: BINDLESS=1/0 injected after #version line
- Texture handles made resident on load, non-resident on unload (PLANNED — `TexMgr_LoadImage` currently ignores bindless entirely)
- uint64_t handles packed into flat uvec4 varyings
- Sampler2D constructor: sampler2D(uvec2) from packed handles
