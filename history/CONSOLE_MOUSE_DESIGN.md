# Console Mouse Support Design

**Bead:** uhexen2-8vw0  
**Reference:** Ironwail console.c (commits with mouse support)  
**Scope:** Full mouse interaction for in-game console

## Overview

Port Ironwail's console mouse support to Hexenwail. Enable clicking URLs in console output to launch browser, drag-to-select text, and Ctrl+C to copy selection to clipboard.

## Architecture

### 1. State Tracking (console_t extension)

```c
typedef enum {
    CMS_NOTPRESSED = 0,  // Mouse up, no selection
    CMS_PRESSED = 1,     // Mouse down, selection starting
    CMS_DRAGGING = 2,    // Actively selecting by drag
} con_mousestate_t;

typedef struct {
    int line;   // Console line number
    int col;    // Column within line (character index)
} con_offset_t;

typedef struct {
    con_offset_t begin, end;  // Selection range
} con_selection_t;
```

**Add to console_t:**
- `con_mousestate_t mouse_state`
- `con_selection_t selection`
- `con_offset_t mouse_hotlink` (for URL detection)
- `int mouse_scroll_delta` (for edge auto-scroll)

### 2. Input Handling

**New functions in console.c:**

```c
// Called from IN_ProcessMouse or similar
void Con_Mousemove(int x, int y)
  - If mouse_state == NOTPRESSED:
    - Detect hotlink at (x,y)
    - Set cursor (HAND if link, IBEAM if text, DEFAULT otherwise)
  - Else:
    - Convert screen coords to console char offset
    - Update selection.end
    - Detect edge scroll region (bottom 8 rows)
    - Set scroll speed if near edge

void Con_MouseButton(qboolean pressed)
  - If pressed: 
    - Get current mouse position
    - Store as selection.begin
    - Set mouse_state = PRESSED
  - Else:
    - Set mouse_state = NOTPRESSED (or IDLE)
    - Keep selection for Ctrl+C

void Con_CopySelection(void)
  - If selection is empty: return false
  - Extract text range from console buffer
  - Convert to UTF-8
  - SDL_SetClipboardText()
  - Play feedback sound: "misc/menu2.wav" (or Hexen II equivalent)
  - Return true

void Con_SelectAll(void)
  - Set selection to entire console buffer
  - selection.begin = {0, 0}
  - selection.end = {con->current, 0}
```

### 3. URL Detection

**New functions:**

```c
qboolean Con_IsURLChar(char c)
  - Return true for: [a-zA-Z0-9_./-:?#@!$&'()*+,;=%]

qboolean Con_ParseURL(const char *text, int *start, int *end)
  - Look for "http://" or "https://"
  - Extend boundaries while URL chars present
  - Return true if found
  - Set start/end indices

qboolean Con_GetLinkAtOffset(con_offset_t ofs, char **url_out)
  - Get line text at ofs.line
  - Check if ofs.col is within a URL
  - If yes, extract URL and return true
```

### 4. Rendering

**Modify Con_DrawConsole:**

```c
// Draw selection highlight (before text)
if (con->mouse_state == CMS_DRAGGING || has_selection)
    Con_DrawSelection()
    
// Draw URLs with different color (during text draw)
Con_DrawCharacterWithLinks()
    - Detect if char is part of URL
    - Draw with color (e.g., cyan) and underline

// Draw cursor feedback
if (con->mouse_state == CMS_PRESSED)
    Con_DrawCopyPrompt("Ctrl+C to copy")
```

### 5. Coordinate Conversion

**New functions:**

```c
qboolean Con_ScreenToOffset(int sx, int sy, con_offset_t *out, int mode)
  - Convert pixel (sx, sy) to console line/col
  - Mode: CT_INSIDE (within bounds), CT_NEAREST (clamp), CT_OUTSIDE (allow overflow)
  - Return true if within console area
  
  // Calculation:
  // Screen layout:
  //   sy = 0 .. (con_vislines - 30): scrollback area
  //   sy = (con_vislines - 22) .. con_vislines: input line
  // Each char = 8 pixels (character width)
  // Each row = 8 pixels (line height)
  
void Con_OffsetToScreen(con_offset_t ofs, int *sx_out, int *sy_out)
  - Reverse of above
```

### 6. Auto-Scroll During Drag

**In Con_Mousemove when dragging:**

```c
// If mouse near bottom, scroll down
// If mouse near top, scroll up (within limits)
// Quadratic easing: speed = -frac^2 * MAX_SPEED
// Scroll every frame if con_scrolldelta > 0
```

## Implementation Phases

### Phase 1: State & Input (1 hour)
- Add mouse state enum and selection struct to console.h
- Implement Con_Mousemove() and Con_MouseButton()
- Wire mouse events in key.c / in_*.c

### Phase 2: Selection & Clipboard (1 hour)
- Implement Con_SelectAll(), Con_CopySelection()
- Add SDL clipboard integration
- Add feedback sound on copy

### Phase 3: URL Detection & Rendering (1.5 hours)
- Implement URL parsing functions
- Modify Con_DrawConsole to highlight URLs
- Add cursor changes (hand/I-beam)

### Phase 4: Browser Launch (0.5 hours)
- Detect click on URL
- Call SDL_OpenURL() to launch browser
- Test with common protocols (http, https)

### Phase 5: Polish (0.5 hours)
- Auto-scroll edge detection
- Selection highlight color/style
- Edge cases (empty selection, scrollback wrap)

## Dependencies

- SDL3: `SDL_SetClipboardText()`, `SDL_OpenURL()`, `SDL_SetMouseCursor()`
- Existing: `Draw_Character()`, console buffer format
- Audio: SFX for copy feedback (reuse existing sound or add "ui/copy.wav")

## Testing

- [ ] Click URL in console → browser opens
- [ ] Drag select text → highlighted correctly
- [ ] Ctrl+C with selection → clipboard contains text
- [ ] Click with no selection → no copy
- [ ] Select across scrollback wrap point → text joins correctly
- [ ] Auto-scroll when dragging near edges
- [ ] Mouse leaves console window → state reset

## Notes

- Hexen II console has 16384-char buffer vs Ironwail's dynamic (Quake is smaller)
- Character width is fixed 8 pixels (same as Ironwail)
- Console output format uses shorts (character codes), same as Ironwail
- Need to handle UTF-8 conversion for non-ASCII copied text
