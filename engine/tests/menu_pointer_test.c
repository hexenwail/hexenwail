/*
 * Gate test for engine/h2shared/menu_pointer.h (issue #137): pointer x to
 * slider value, and scrollbar press/drag to scroll offset.
 */
#include <stdio.h>

#include "menu_pointer.h"

static int	failures;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "menu_pointer: " __VA_ARGS__); \
			fputc('\n', stderr); \
			failures++; \
		} \
	} while (0)

static int Near (float a, float b)
{
	float d = a > b ? a - b : b - a;
	return d < 0.0001f;
}

/* The slider is drawn at x = 220 by every options submenu. */
#define SX	220

/* Where M_DrawSlider puts the thumb's left edge for a fraction. */
static int ThumbLeft (float frac)
{
	return SX + (int)(MENU_SLIDER_TRAVEL * frac);
}

static void TestSliderFraction (void)
{
	int	i;

	CHECK(Near(MenuPointer_SliderFraction(SX + 4, SX), 0.0f), "thumb centre at 0 -> 0");
	CHECK(Near(MenuPointer_SliderFraction(SX + 4 + MENU_SLIDER_TRAVEL, SX), 1.0f),
	      "thumb centre at end -> 1");
	CHECK(Near(MenuPointer_SliderFraction(SX + 4 + MENU_SLIDER_TRAVEL / 2, SX), 0.5f),
	      "middle -> 0.5");
	CHECK(Near(MenuPointer_SliderFraction(0, SX), 0.0f), "far left clamps to 0");
	CHECK(Near(MenuPointer_SliderFraction(1000, SX), 1.0f), "far right clamps to 1");

	/* Grabbing the thumb where it is drawn must not move it: the pointer at
	 * the drawn thumb's centre maps back to (about) the same fraction. */
	for (i = 0; i <= 10; i++)
	{
		float f = i / 10.0f;
		float g = MenuPointer_SliderFraction(ThumbLeft(f) + 4, SX);
		CHECK(g > f - 1.0f / MENU_SLIDER_TRAVEL - 0.0001f && g < f + 0.0001f,
		      "grab at thumb %d%% moved it to %f", i * 10, g);
	}

	/* monotonic across the whole bar */
	for (i = SX - 20; i < SX + 120; i++)
		CHECK(MenuPointer_SliderFraction(i + 1, SX) >= MenuPointer_SliderFraction(i, SX),
		      "fraction not monotonic at x=%d", i);

	/* hit zone: caps plus slop, not the value text at x+100 */
	CHECK(MenuPointer_OnSlider(SX - 8, SX), "left cap is on the slider");
	CHECK(MenuPointer_OnSlider(SX + MENU_SLIDER_CELLS * 8 + 4, SX), "right cap is on the slider");
	CHECK(!MenuPointer_OnSlider(SX - 13, SX), "label column is not the slider");
	CHECK(!MenuPointer_OnSlider(SX + 100, SX), "value text is not the slider");
}

static void TestSliderValue (void)
{
	float	v;
	int	i;

	/* volume: 0..1, step 0.1 */
	CHECK(Near(MenuPointer_SliderValue(0.0f, 0, 1, 0, 1, 0.1f), 0.0f), "volume left");
	CHECK(Near(MenuPointer_SliderValue(1.0f, 0, 1, 0, 1, 0.1f), 1.0f), "volume right");
	CHECK(Near(MenuPointer_SliderValue(0.54f, 0, 1, 0, 1, 0.1f), 0.5f), "volume snaps down");
	CHECK(Near(MenuPointer_SliderValue(0.56f, 0, 1, 0, 1, 0.1f), 0.6f), "volume snaps up");

	/* gamma draws backwards: left end is 1.0, right end 0.3 */
	CHECK(Near(MenuPointer_SliderValue(0.0f, 1.0f, 0.3f, 0.3f, 1.0f, 0.05f), 1.0f), "gamma left");
	CHECK(Near(MenuPointer_SliderValue(1.0f, 1.0f, 0.3f, 0.3f, 1.0f, 0.05f), 0.3f), "gamma right");
	v = MenuPointer_SliderValue(0.5f, 1.0f, 0.3f, 0.3f, 1.0f, 0.05f);
	CHECK(Near(v, 0.65f), "gamma middle is 0.65, got %f", v);

	/* water alpha draws 0..1 but only accepts 0.7..1 */
	CHECK(Near(MenuPointer_SliderValue(0.2f, 0, 1, 0.7f, 1.0f, 0.05f), 0.7f), "wateralpha clamps low");
	CHECK(Near(MenuPointer_SliderValue(0.8f, 0, 1, 0.7f, 1.0f, 0.05f), 0.8f), "wateralpha in range");

	/* FOV 60..130, step 2: always an even integer inside the range */
	for (i = 0; i <= 100; i++)
	{
		v = MenuPointer_SliderValue(i / 100.0f, 60, 130, 60, 130, 2);
		CHECK(v >= 60 && v <= 130, "fov %f out of range", v);
		CHECK(Near(v / 2 - (int)(v / 2 + 0.5f), 0), "fov %f not on the key step", v);
	}

	/* no step: continuous */
	CHECK(Near(MenuPointer_SliderValue(0.25f, 0, 4, 0, 4, 0), 1.0f), "unsnapped");

	/* the full pipeline, pointer x -> cvar: dragging to the right cap of the
	 * sound volume gives 100%, to the left cap 0% */
	CHECK(Near(MenuPointer_SliderValue(MenuPointer_SliderFraction(SX + 84, SX), 0, 1, 0, 1, 0.1f), 1.0f),
	      "pointer at right cap -> full volume");
	CHECK(Near(MenuPointer_SliderValue(MenuPointer_SliderFraction(SX - 8, SX), 0, 1, 0, 1, 0.1f), 0.0f),
	      "pointer at left cap -> silence");
}

/* The mods menu's real numbers: list at y=60, 13 rows visible. */
#define TRACK_Y		60
#define VISIBLE		13

static void TestScrollbarLayout (void)
{
	menu_scrollbar_t	sb;

	CHECK(!MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, 0, VISIBLE, VISIBLE),
	      "a list that fits has no bar");
	CHECK(MenuScrollbar_HitTest(&sb, TRACK_Y + 4) == MENU_SCROLLBAR_MISS,
	      "no bar, nothing to hit");

	CHECK(MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, 0, 40, VISIBLE), "40 rows need a bar");
	CHECK(sb.thumb_y == TRACK_Y, "top of list, thumb at top");
	CHECK(MenuScrollbar_HitTest(&sb, TRACK_Y + 1) == MENU_SCROLLBAR_THUMB, "press on thumb");
	CHECK(MenuScrollbar_HitTest(&sb, TRACK_Y + VISIBLE * 8 - 1) == MENU_SCROLLBAR_BELOW,
	      "press below thumb");
	CHECK(MenuScrollbar_HitTest(&sb, TRACK_Y - 1) == MENU_SCROLLBAR_MISS, "above the track");
	CHECK(MenuScrollbar_HitTest(&sb, TRACK_Y + VISIBLE * 8) == MENU_SCROLLBAR_MISS,
	      "below the track");

	MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, 27, 40, VISIBLE);
	CHECK(MenuScrollbar_HitTest(&sb, TRACK_Y + 1) == MENU_SCROLLBAR_ABOVE, "press above thumb");
}

static void TestScrollbarHitMatchesDraw (void)
{
	menu_scrollbar_t	sb;
	int			count, top, y, cells;

	/* Every pixel of every cell the draw loop paints as thumb must hit-test
	 * as thumb, and every other track pixel as track: clickable == visible. */
	for (count = VISIBLE + 1; count <= 256; count += 7)
	{
		for (top = 0; top <= count - VISIBLE; top++)
		{
			MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8 + 8, top, count, VISIBLE);
			cells = 0;
			for (y = sb.track_y; y < sb.track_y + sb.track_h; y++)
			{
				int cell = sb.track_y + ((y - sb.track_y) / 8) * 8;
				int drawn = MenuScrollbar_CellIsThumb(&sb, cell);
				int hit = MenuScrollbar_HitTest(&sb, y);
				if (drawn != (hit == MENU_SCROLLBAR_THUMB))
				{
					CHECK(0, "count %d top %d y %d: drawn %d hit %d", count, top, y, drawn, hit);
					return;
				}
				cells += drawn;
			}
			CHECK(cells > 0, "count %d top %d: thumb drew no cell", count, top);
		}
	}
}

static void TestScrollbarDrag (void)
{
	menu_scrollbar_t	sb, after;
	int			count, top, prev, y;

	for (count = VISIBLE + 1; count <= 256; count += 5)
	{
		int max_top = count - VISIBLE;

		/* grab without moving: the thumb stays where it was drawn */
		for (top = 0; top <= max_top; top++)
		{
			int offset, t;

			MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, top, count, VISIBLE);
			for (offset = 0; offset < sb.thumb_h; offset += 3)
			{
				t = MenuScrollbar_DragTop(&sb, top, sb.thumb_y + offset, offset);
				if (t != top)
				{
					CHECK(0, "count %d top %d: grab at +%d scrolled to %d",
					      count, top, offset, t);
					return;
				}
			}
		}

		/* every thumb position the list can draw is reachable by drag */
		for (top = 0; top <= max_top; top++)
		{
			MenuScrollbar_Layout(&after, TRACK_Y, VISIBLE * 8, top, count, VISIBLE);
			MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, 0, count, VISIBLE);
			y = MenuScrollbar_DragTop(&sb, 0, after.thumb_y, 0);
			MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, y, count, VISIBLE);
			if (sb.thumb_y != after.thumb_y)
			{
				CHECK(0, "count %d: drag to thumb of top %d landed at %d", count, top, y);
				break;
			}
		}

		/* thumb bottom is flush with the track bottom at the end */
		MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, max_top, count, VISIBLE);
		CHECK(sb.thumb_y + sb.thumb_h == TRACK_Y + VISIBLE * 8,
		      "count %d: thumb not flush at the bottom", count);

		/* dragging far past either end pins to the ends */
		MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, 0, count, VISIBLE);
		CHECK(MenuScrollbar_DragTop(&sb, 0, -500, 2) == 0, "drag off the top -> 0");
		CHECK(MenuScrollbar_DragTop(&sb, 0, 5000, 2) == max_top,
		      "count %d: drag off the bottom -> %d", count, max_top);

		/* monotonic: moving the pointer down never scrolls up */
		prev = 0;
		for (y = TRACK_Y - 20; y < TRACK_Y + VISIBLE * 8 + 20; y++)
		{
			int t = MenuScrollbar_DragTop(&sb, prev, y, 4);
			CHECK(t >= prev, "count %d: drag not monotonic at y=%d", count, y);
			prev = t;
		}
	}
}

static void TestScrollbarPage (void)
{
	menu_scrollbar_t	sb;

	MenuScrollbar_Layout(&sb, TRACK_Y, VISIBLE * 8, 10, 40, VISIBLE);
	CHECK(MenuScrollbar_PageTop(&sb, 10, MENU_SCROLLBAR_BELOW) == 23, "page down one screen");
	CHECK(MenuScrollbar_PageTop(&sb, 23, MENU_SCROLLBAR_BELOW) == 27, "page down clamps to end");
	CHECK(MenuScrollbar_PageTop(&sb, 10, MENU_SCROLLBAR_ABOVE) == 0, "page up clamps to 0");
	CHECK(MenuScrollbar_PageTop(&sb, 20, MENU_SCROLLBAR_ABOVE) == 7, "page up one screen");
	CHECK(MenuScrollbar_PageTop(&sb, 20, MENU_SCROLLBAR_THUMB) == 20, "thumb is not a page");
}

int main (void)
{
	TestSliderFraction();
	TestSliderValue();
	TestScrollbarLayout();
	TestScrollbarHitMatchesDraw();
	TestScrollbarDrag();
	TestScrollbarPage();

	if (failures)
	{
		fprintf(stderr, "menu_pointer: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
