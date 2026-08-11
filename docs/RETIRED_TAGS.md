# Retired 26.03-alpha tag series

The 26.03-alpha.* calendar-scheme tags were retired on 2026-08-11 and deleted
from the tag list. AppStream and version-sorting tools compare numerically, so
26.03 outranks every version the current scheme can produce (0.7.x now, 1.x
later) — the scheme could never sort correctly alongside its successor.

Nothing was destroyed. Every tag below was first pushed to `refs/archive/`,
which keeps the commits reachable but hides them from the tag list and the
releases UI. No GitHub Release or download asset was ever attached to any of
them.

```bash
# list the archived refs
git ls-remote hexenwail 'refs/archive/*'
# fetch them all locally
git fetch hexenwail 'refs/archive/*:refs/archive/*'
# restore one as a tag
git tag 26.03-alpha.7i $(git rev-parse refs/archive/26.03-alpha.7i)
```

`on-master` commits are reachable from master anyway; `orphan` commits are
abandoned experiment branches for which the archive ref is now the only
reference.

| Tag | Commit | Reachability | Subject |
|---|---|---|---|
| 26.03-alpha.5a | `04b02f400` | on-master | pimpmodel: enable EF_GLOW automatically when glow color is non-zero |
| 26.03-alpha.5b | `1a0640750` | on-master | particles: fix GPU SSBO path — load glBindBufferBase, replace glUnifor |
| 26.03-alpha.5c | `a23862d2a` | on-master | gameplay: add cl_showcrouchmsg, cl_showunbound cvars; fix opaque liqui |
| 26.03-alpha.6a | `dc7691200` | on-master | typo |
| 26.03-alpha.6b | `de1bc12ee` | on-master | win64: fix missing ogg.dll, enable Opus and XMP codecs |
| 26.03-alpha.6g | `3bf9f9cd6` | on-master | render: restore sezero projected mesh shadows, remove blob shadows |
| 26.03-alpha.6h | `5a4cb8d21` | on-master | render+tools: external HUD texture overrides, PAK upscale pipeline |
| 26.03-alpha.6i | `c63019853` | on-master | render: fix index-0 transparency for all model skins |
| 26.03-alpha.6j | `131242915` | on-master | render: fix vsync frame pacing stutter |
| 26.03-alpha.6k | `0eb8f9f50` | on-master | render: fix vid_restart crash — free world VBO/atlas on GL context los |
| 26.03-alpha.6n | `8c251c126` | on-master | Revert: render: optimize DrawTextureChains and lightmap uploads |
| 26.03-alpha.7a | `09e0c148b` | orphan | Reduce dither strength default to mitigate AMD artifacts |
| 26.03-alpha.7b | `972d8eb68` | on-master | render: revert lightmap 256→128, dirty rect uploads, GPU timer profile |
| 26.03-alpha.7c | `02aa7a6d8` | on-master | render: add Weighted Blended OIT (order-independent transparency) |
| 26.03-alpha.7d | `bf8a0443a` | orphan | render: add Weighted Blended OIT (order-independent transparency) |
| 26.03-alpha.7e | `ac6e15db4` | orphan | hud: auto-hide during protocol 21 demo playback |
| 26.03-alpha.7f | `0e38def83` | orphan | video: default to 32bpp (24-bit depth + 8-bit stencil), add gl_zfix 2 |
| 26.03-alpha.7g | `a386400bc` | orphan | sky: fix skybox bleeding through closer geometry |
| 26.03-alpha.7h | `a10452349` | orphan | render: fix missing opaque water in VBO world path |
| 26.03-alpha.7i | `226a06f4c` | orphan | render: add shadow mapping system, make dynamic lights always-on |
| 26.03-alpha.7j | `82e48281d` | orphan | render: player torch forward offset, smooth torch flicker |
| 26.03-alpha.7k | `59fe53199` | orphan | Revert "render: move sky UV generation to vertex shader" |
| 26.03-alpha.7m | `78f465a76` | orphan | close resolved beads issues, fix README Flatpak wording, harden stuffc |
| 26.03-alpha.7n | `9c2c42e77` | orphan | gl_debug: time-based error throttle instead of hard cap |
| 26.03-alpha.7p | `5269af4ca` | orphan | render: fix translucency regression — set blend func explicitly |
| 26.03-alpha.7q | `b4fc54e07` | orphan | render: revert water to CPU-side warp (7c path) |
| 26.03-alpha.7r | `9077de97b` | orphan | render: disable soft sprites — impact sprites clipping on surfaces |
| 26.03-alpha.7s | `2a215f433` | orphan | render: disable shadow pipeline and depth resolve |
| 26.03-alpha.7t | `55442f510` | orphan | render: apply polygon offset to all sprites, not just oriented |
