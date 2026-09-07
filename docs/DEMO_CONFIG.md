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

**mydemostart.cfg**:
```
fov 120
host_maxfps 60
hide_hud 1
gamma 0.8
```

**mydemoend.cfg**:
```
fov 90
host_maxfps 0
hide_hud 0
gamma 1.0
```

### Scripted Sequences

Execute console commands before/after demos:

**introstart.cfg**:
```
// Pre-demo setup
bgmvolume 0.5
```

**introend.cfg**:
```
// Post-demo cleanup
bgmvolume 1.0
```

## Available CVars

- `hide_hud` — Hide status bar and HUD elements (0=show, 1=hide, default 0)
- `scr_demobar_timeout` — Playback bar idle timeout, in seconds (default 1).
  Negative hides the bar entirely, 0 pins it up for the whole demo.
- Standard cvars: `fov`, `host_maxfps`, `gamma`, `contrast`, `bgmvolume`, `volume`, etc.

## The Playback Bar

During playback the engine draws a bar above the status bar showing the demo
name, the play/pause state, how far through the file playback has reached, and
the elapsed map time. It appears when the demo starts and whenever input
reaches the demo -- a mouse click, say -- then fades out after
`scr_demobar_timeout` seconds of no interaction.

Menu and console keys deliberately do not count. During playback most keys open
the main menu, so counting them would pop the bar up underneath the menu and
re-arm it on every keystroke of the navigation that follows.

Position is derived from the demo file offset, not from a time index, so it is
an approximation: a stretch of the demo where little happens produces few bytes
and the cursor crawls through it. A `.dem` header carries no duration, and
finding one would mean reading the whole file before playback could start.

For clean cutscene footage, suppress it alongside the HUD:

**t9start.cfg**:
```
hide_hud 1
scr_demobar_timeout -1
```

**t9end.cfg**:
```
hide_hud 0
scr_demobar_timeout 1
```

## File Location

Put the config beside the demo, on the engine's search path — in practice your
**gamedir**: `data1/` for the base game, `portals/` for Portals of Praevus, or
the mod's own directory. The engine resolves it the same way it resolves any
other file, so a loose file in your gamedir wins over a packed one.

The paths under `gamecode/res/` are **repository sources**, not runtime
locations: they are where the shipped configs are authored before packaging.
`gamecode/res/portals/` holds the only pair that ships (`t9start.cfg` and
`t9end.cfg`); `gamecode/res/h2/` contains no demo configs at all. Do not tell a
player to put files there.

## Example: Clean Demo Footage

To record a demo without HUD:

1. Create `mydemostart.cfg`:
   ```
   hide_hud 1
   ```

2. Create `mydemoend.cfg`:
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

For more information, see `engine/hexen2/cl_demo.c` (the start/end config
lookup), `engine/hexen2/sbar.c :: hide_hud`, and — for the playback bar
documented above — `engine/h2shared/gl_screen.c :: SCR_DrawDemoBar()`.
