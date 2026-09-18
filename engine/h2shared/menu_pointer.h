/*
 * menu_pointer.h -- pointer geometry for menu sliders and scrollbars
 *
 * Pure arithmetic, no engine state: menu.c owns the cvars, the cursor and the
 * grab, and asks this file only "where on the widget is the pointer, and what
 * value or scroll offset does that mean".  Kept apart so the mapping can be
 * gate-tested without SDL or a renderer (engine/tests/menu_pointer_test.c).
 *
 * All coordinates are menu-canvas units (the 320-wide CANVAS_MENU space).
 */
#ifndef H2_MENU_POINTER_H
#define H2_MENU_POINTER_H

/* in_sdl.c: bit (b - 1) of menu_mouse_buttons is SDL mouse button b. */
#define MENU_MOUSE_BUTTON(b)	(1 << ((b) - 1))
#define MENU_MOUSE_LEFT		MENU_MOUSE_BUTTON(1)

/* ---------------------------------------------------------------------------
 * Pointer -> framebuffer pixels.  SDL3 reports the mouse in window POINTS;
 * every window is created SDL_WINDOW_HIGH_PIXEL_DENSITY (gl_vidsdl.c,
 * vid_soft_web.c), so on a 2x display the framebuffer has twice as many
 * pixels as the pointer has points.  Per axis, from the two sizes SDL gives
 * (SDL_GetWindowSize / SDL_GetWindowSizeInPixels), rather than one density
 * number, so a non-uniform ratio cannot skew one axis.
 * ------------------------------------------------------------------------- */
static inline float MenuPointer_PointsToPixels (float points, int size_points,
						int size_pixels)
{
	if (size_points <= 0 || size_pixels <= 0)
		return points;
	return points * (float)size_pixels / (float)size_points;
}

/*
 * Undo a letterboxed upscale: the software web renderer draws a small
 * framebuffer (fb_size) and the presenter stretches it into dest_size device
 * pixels at dest_off (VID_DestRect in vid_soft_web.c).  Device pixel -> the
 * framebuffer pixel under it.  Outside the image clamps to the edge.
 */
static inline float MenuPointer_UnLetterbox (float device, int dest_off,
					     int dest_size, int fb_size)
{
	float	f;

	if (dest_size <= 0 || fb_size <= 0)
		return device;
	f = (device - (float)dest_off) * (float)fb_size / (float)dest_size;
	if (f < 0.0f)
		f = 0.0f;
	if (f > (float)fb_size - 1.0f)
		f = (float)fb_size - 1.0f;
	return f;
}

/* ---------------------------------------------------------------------------
 * Framebuffer pixel -> menu canvas unit.  The exact inverse of CANVAS_MENU in
 * GL_SetCanvas (gl_draw.c):
 *
 *	s = SCR_CalcUIScale (&scr_menuscale);
 *	s = q_min (s, (float)gw / 320.0f);	// width is the ONLY clamp
 *	w = (int)(320.0f * s * px);		// px = glwidth / gw
 *	h = glheight;
 *	glViewport (glx + (glwidth - w) / 2, gly, w, h);
 *	GL_Ortho (0, 320, (float)gh / s, 0, ...);
 *
 * gw/gh is SCR_GuiSize, the framebuffer squashed by scr_pixelaspect, so a
 * "Stretched" 2D aspect makes canvas units taller than they are wide.  The
 * software renderer fits the same formula with s = 1, gw = glwidth,
 * gh = glheight: its canvas sits at (vid.width - 320) / 2, unscaled
 * (draw_soft_web.c GL_SetCanvas).  glx/gly are 0 in both backends.
 * ------------------------------------------------------------------------- */
typedef struct
{
	int	glwidth, glheight;	/* framebuffer, pixels */
	int	gw, gh;			/* SCR_GuiSize */
	float	scale;			/* SCR_CalcUIScale (&scr_menuscale), unclamped */
} menu_canvas_t;

static inline int MenuPointer_Floor (float v)
{
	int	i = (int)v;
	return ((float)i > v) ? i - 1 : i;
}

static inline void MenuPointer_CanvasFrame (const menu_canvas_t *c, float *s,
					    int *left, int *w)
{
	float	px;

	*s = c->scale;
	if (c->gw > 0 && *s > (float)c->gw / 320.0f)
		*s = (float)c->gw / 320.0f;
	if (*s <= 0.0f)
		*s = 1.0f;
	px = c->gw > 0 ? (float)c->glwidth / (float)c->gw : 1.0f;
	*w = (int)(320.0f * *s * px);
	if (*w < 1)
		*w = 1;
	*left = (c->glwidth - *w) / 2;
}

static inline int MenuPointer_CanvasX (const menu_canvas_t *c, float fb_x)
{
	float	s;
	int	left, w;

	MenuPointer_CanvasFrame (c, &s, &left, &w);
	return MenuPointer_Floor ((fb_x - (float)left) * 320.0f / (float)w);
}

static inline int MenuPointer_CanvasY (const menu_canvas_t *c, float fb_y)
{
	float	s;
	int	left, w;

	MenuPointer_CanvasFrame (c, &s, &left, &w);
	if (c->glheight <= 0)
		return MenuPointer_Floor (fb_y);
	return MenuPointer_Floor (fb_y * ((float)c->gh / s) / (float)c->glheight);
}

/* ---------------------------------------------------------------------------
 * Sliders.  M_DrawSlider draws a left cap at x-8, MENU_SLIDER_CELLS bar cells
 * from x, a right cap after them, and the 8-wide thumb at
 * x + (MENU_SLIDER_CELLS-1)*8 * fraction.  The pointer maps to the fraction
 * that puts the thumb's CENTRE under it, so grabbing the thumb does not jump.
 * ------------------------------------------------------------------------- */
#define MENU_SLIDER_CELLS	10
#define MENU_SLIDER_TRAVEL	((MENU_SLIDER_CELLS - 1) * 8)

/* True when canvas x is over the drawn slider, caps included, with 4 units of
 * slop either side (Ironwail's M_SliderClick uses the same zone). */
static inline int MenuPointer_OnSlider (int cx, int slider_x)
{
	return cx >= slider_x - 12 && cx <= slider_x + MENU_SLIDER_CELLS * 8 + 4;
}

/*
 * Is the bar drawn at all?  Some rows print "Off" instead of a bar once their
 * value reaches the bottom (liquid warp, flash, motion blur, gun FOV scale).
 * A press there must stay Enter, which steps the value up: claimed as a
 * slider press it would map to the bottom, change nothing, and swallow the
 * one input that used to turn the effect back on.
 */
static inline int MenuPointer_SliderBarDrawn (float value, float vmin, int off_at_min)
{
	return !off_at_min || value > vmin;
}

static inline float MenuPointer_SliderFraction (int cx, int slider_x)
{
	float	f = (float)(cx - slider_x - 4) / (float)MENU_SLIDER_TRAVEL;

	if (f < 0.0f)
		return 0.0f;
	if (f > 1.0f)
		return 1.0f;
	return f;
}

/*
 * Fraction -> cvar value.  v0/v1 are the values the DRAW code puts at the left
 * and right ends of the bar -- which is not always the legal range: water
 * alpha draws 0..1 but only accepts 0.7..1, gamma draws backwards.  The result
 * is clamped to [vmin, vmax] and snapped to the arrow-key step so a drag lands
 * on the same values the keyboard does.  step <= 0 disables snapping.
 */
static inline float MenuPointer_SliderValue (float frac, float v0, float v1,
					     float vmin, float vmax, float step)
{
	float	v = v0 + frac * (v1 - v0);

	if (v < vmin)
		v = vmin;
	if (v > vmax)
		v = vmax;
	if (step > 0.0f)
	{
		int n = (int)((v - vmin) / step + 0.5f);	/* v >= vmin here */
		v = vmin + (float)n * step;
		if (v > vmax)
			v = vmax;
	}
	return v;
}

/* ---------------------------------------------------------------------------
 * Proportional scrollbar, drawn as a column of 8-unit cells over
 * [track_y, track_y + track_h).  A cell is thumb when its top edge lies in
 * [thumb_y, thumb_y + thumb_h) -- the draw loop and the hit test both ask
 * MenuScrollbar_CellIsThumb, so what is clickable is what is visible.
 * ------------------------------------------------------------------------- */
typedef struct
{
	int	track_y, track_h;
	int	thumb_y, thumb_h;
	int	count, visible;		/* rows in the list, rows on screen */
} menu_scrollbar_t;

enum
{
	MENU_SCROLLBAR_MISS = 0,
	MENU_SCROLLBAR_ABOVE,	/* track above the thumb: page up */
	MENU_SCROLLBAR_THUMB,
	MENU_SCROLLBAR_BELOW	/* track below the thumb: page down */
};

static inline int MenuScrollbar_MaxTop (int count, int visible)
{
	return count > visible ? count - visible : 0;
}

static inline int MenuScrollbar_ClampTop (int top, int count, int visible)
{
	int	max_top = MenuScrollbar_MaxTop (count, visible);

	if (top > max_top)
		top = max_top;
	if (top < 0)
		top = 0;
	return top;
}

/*
 * Thumb top for scroll offset `top'.  Spread over the travel left once the
 * thumb's own height is taken out, so the last offset puts the thumb flush
 * with the bottom of the track.  The older `top * track_h / count' only did
 * that while the thumb was its proportional size: once it was held at the
 * 8-unit minimum (a long list) its bottom rows ran past the track and the
 * draw loop, which stops at the track, drew no thumb at all.
 */
static inline int MenuScrollbar_ThumbY (const menu_scrollbar_t *sb, int top)
{
	int	max_top = MenuScrollbar_MaxTop (sb->count, sb->visible);
	int	travel = sb->track_h - sb->thumb_h;

	if (max_top <= 0 || travel <= 0)
		return sb->track_y;
	top = MenuScrollbar_ClampTop (top, sb->count, sb->visible);
	return sb->track_y + (top * travel) / max_top;
}

/* Returns 0 (and leaves the thumb empty) when everything fits: no bar. */
static inline int MenuScrollbar_Layout (menu_scrollbar_t *sb, int track_y,
					int track_h, int top, int count, int visible)
{
	sb->track_y = track_y;
	sb->track_h = track_h;
	sb->count = count;
	sb->visible = visible;
	sb->thumb_y = track_y;
	sb->thumb_h = 0;

	if (count <= visible || count <= 0 || track_h <= 0)
		return 0;

	sb->thumb_h = (visible * track_h) / count;
	if (sb->thumb_h < 8)
		sb->thumb_h = 8;
	if (sb->thumb_h > track_h)
		sb->thumb_h = track_h;
	sb->thumb_y = MenuScrollbar_ThumbY (sb, top);
	return 1;
}

static inline int MenuScrollbar_CellIsThumb (const menu_scrollbar_t *sb, int cell_y)
{
	return cell_y >= sb->thumb_y && cell_y < sb->thumb_y + sb->thumb_h;
}

/* Which part of the bar is under canvas y.  The x test is the caller's: the
 * bar's column is a layout detail of each menu. */
static inline int MenuScrollbar_HitTest (const menu_scrollbar_t *sb, int y)
{
	int	cell_y;

	if (sb->thumb_h <= 0 || y < sb->track_y || y >= sb->track_y + sb->track_h)
		return MENU_SCROLLBAR_MISS;

	cell_y = sb->track_y + ((y - sb->track_y) / 8) * 8;
	if (MenuScrollbar_CellIsThumb (sb, cell_y))
		return MENU_SCROLLBAR_THUMB;
	return cell_y < sb->thumb_y ? MENU_SCROLLBAR_ABOVE : MENU_SCROLLBAR_BELOW;
}

/*
 * Thumb drag -> scroll offset.  grab_offset is pointer_y - thumb_y at the
 * moment of the press, so the thumb keeps its grip point under the pointer
 * instead of snapping its top edge to it.  Returns the offset whose thumb
 * lands nearest the target.  When a list is longer than the track has pixels
 * several offsets share one thumb position; cur_top wins that tie, so a press
 * or a sub-pixel wobble never scrolls the list under a thumb that stayed put.
 */
static inline int MenuScrollbar_DragTop (const menu_scrollbar_t *sb, int cur_top,
					 int pointer_y, int grab_offset)
{
	int	max_top = MenuScrollbar_MaxTop (sb->count, sb->visible);
	int	travel = sb->track_h - sb->thumb_h;
	int	target, top, best, best_d, t;

	if (sb->thumb_h <= 0 || max_top <= 0 || travel <= 0)
		return 0;

	target = pointer_y - grab_offset;
	if (target < sb->track_y)
		target = sb->track_y;
	if (target > sb->track_y + travel)
		target = sb->track_y + travel;

	cur_top = MenuScrollbar_ClampTop (cur_top, sb->count, sb->visible);
	if (MenuScrollbar_ThumbY (sb, cur_top) == target)
		return cur_top;

	/* the rounded inverse, then settle on the nearest neighbour: integer
	 * division in ThumbY can leave the inverse one row off */
	top = ((target - sb->track_y) * max_top + travel / 2) / travel;
	top = MenuScrollbar_ClampTop (top, sb->count, sb->visible);
	best = top;
	best_d = -1;
	for (t = top - 1; t <= top + 1; t++)
	{
		int d;
		if (t < 0 || t > max_top)
			continue;
		d = MenuScrollbar_ThumbY (sb, t) - target;
		if (d < 0)
			d = -d;
		if (best_d < 0 || d < best_d)
		{
			best = t;
			best_d = d;
		}
	}
	return best;
}

/* A click on the track moves one page (a screenful) toward the click. */
static inline int MenuScrollbar_PageTop (const menu_scrollbar_t *sb, int top, int hit)
{
	if (hit == MENU_SCROLLBAR_ABOVE)
		top -= sb->visible;
	else if (hit == MENU_SCROLLBAR_BELOW)
		top += sb->visible;
	return MenuScrollbar_ClampTop (top, sb->count, sb->visible);
}

#endif /* H2_MENU_POINTER_H */
