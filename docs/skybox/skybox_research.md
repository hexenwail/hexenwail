# QuakeSpasm Skybox Implementation Research

## QuakeSpasm Architecture (gl_sky.c)

### Key Data Structures

```c
// Global state
char skybox_name[1024];           // Current skybox identifier
gltexture_t *skybox_textures[6];  // Six cube face textures
gltexture_t *solidskytexture;     // Solid cloud layer
gltexture_t *alphaskytexture;     // Alpha cloud layer

// Bounding boxes for 6 faces (2D projection)
float skymins[2][6];
float skymaxs[2][6];

// Configuration
float skyfog;                     // Fog density
float skyflatcolor[3];           // Fallback solid color
```

### Console Variables

```c
r_fastsky         // Enable flat-color sky (performance mode)
r_sky_quality     // Subdivision quality (0.125 to 2)
r_skyalpha        // Cloud transparency (0-1)
r_skyfog          // Fog opacity (0-1)
```

### Core Functions

#### 1. Initialization
- **`Sky_Init()`** - Register cvars and console commands

#### 2. Texture Loading
- **`Sky_LoadSkyBox(const char *name)`**
  - Loads 6 textures from `gfx/env/` directory
  - Naming: `{name}{suffix}.{tga|pcx}` where suffix = rt, bk, lf, ft, up, dn
  - Fallback to solid sky if any face fails

- **`Sky_LoadTexture()`** - Classic 256x128 scrolling sky
- **`Sky_LoadTextureQ64()`** - Quake64 32x64 format

#### 3. Map Integration
- **`Sky_NewMap()`**
  - Parses worldspawn entity keys: "sky", "skyname", "qlsky"
  - Calls `Sky_LoadSkyBox()` if skybox specified

- **`Sky_ClearAll()`** - Cleanup on map unload

#### 4. Geometry Processing
- **`Sky_ProcessTextureChains()`** - Find sky surfaces in world
- **`Sky_ProcessEntities()`** - Find sky on brush entities
- **`Sky_ProcessPoly(glpoly_t *p)`** - Process individual polygon
- **`Sky_ProjectPoly(int nump, vec3_t vecs)`** - Project 3D to skybox 2D
- **`Sky_ClipPoly()`** - Recursive polygon clipping against cube planes

#### 5. Rendering
- **`Sky_DrawSky()`** - Main entry point (called once per frame)
  - Resets bounds
  - Processes geometry
  - Calls appropriate draw function

- **`Sky_DrawSkyBox()`** - Render 6-face cubemap
  - Iterates through faces with valid bounds
  - Binds texture, emits quads
  - Optional fog pass

- **`Sky_DrawSkyLayers()`** - Render scrolling clouds
  - Dual-layer (solid + alpha)
  - Animated texture coordinates
  - Quality-based subdivision

#### 6. Utilities
- **`Sky_EmitSkyBoxVertex()`** - Generate textured vertex
- **`Sky_GetTexCoord()`** - Calculate animated UV coords
- **`Sky_SetBoxVert()`** - Transform 2D face coords to 3D

### Coordinate System

**Face ordering (0-5):**
- 0: Right (+X)
- 1: Back (-Y)
- 2: Left (-X)
- 3: Front (+Y)
- 4: Up (+Z)
- 5: Down (-Z)

**Lookup tables:**
- `st_to_vec[6][3]` - Convert 2D face coords to 3D vectors
- `vec_to_st[6][3]` - Convert 3D vectors to 2D face coords
- `skytexorder[6]` - Texture binding order

### Rendering Pipeline

```
Frame Start
    ↓
Sky_DrawSky() - Main entry
    ↓
Reset bounds to infinity
    ↓
Sky_ProcessTextureChains() - Find world sky surfaces
    ↓
Sky_ProcessEntities() - Find entity sky surfaces
    ↓
For each sky polygon:
    Sky_ProcessPoly()
        ↓
    Sky_ProjectPoly() - Clip & project to cube faces
        ↓
    Update skymins/skymaxs bounds
    ↓
Check r_fastsky → Draw flat color (fast path)
    ↓
Has skybox textures?
    ├─ Yes → Sky_DrawSkyBox()
    │         - Iterate 6 faces
    │         - Draw textured quads
    │         - Optional fog overlay
    └─ No  → Sky_DrawSkyLayers()
              - Draw scrolling clouds
              - Dual-layer blending
```

## Hexen2 Current State

### Existing Sky Code
- `R_DrawSkyChain()` referenced in gl_rsurf.c:1009
- `R_DrawSkyBox()` referenced in gl_rsurf.c:1350
- `r_skyalpha` cvar exists in gl_rmain.c

### Missing Components
- No dedicated gl_sky.c file
- No skybox texture loading system
- No modern cubemap rendering
- Limited to classic scrolling sky

### Integration Points
- gl_rsurf.c - Surface rendering
- gl_rmain.c - Main rendering loop
- Need to add Sky_ProcessTextureChains() hook
- Need worldspawn entity parsing for sky keys

## Porting Plan

### Phase 1: Core Infrastructure
1. Create `engine/hexen2/gl_sky.c` and `gl_sky.h`
2. Port data structures and globals
3. Implement `Sky_Init()` and cvar registration
4. Add `Sky_NewMap()` for worldspawn parsing

### Phase 2: Texture Loading
1. Port `Sky_LoadSkyBox()` function
2. Integrate with existing texture manager
3. Add TGA/PCX loading support for skyboxes
4. Implement fallback mechanisms

### Phase 3: Geometry Processing
1. Port polygon projection system
2. Implement clipping algorithm
3. Add `Sky_ProcessTextureChains()` integration
4. Hook into existing surface rendering

### Phase 4: Rendering
1. Implement `Sky_DrawSkyBox()` for cubemap
2. Keep existing scrolling sky as fallback
3. Add fog integration
4. Optimize face culling

### Phase 5: Testing & Polish
1. Test with various skybox packs
2. Performance profiling
3. Documentation
4. Map editor integration (Trenchbroom)

## Technical Challenges

### 1. Texture Manager Conflicts
From memory notes: QuakeSpasm uses advanced texture manager that conflicts with Hexen2's simpler system
- **Solution**: May need to adapt skybox code to use Hexen2's texture functions
- **Alternative**: Port minimal texture manager features needed for skyboxes

### 2. Rendering Pipeline Differences
Hexen2 vs Quake rendering may have architectural differences
- **Solution**: Careful integration testing
- **Hook points**: R_RenderView(), R_DrawWorld()

### 3. Memory Management
High-res skyboxes can use significant memory
- **Solution**: Implement texture size limits
- **Solution**: Add cvar for max skybox resolution

### 4. Backward Compatibility
Must maintain existing sky functionality
- **Solution**: Keep classic scrolling sky as fallback
- **Solution**: Make skybox system optional via cvar

## References

- QuakeSpasm source: https://github.com/sezero/quakespasm
- gl_sky.c: https://github.com/sezero/quakespasm/blob/master/Quake/gl_sky.c
- QuakeSpasm-Spiked: https://github.com/Shpoike/Quakespasm
- Ironwail fork: https://github.com/andrei-drexler/ironwail

## Next Steps

1. ✓ Complete research phase
2. Create gl_sky.c skeleton with data structures
3. Port Sky_Init() and basic infrastructure
4. Begin texture loading implementation
5. Test with simple skybox pack
