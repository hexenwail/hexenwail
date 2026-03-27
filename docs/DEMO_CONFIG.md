# Demo Configuration Files

Hexenwail supports per-demo configuration files that allow modders and content creators to customize engine behavior for specific demos without modifying the engine itself.

## How It Works

When a demo named `DEMONAME.dem` is played, the engine automatically executes config files in this order:

1. **Start**: `DEMONAMEstart.cfg` (before demo playback begins)
2. **Demo plays**
3. **End**: `DEMONAMEend.cfg` (after demo playback ends)

Config files are optional—if they don't exist, no error occurs. The `exec` command silently succeeds.

## Use Cases

### Hide HUD for Cutscene Demos

For cinematic demos (e.g., machinima), use the `hide_hud` cvar to show clean footage:

**t9start.cfg** (mission pack intro):
```
hide_hud 1
```

**t9end.cfg**:
```
hide_hud 0
```

When you run `playdemo t9`, the HUD is hidden during playback and restored afterward.

### Per-Demo Settings

Configure any cvar per-demo:

**mydemo_start.cfg**:
```
fov 120
r_maxfps 60
hide_hud 1
gamma 0.8
```

**mydemo_end.cfg**:
```
fov 90
r_maxfps 0
hide_hud 0
gamma 1.0
```

### Scripted Sequences

Execute console commands before/after demos:

**intro_start.cfg**:
```
// Pre-demo setup
volumeRGB_UI 0.5
cd_music 0
```

**intro_end.cfg**:
```
// Post-demo cleanup
volumeRGB_UI 1.0
cd_music 1
```

## Available CVars

- `hide_hud` — Hide status bar and HUD elements (0=show, 1=hide, default 0)
- Standard cvars: `fov`, `r_maxfps`, `gamma`, `brightness`, etc.

## File Location

Place config files in the same directory as the demo:

- **Hexen II**: `gamecode/res/h2/`
- **Hexen II: Portals**: `gamecode/res/portals/`
- **Other mods**: Appropriate mod data directory

## Example: Clean Demo Footage

To record a demo without HUD:

1. Create `mydemo_start.cfg`:
   ```
   hide_hud 1
   ```

2. Create `mydemo_end.cfg`:
   ```
   hide_hud 0
   ```

3. Record or play the demo:
   ```
   playdemo mydemo
   ```

The HUD will be hidden during playback and automatically restored.

## Notes

- Config files execute in the context of `src_command` (same as typing in console)
- Demo name matching is case-insensitive on case-insensitive filesystems
- If both start and end configs don't exist, the system gracefully handles this
- Settings are not archived—they reset when the engine restarts

For more information, see `engine/hexen2/cl_demo.c` and `engine/hexen2/sbar.c`.
