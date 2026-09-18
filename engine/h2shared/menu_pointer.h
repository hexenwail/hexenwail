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
