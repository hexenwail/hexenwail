#!/usr/bin/env python3
"""Regression test for the menu's yes/no prompts (#316, #343 and its follow-up).

The menus used to ask y/n through SCR_ModalMessage, which waits for a key by
blocking.  A browser delivers keys only between frames, so the wait spun inside
one wasm frame forever.  The prompts are a menu state now (m_confirm), answered
by M_ConfirmKey -- and that put them behind M_Keydown, which turns every left
click into K_ENTER.  M_ConfirmKey takes K_ENTER as yes, so one click anywhere
on the screen started a new game over the running one, or deleted a save.  The
quit prompt (m_quit, M_Quit_Key) had the same hole.

A click on either prompt now answers only when it lands on an answer row, only
once the pointer has moved since the prompt went up, and the click after an
answer is swallowed for CLICK_LOCKOUT seconds.

Part one compiles the engine's own M_Confirm, M_ConfirmDraw, M_ConfirmKey,
M_Quit_Draw, M_Quit_Key, the M_Prompt_* and M_Confirm_*Key helpers and the
click block of M_Keydown out of menu.c, with the draw calls recorded instead of
drawn, and sweeps every point of the menu canvas:

  * between a prompt going up and its first draw no point answers, whatever
    rows the previous prompt left behind;
  * once drawn, a pointer click is yes exactly on the row where the yes hint
    was printed, no exactly on the row where the no hint was, across the width
    the prompt gives them, and nothing anywhere else;
  * a prompt raised under a resting pointer does not highlight or answer there
    until the pointer has moved, and then it does;
  * a click with no pointer (ui_mouse off) answers nowhere;
  * the web touch overlay's K_MOUSE1 is yes, as gamepad A is;
  * the row under the pointer, and only that row, changes face;
  * after a pointer answer the next pointer clicks are dropped until
    CLICK_LOCKOUT has passed; touch and ui_mouse-off clicks are not;
  * y, Y and K_ENTER are yes; n, N and K_ESCAPE are no; no other key answers.

Part two reads the source for the wiring the harness cannot reach, because
M_Keydown dispatches to every menu in the game and cannot be compiled whole:

  * no menu code calls SCR_ModalMessage;
  * M_Keydown opens with the click block, and the block and the remap chain
    after it are exactly what the harness compiled;
  * each prompt function and state variable has the uses it should, and no
    macro stands over them;
  * menu_touch_key is raised by Hexenwail_TouchKey around its Key_Event and by
    nothing else, so a real click can never pass for a touch;
  * the mouse button bit is set in one place, behind ui_mouse.

Comments, `//` lines and `#if 0` blocks are stripped first, so disabled code
does not satisfy a check.  The wiring checks compare exact text on purpose: a
reformat of those few lines fails here and is fixed here, which is cheaper than
a gate that a reordering can slip past.
"""
import os
import re
import subprocess
import tempfile

from test_multiplayer_transparency import ROOT, function

MENU = "engine/hexen2/menu.c"
INPUT = "engine/h2shared/in_sdl.c"
POINTER = "engine/h2shared/menu_pointer.h"
# Any C assignment operator, and not a comparison.
ASSIGN = r"(?:[-+*/%&|^]|<<|>>)?=(?!=)"

PRELUDE = r"""
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef int qboolean;
#define true 1
#define false 0
#define K_ENTER 13
#define K_ESCAPE 27
#define K_MOUSE1 200
#define MENU_MOUSE_LEFT 1
#define HW_VERSION "0.0"
#define GAME_PORTALS 1
#define MAX_LINES2 1
#define MAX_LINES2_MP 1
typedef enum { CONFIRM_NONE = 0, CONFIRM_DELETE_S } confirm_action_t;
enum m_state_e { m_none, m_load, m_confirm, m_quit };
enum { key_game };
typedef struct { int unused; } qpic_t;
static confirm_action_t m_confirm_action;
static enum m_state_e m_confirm_prevstate, m_quit_prevstate, m_state;
static qboolean m_entersound, m_recursiveDraw, wasInMenus, LineTxt2, SoundPlayed;
static qboolean menu_touch_key;
static int load_cursor, m_confirm_cursor;
static int menu_mouse_buttons, menu_mouse_x, menu_mouse_y;
static const char *m_confirm_text;
static int m_confirm_hint_x0, m_confirm_hint_x1, m_confirm_hint_y = -1;
static unsigned int menu_mouse_motions, m_prompt_motions;
static double realtime, m_click_lockout, host_frametime;
static float LinePos;
static int LineTimes, MaxLines;
static unsigned gameflags;
static const char *no_lines[1] = { "" };
static const char **LineText = no_lines;
static const char *Credit2Text[MAX_LINES2] = { "" };
static const char *Credit2TextMP[MAX_LINES2_MP] = { "" };
static struct { int integer; } ui_mouse = { 1 };

static int deleted, quits, to_game, press_claims;
static void M_ConfirmAction (confirm_action_t action) { if (action == CONFIRM_DELETE_S) deleted++; }
static void Host_Quit_f (void) { quits++; }
static void Key_SetDest (int dest) { assert (dest == key_game); to_game++; }
static void Sbar_Changed (void) {}
/* The canvas the draw calls land in.  A click is tested in menu canvas units,
 * so every prompt draw must be in the menu canvas.  M_Draw here is the menu
 * under the quit prompt being drawn, and it leaves the way the real one does:
 * check_wiring pins that tail. */
enum { CANVAS_DEFAULT, CANVAS_MENU };
static int canvas;
static qboolean m_canvas_active;
static void GL_SetCanvas (int c) { canvas = c; }
static void M_Draw (void) { m_canvas_active = false; GL_SetCanvas (CANVAS_DEFAULT); }
static void BGM_PlayCDtrack (int track, qboolean looping) { (void)track; (void)looping; }
static void S_LocalSound (const char *name) { (void)name; }
static qpic_t a_pic;
static qpic_t *Draw_CachePic (const char *name) { (void)name; return &a_pic; }
static void M_DrawPic (int x, int y, qpic_t *pic) { (void)x; (void)y; (void)pic; }
/* The pointer layer (slider bars, scrollbars) is offered every pointer press. */
static qboolean M_MousePress (void) { return press_claims; }

/* The harness canvas is the framebuffer: one unit, one pixel. */
static int M_PointerCanvasX (void) { return menu_mouse_x; }
static int M_PointerCanvasY (void) { return menu_mouse_y; }
/* What the prompt put on screen, recorded instead of drawn. */
static int box_n, box_x, box_y, box_w, box_lines;
static void M_DrawTextBox (int x, int y, int width, int lines)
{
    assert (canvas == CANVAS_MENU && m_canvas_active);
    box_n++; box_x = x; box_y = y; box_w = width; box_lines = lines;
}
static struct { int white, x, y; char text[80]; } printed[16];
static int nprinted;
static void record (int white, int cx, int cy, const char *str)
{
    assert (canvas == CANVAS_MENU && m_canvas_active);
    assert (nprinted < 16 && strlen (str) < 80);
    printed[nprinted].white = white; printed[nprinted].x = cx; printed[nprinted].y = cy;
    strcpy (printed[nprinted].text, str);
    nprinted++;
}
static void M_Print (int cx, int cy, const char *str) { record (0, cx, cy, str); }
static void M_PrintWhite (int cx, int cy, const char *str) { record (1, cx, cy, str); }
"""

# M_Keydown's click block, lifted out whole: `return;` leaves out_key at 0,
# falling off the end hands the key on.
CLICK_BLOCK = r"""
static int out_key;
static void click_block (int key, qboolean repeat)
{
    out_key = 0;
%s
    out_key = key;
}
"""

TEST = r"""
/* One K_MOUSE1 as M_Keydown sees it.  Returns the key it hands on, 0 if it
 * drops the click. */
static int mouse1 (qboolean touch, qboolean pointer)
{
    menu_touch_key = touch;
    menu_mouse_buttons = pointer ? MENU_MOUSE_LEFT : 0;
    click_block (K_MOUSE1, false);
    menu_touch_key = false;
    menu_mouse_buttons = 0;
    return out_key;
}

enum { DELETE, QUIT };
static int kind;

/* The pointer as in_sdl.c feeds it (check_wiring pins both halves).  A motion
 * event while the menu owns input moves it and is counted, if it went
 * anywhere.  A motion event while the game owns input, or a click, sets the
 * position and is not counted: the pointer is merely found there. */
static void move (int x, int y)
{
    if (x != menu_mouse_x || y != menu_mouse_y)
    {
        menu_mouse_x = x; menu_mouse_y = y;
        menu_mouse_motions++;
    }
}
static void found (int x, int y) { menu_mouse_x = x; menu_mouse_y = y; }

static void redraw (void)
{
    box_n = nprinted = 0;
    /* as M_Draw does before it dispatches on m_state */
    GL_SetCanvas (CANVAS_MENU);
    m_canvas_active = true;
    if (kind == DELETE) M_ConfirmDraw (); else M_Quit_Draw ();
    assert (box_n == 1);
    assert (canvas == CANVAS_MENU && m_canvas_active);
}

static void draw (int px, int py)
{
    move (px, py);
    redraw ();
}

/* Put the prompt back up without reopening it (so the aim is kept), and hand
 * it one key.  Returns 1 if the answer was yes (save deleted, or quit), 0 if
 * the prompt is still up, -1 if it was cancelled. */
static int answer (int key)
{
    deleted = quits = to_game = 0;
    if (kind == DELETE)
    {
        m_confirm_action = CONFIRM_DELETE_S;
        m_confirm_prevstate = m_load;
        m_state = m_confirm;
        if (key) M_ConfirmKey (key);
        assert (!quits && !to_game);
        if (deleted) { assert (deleted == 1 && m_state == m_load); return 1; }
        if (m_state == m_confirm) return 0;
        assert (m_state == m_load);
        return -1;
    }
    m_quit_prevstate = m_load;
    m_state = m_quit;
    if (key) M_Quit_Key (key);
    assert (!deleted);
    if (quits) { assert (quits == 1); return 1; }
    if (m_state == m_quit) { assert (!to_game); return 0; }
    /* cancelled: back to the menu it came from, or to the game */
    if (wasInMenus) assert (m_state == m_load && !to_game);
    else            assert (m_state == m_none && to_game == 1);
    /* the quit prompt takes its rows with it; put them back for the caller */
    assert (m_confirm_hint_y < 0);
    redraw ();
    return -1;
}

/* The pointer moved to (vx, vy), then one K_MOUSE1 against the prompt, long
 * after any earlier click. */
static int click (qboolean touch, qboolean pointer, int vx, int vy)
{
    int key;
    m_state = (kind == DELETE) ? m_confirm : m_quit;
    move (vx, vy);
    realtime += 1.0;
    key = mouse1 (touch, pointer);
    return answer (key);
}

/* One pointer K_MOUSE1 that finds the pointer at (vx, vy) without the menu
 * having seen it get there. */
static int click_found (int vx, int vy)
{
    int key;
    m_state = (kind == DELETE) ? m_confirm : m_quit;
    found (vx, vy);
    realtime += 1.0;
    key = mouse1 (false, true);
    return answer (key);
}

static void open (int px, int py, const char *question)
{
    found (px, py);
    m_state = m_load;
    if (kind == DELETE)
    {
        M_Confirm (CONFIRM_DELETE_S, question);
        assert (m_state == m_confirm && m_confirm_prevstate == m_load);
    }
    else
    {   /* what M_Menu_Quit_f does; check_wiring pins that it does */
        m_quit_prevstate = m_state;
        M_Prompt_Open ();
        m_state = m_quit;
    }
}

static int find (const char *text)
{
    int i, hit = -1;
    for (i = 0; i < nprinted; i++)
        if (!strcmp (printed[i].text, text)) { assert (hit < 0); hit = i; }
    assert (hit >= 0);
    return hit;
}

/* Every point of the menu canvas and a margin round it. */
#define SWEEP(x, y) for (y = -16; y < 216; y++) for (x = -16; x < 336; x++)
#define FAR -100    /* off the sweep, so every swept point is a move */

static void prompt (int k, const char *question)
{
    const char *hint1 = (k == DELETE) ? CONFIRM_HINT1 : QUIT_HINT1;
    const char *hint2 = (k == DELETE) ? CONFIRM_HINT2 : QUIT_HINT2;
    int x, y, x0, x1, top, bottom, yes, no, yx, nx, yy, ny, i, want, c;
    int corner[4][2];

    kind = k;

    /* Raised, not yet drawn: nothing on the canvas is an answer, whatever the
     * prompt before this one left behind. */
    open (FAR, FAR, question);
    SWEEP (x, y)
    {
        assert (m_confirm_hint_y < 0);
        m_state = (kind == DELETE) ? m_confirm : m_quit;
        menu_mouse_x = x; menu_mouse_y = y;
        realtime += 1.0;
        assert (mouse1 (false, true) == 0);
    }

    /* Drawn.  The answer rows are wherever the two hints were actually
     * printed.  A confirmation's run across the inside of the box
     * M_DrawTextBox was asked for (an 8-unit border either side of `width`
     * 8-unit columns); the quit prompt's box is the whole screen, so its rows
     * are as wide as the longer hint and a column either side. */
    draw (0, 0);
    top = box_y + 8;
    bottom = box_y + 8 + box_lines * 8;
    yes = find (hint1);
    no = find (hint2);
    yx = printed[yes].x;
    nx = printed[no].x;
    yy = printed[yes].y;
    ny = printed[no].y;
/* hovered or not, each hint is drawn in the one place */
#define PLACED() assert (printed[find (hint1)].x == yx && printed[find (hint1)].y == yy \
                      && printed[find (hint2)].x == nx && printed[find (hint2)].y == ny)
    if (kind == DELETE)
    {
        x0 = box_x + 8;
        x1 = box_x + 8 + box_w * 8;
    }
    else
    {
        assert (box_x == 0 && box_y == 0 && box_w == 38 && box_lines == 23);
        assert (strlen (hint2) >= strlen (hint1));
        x0 = printed[no].x - 8;
        x1 = printed[no].x + 8 * (int)strlen (hint2) + 8;
    }
    assert (ny == yy + 8);
    assert (x0 >= 0 && x1 <= 320 && top >= 0 && bottom <= 200);
    /* both hints centred on the same axis as the rows */
    assert (2 * printed[yes].x + 8 * (int)strlen (hint1) == x0 + x1);
    assert (2 * printed[no].x + 8 * (int)strlen (hint2) == x0 + x1);
    for (i = 0; i < nprinted; i++)
    {   /* all of it inside the box, and nothing else is on an answer row */
        assert (printed[i].x >= box_x + 8 && printed[i].x + 8 * (int)strlen (printed[i].text) <= box_x + 8 + box_w * 8);
        assert (printed[i].y >= top && printed[i].y + 8 <= bottom);
        if (i != yes && i != no)
            assert (printed[i].y + 8 <= yy);
    }
    assert (printed[yes].white && printed[no].white);   /* pointer at 0,0 */

    /* The whole canvas: a click answers on those two rows and nowhere else. */
    SWEEP (x, y)
    {
        want = (x < x0 || x >= x1) ? 0
             : (y >= yy && y < yy + 8) ? 1
             : (y >= ny && y < ny + 8) ? -1 : 0;
        assert (click (false, true, x, y) == want);
        /* ui_mouse off: the click has no position, so it is never an answer. */
        assert (click (false, false, x, y) == 0);
    }

    /* The touch overlay's attack button accepts wherever the pointer was last
     * left, held mouse button or not. */
    assert (click (true, false, 0, 0) == 1);
    assert (click (true, true, x0, ny) == 1);

    /* The row under the pointer is drawn in the question's face, and only
     * with a pointer to aim. */
    draw (x0, yy);
    PLACED ();
    assert (!printed[find (hint1)].white && printed[find (hint2)].white);
    draw (x1 - 1, ny + 7);
    PLACED ();
    assert (printed[find (hint1)].white && !printed[find (hint2)].white);
    draw (x0 - 1, yy);
    PLACED ();
    assert (printed[find (hint1)].white && printed[find (hint2)].white);
    ui_mouse.integer = 0;
    draw (x0, yy);
    PLACED ();
    assert (printed[find (hint1)].white && printed[find (hint2)].white);
    ui_mouse.integer = 1;

    /* Raised under a resting pointer: the row it happens to be on is not a
     * target and not an answer until the pointer has been moved.  Then it is,
     * and stays one when the pointer comes back to the same spot. */
    corner[0][0] = x0;     corner[0][1] = yy;
    corner[1][0] = x1 - 1; corner[1][1] = yy + 7;
    corner[2][0] = x0;     corner[2][1] = ny;
    corner[3][0] = x1 - 1; corner[3][1] = ny + 7;
    for (c = 0; c < 4; c++)
    {
        x = corner[c][0]; y = corner[c][1];
        want = (c < 2) ? 1 : -1;
        open (x, y, question);
        draw (x, y);
        assert (printed[find (hint1)].white && printed[find (hint2)].white);
        assert (click (false, true, x, y) == 0);
        assert (click (false, true, x, y) == 0);
        assert (click (true, false, x, y) == 1);     /* touch needs no aim */
        assert (click (false, true, x, y) == 0);     /* and gives none */
        redraw ();
        assert (printed[find (hint1)].white && printed[find (hint2)].white);
        if (c & 1)
            assert (click (false, true, x, y - 16) == 0);  /* a missed click is a move */
        else if (c == 0)
            draw (x + 1, y);                         /* one unit is a move */
        else
            draw (x, y + 1);
        draw (x, y);
        PLACED ();
        assert (printed[find (hint1)].white == (want != 1));
        assert (printed[find (hint2)].white == (want != -1));
        assert (click (false, true, x, y) == want);
        assert (click (false, true, x, y) == want);

        /* The pointer was put on the row while the game owned input, so the
         * menu never saw it move.  The click that finds it there refreshes
         * the position; that is not a move, and it is not an answer. */
        open (FAR, FAR, question);
        redraw ();
        assert (click_found (x, y) == 0);
        assert (click_found (x, y) == 0);
        redraw ();
        PLACED ();
        assert (printed[find (hint1)].white && printed[find (hint2)].white);
        move (x, y);                                 /* already there: no move */
        assert (click_found (x, y) == 0);
        move (x + 1, y);
        move (x, y);
        redraw ();
        assert (printed[find (hint1)].white == (want != 1));
        assert (click_found (x, y) == want);

        /* in_sdl.c follows the pointer while the game owns input too, so the
         * motion event SDL sends by itself on leaving relative mouse mode
         * finds the pointer where it was last seen.  That is no move. */
        open (x, y, question);
        redraw ();
        move (x, y);
        redraw ();
        PLACED ();
        assert (printed[find (hint1)].white && printed[find (hint2)].white);
        assert (click_found (x, y) == 0);
        assert (click_found (x, y) == 0);

        /* One move is enough: straight onto the row from somewhere else. */
        open (FAR, FAR, question);
        redraw ();
        move (x, y);
        redraw ();
        assert (printed[find (hint1)].white == (want != 1));
        assert (printed[find (hint2)].white == (want != -1));
        assert (click_found (x, y) == want);
    }
#undef PLACED
}

/* After a pointer click answers a prompt, the menu under it must not take the
 * second click of a double click as Enter. */
static void lockout (int k, int vx, int vy, int want)
{
    double t0, was;

    kind = k;
    open (FAR, FAR, "Sure?\n");
    draw (0, 0);
    press_claims = 0;

    /* touch, ui_mouse-off and missed clicks start no lockout */
    was = m_click_lockout;
    assert (click (false, true, FAR + 1, FAR) == 0);
    assert (click (false, false, vx, vy) == 0);
    assert (click (true, false, vx, vy) == 1);
    assert (m_click_lockout == was);

    t0 = realtime + 1.0;
    assert (click (false, true, vx, vy) == want);
    assert (realtime == t0 && m_click_lockout == t0 + CLICK_LOCKOUT);
    assert (CLICK_LOCKOUT >= 0.25 && CLICK_LOCKOUT <= 0.5);

    /* the menu underneath, pointer where the answer row was */
    m_state = m_load;
    assert (mouse1 (false, true) == 0);
    realtime = t0 + CLICK_LOCKOUT - 0.001;
    assert (mouse1 (false, true) == 0);
    assert (mouse1 (true, false) == K_ENTER);   /* touch is never locked out */
    assert (mouse1 (false, false) == K_ENTER);  /* nor a click with ui_mouse off */
    /* nor does the pointer layer get the press */
    press_claims = 2;
    assert (mouse1 (false, true) == 0);
    press_claims = 0;
    /* a prompt raised again in that time is locked too */
    m_state = (kind == DELETE) ? m_confirm : m_quit;
    assert (mouse1 (false, true) == 0);
    assert (m_click_lockout == t0 + CLICK_LOCKOUT);

    realtime = t0 + CLICK_LOCKOUT;
    m_state = m_load;
    assert (mouse1 (false, true) == K_ENTER);
    assert (m_click_lockout == t0 + CLICK_LOCKOUT);
    realtime += 1.0;

    /* Outside a prompt and outside the lockout: the pointer layer is offered
     * a pointer press and may claim it; nothing else is offered to it. */
    press_claims = 1;
    assert (mouse1 (false, true) == 0);
    assert (mouse1 (false, false) == K_ENTER);
    assert (mouse1 (true, false) == K_ENTER);
    press_claims = 0;
    assert (mouse1 (false, true) == K_ENTER);
}

int main (void)
{
    int k, w;

    /* The shipped questions, then one wider and one taller than the hints so
     * the box, and the rows with it, land somewhere else each time; the quit
     * prompt between them leaves its own rows behind for the next. */
    prompt (DELETE, "Are you sure you want to\nstart a new game?\n");
    wasInMenus = true;      /* over a menu, which M_Quit_Draw draws first */
    prompt (QUIT, NULL);
    prompt (DELETE, "Are you sure you want to\ndelete this saved game?\n");
    prompt (DELETE, "A question wider than both hints\n");
    wasInMenus = false;     /* straight from the game */
    prompt (QUIT, NULL);
    prompt (DELETE, "One\ntwo\nthree\nfour\nfive\nsix\n");
    prompt (DELETE, "Are you sure you want to\nstart a new game?\n");

    /* Rows as drawn for "Sure?" are at y 96 and 104, the quit prompt's at
     * y 176 and 184; lockout() fails on its first answer if they move. */
    lockout (DELETE, 100, 100, 1);
    lockout (DELETE, 100, 108, -1);
    wasInMenus = true;
    lockout (QUIT, 160, 176, 1);
    lockout (QUIT, 160, 184, -1);

    /* Keys: exactly these answer, on both prompts, from a menu or the game. */
    for (w = 0; w < 2; w++)
        for (kind = DELETE; kind <= QUIT; kind++)
        {
            wasInMenus = w;
            open (FAR, FAR, "Sure?\n");
            redraw ();
            for (k = 0; k < 512; k++)
            {
                int want = (k == 'y' || k == 'Y' || k == K_ENTER) ? 1
                         : (k == 'n' || k == 'N' || k == K_ESCAPE) ? -1 : 0;
                assert (answer (k) == want);
            }
        }

    puts ("PASS: a click answers a prompt only on its answer rows as drawn, only "
          "once aimed, and not twice; touch, y/Y/Enter confirm and n/N/Escape cancel");
    return 0;
}
"""



# Whole-word uses in menu.c with comments stripped.
USES = (
    ("M_ConfirmKey", 3), ("M_Confirm_ClickKey", 2), ("M_Confirm_HitKey", 3),
    ("M_ConfirmAction", 4), ("M_ConfirmDraw", 3),
    ("M_Quit_Key", 3), ("M_Quit_Draw", 3),
    ("M_Prompt_Open", 4), ("M_Prompt_Aimed", 3), ("M_Prompt_DrawAnswers", 4),
    ("menu_touch_key", 2),
    ("m_confirm", 4), ("m_confirm_action", 5), ("m_confirm_prevstate", 4),
    ("m_quit", 6), ("m_quit_prevstate", 4),
    ("m_confirm_hint_x0", 3), ("m_confirm_hint_x1", 3), ("m_confirm_hint_y", 9),
    ("m_prompt_motions", 3), ("menu_mouse_motions", 3),
    ("m_click_lockout", 3), ("CLICK_LOCKOUT", 2),
)


def live(text):
    """Source with comments and #if 0 blocks removed."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"^[ \t]*#[ \t]*if[ \t]+0\b.*?^[ \t]*#[ \t]*endif[^\n]*", "", text,
                  flags=re.S | re.M)


def squash(text):
    return re.sub(r"\s+", " ", text)


def defines(names):
    text = (ROOT / MENU).read_text()
    out = []
    for name in names:
        found = re.findall(r"^#define[ \t]+%s[ \t]+[^\n/]*" % name, text, re.M)
        assert len(found) == 1, f"{name} not defined exactly once in menu.c"
        out.append(found[0].rstrip())
    return out


KEYDOWN = "void M_Keydown (int key, qboolean repeat)\n{"


def click_block():
    """M_Keydown from its first statement to the end of the click block."""
    keydown = function(MENU, KEYDOWN)
    start = keydown.index("\tif (key == K_MOUSE1)\n")
    return keydown[start:keydown.index("\telse if (key == K_MWHEELUP)\n")]


def run_harness():
    source = PRELUDE + "\n".join(defines((
        "CONFIRM_HINT1", "CONFIRM_HINT2", "QUIT_HINT1", "QUIT_HINT2",
        "QUIT_SIZE", "CLICK_LOCKOUT"))) + "\n" + "\n".join(function(MENU, sig) for sig in (
        "static void M_Prompt_Open (void)\n{",
        "static qboolean M_Prompt_Aimed (void)\n{",
        "static int M_Confirm_HitKey (int vx, int vy)\n{",
        "static int M_Confirm_ClickKey (qboolean touch, qboolean pointer, int vx, int vy)\n{",
        "static void M_Prompt_DrawAnswers (int x0, int x1, int y, const char *yes, const char *no)\n{",
        "static void M_ConfirmKey (int key)\n{",
        "static void M_Confirm (confirm_action_t action, const char *question)\n{",
        "static void M_ConfirmDraw (void)\n{",
        "static void M_Quit_Key (int key)\n{",
        "static void M_Quit_Draw (void)\n{",
    )) + CLICK_BLOCK % click_block() + TEST
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "confirm.c")
        exe = os.path.join(tmp, "confirm")
        with open(src, "w") as f:
            f.write(source)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O1", "-Wall", "-Wextra",
                        "-Werror", src, "-o", exe, "-lm"], check=True)
        subprocess.run([exe], check=True)


def check_wiring():
    menu = squash(live((ROOT / MENU).read_text()))
    assert "SCR_ModalMessage" not in menu, \
        "menu.c calls SCR_ModalMessage: it blocks, and hangs the web client"

    # The click block is the first thing M_Keydown does, and this is all of it:
    # nothing may rewrite the key above it, inside it or beside it.  It is the
    # text the harness compiled.
    keydown = squash(live(function(MENU, KEYDOWN)))
    want = ("void M_Keydown (int key, qboolean repeat) { "
            "if (key == K_MOUSE1) { "
            "qboolean pointer = (menu_mouse_buttons & MENU_MOUSE_LEFT) != 0; "
            "if (pointer && realtime < m_click_lockout) return; "
            "if (!repeat && pointer && M_MousePress ()) return; "
            "if (m_state == m_confirm || m_state == m_quit) { "
            "key = M_Confirm_ClickKey (menu_touch_key, pointer, "
            "M_PointerCanvasX (), M_PointerCanvasY ()); "
            "if (!key) return; "
            "if (key != K_ENTER) m_click_lockout = realtime + CLICK_LOCKOUT; "
            "} else key = K_ENTER; } "
            # The rest of the remap chain: no wheel notch, other button or
            # pad key may become an answer the click could not be.
            "else if (key == K_MWHEELUP) key = K_UPARROW; "
            "else if (key == K_MWHEELDOWN) key = K_DOWNARROW; "
            "else if (key == K_GP_A) key = K_ENTER; "
            "else if (key == K_GP_B) key = K_ESCAPE; "
            "else if (key == K_GP_DPAD_UP || key == K_GP_DPAD_DOWN || "
            "key == K_GP_DPAD_LEFT || key == K_GP_DPAD_RIGHT) {")
    assert keydown.startswith(want), \
        "M_Keydown's click block or remap chain changed; a click on a prompt must go " \
        "through M_Confirm_ClickKey and nothing else. Found: " + keydown[:len(want) + 80]
    assert squash(live(click_block())).strip() + " " == \
        want[len("void M_Keydown (int key, qboolean repeat) { "):want.index("else if (key == K_MWHEELUP)")], \
        "the harness did not compile the click block that M_Keydown opens with"
    assert "case m_confirm: M_ConfirmKey (key); return;" in keydown, \
        "M_Keydown must hand m_confirm's key straight to M_ConfirmKey"
    assert "case m_quit: M_Quit_Key (key); return;" in keydown, \
        "M_Keydown must hand m_quit's key straight to M_Quit_Key"
    for state in ("m_confirm", "m_quit"):
        assert len(re.findall(r"\b%s\b" % state, keydown)) == 2, \
            f"M_Keydown mentions {state} outside the click block and the dispatch"
    # Ten rewrites of the key in all: the click block's two, wheel up and
    # down, pad A and B, and the four d-pad arrows.  Two of them are Enter.
    assert len(re.findall(r"\bkey\s*(?:%s|\+\+|--)|(?:\+\+|--)\s*key\b" % ASSIGN, keydown)) == 10, \
        "M_Keydown rewrites the key somewhere new"
    assert keydown.count("key = K_ENTER;") == 2, \
        "M_Keydown turns something other than a non-prompt click and pad A into Enter"

    # The pointer layer runs ahead of the click block; it must not know the
    # prompts exist.
    press = squash(live(function(MENU, "static qboolean M_MousePress (void)\n{")))
    assert press == ("static qboolean M_MousePress (void) { m_grab = MGRAB_NONE; "
                     "if (m_state == m_mods) return M_Mods_MousePress (); "
                     "return M_Slider_MousePress (); }"), \
        "M_MousePress changed; it runs before the prompts' click test"

    # One caller each, so there is no second way in, and nothing else that
    # answers, arms or leaves a prompt.
    for name, uses in USES:
        found = len(re.findall(r"\b%s\b" % name, menu))
        assert found == uses, f"menu.c uses {name!r} {found} times, expected {uses}"
    assert "case m_confirm: M_ConfirmDraw (); break;" in menu, \
        "M_Draw's m_confirm case must draw the prompt and do nothing else"
    assert "case m_quit: M_Quit_Draw (); break;" in menu, \
        "M_Draw's m_quit case must draw the prompt and do nothing else"
    # The harness's M_Draw stub and redraw() copy the canvas M_Draw dispatches
    # in and the canvas it leaves in.
    draw_fn = squash(live(function(MENU, "void M_Draw (void)\n{")))
    assert "GL_SetCanvas (CANVAS_MENU); m_canvas_active = true; switch (m_state) {" in draw_fn, \
        "M_Draw must dispatch inside the menu canvas"
    assert ("menu_mouse_moved = false; m_canvas_active = false; "
            "GL_SetCanvas (CANVAS_DEFAULT);") in draw_fn, \
        "M_Draw's tail changed; the harness's M_Draw stub models it"
    # The harness raises the quit prompt by hand; this is what it copies.
    quit_f = squash(live(function(MENU, "void M_Menu_Quit_f (void)\n{")))
    assert ("wasInMenus = !!(Key_GetDest () & key_menu); Key_SetDest (key_menu); "
            "m_quit_prevstate = m_state; M_Prompt_Open (); m_state = m_quit; ") in quit_f, \
        "M_Menu_Quit_f must open the prompt through M_Prompt_Open"
    for name in ("menu_mouse_buttons", "menu_mouse_x", "menu_mouse_y", "menu_mouse_motions", "realtime"):
        assert not re.search(r"\b%s\s*(?:%s|\+\+|--)|(?:\+\+|--)\s*%s\b" % (name, ASSIGN, name), menu), \
            f"menu.c writes {name}; the prompts' click test reads it as input"
    assert "#define CLICK_LOCKOUT 0.5 " in menu, "CLICK_LOCKOUT changed"
    for path in (MENU, INPUT):
        for line in live((ROOT / path).read_text()).splitlines():
            assert not re.match(r"\s*#\s*(define|undef)\b.*(M_Confirm|m_confirm|M_Prompt|m_prompt|"
                                r"M_Quit|m_quit|m_click_lockout|menu_touch_key|menu_mouse_motions|\brealtime\b)", line), \
                f"{path} has a macro over the prompt code: {line.strip()}"

    # menu_touch_key is zero-initialised, raised by Hexenwail_TouchKey around
    # its Key_Event and touched by nothing else, so a real click can never
    # pass for a touch.
    inp = squash(live((ROOT / INPUT).read_text()))
    touch = squash(live(function(INPUT, "EMSCRIPTEN_KEEPALIVE int Hexenwail_TouchKey (int key, int down)\n{")))
    assert touch.endswith("{ if (key <= 0 || key > K_GP_DPAD_RIGHT) return 0; "
                          "menu_touch_key = true; Key_Event(key, down ? true : false); "
                          "menu_touch_key = false; return 1; }"), \
        "Hexenwail_TouchKey must raise menu_touch_key around its Key_Event only"
    assert inp.count("menu_touch_key") == 3, \
        "in_sdl.c touches menu_touch_key outside its definition and Hexenwail_TouchKey"
    assert " qboolean menu_touch_key; " in inp, \
        "menu_touch_key must be defined without an initialiser"

    # What M_Keydown's `pointer` argument rests on: the button bit is set in
    # one place, behind ui_mouse, and that place sends the click as K_MOUSE1.
    block = ("case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: "
             "if (!event.button.down && event.button.button >= 1 && "
             "event.button.button <= 16) "
             "menu_mouse_buttons &= ~MENU_MOUSE_BUTTON(event.button.button); "
             "if ((Key_GetDest() & key_menu) && ui_mouse.integer) { "
             "if (event.button.down && event.button.button >= 1 && "
             "event.button.button <= 16) "
             "menu_mouse_buttons |= MENU_MOUSE_BUTTON(event.button.button); "
             "IN_MenuPointer (&menu_mouse_x, &menu_mouse_y); "
             "menu_mouse_moved = true; "
             "if (event.button.button == 1) Key_Event(K_MOUSE1, event.button.down); "
             "break; }")
    assert block in inp, \
        "in_sdl.c's menu click block changed; the button bit must be set only " \
        "with ui_mouse on, and a click must arrive as K_MOUSE1"
    # Every write to the bits, by any operator: two clears, the release, and
    # the one set pinned above.
    writes = re.findall(r"\bmenu_mouse_buttons\s*(%s\s*\S+)" % ASSIGN, inp)
    assert sorted(writes) == sorted(["= 0;", "= 0;", "&= ~MENU_MOUSE_BUTTON(event.button.button);",
                                     "|= MENU_MOUSE_BUTTON(event.button.button);"]), \
        f"in_sdl.c writes menu_mouse_buttons somewhere new: {writes}"
    assert not re.search(r"(\+\+|--)\s*menu_mouse_buttons|menu_mouse_buttons\s*(\+\+|--)", inp), \
        "in_sdl.c writes menu_mouse_buttons somewhere new"

    # What M_Prompt_Aimed rests on, and what the harness's move() copies: the
    # count goes up when a motion event moves the menu pointer, and nowhere
    # else -- not in the button handler above, which only refreshes x/y.
    # The position is taken on every motion event, whoever owns input, before
    # anything is counted: a position left stale while the game owned input
    # lets the motion event SDL sends on leaving relative mode pass for a move.
    motion = ("case SDL_EVENT_MOUSE_MOTION: { int x, y; qboolean moved; "
              "IN_MenuPointer (&x, &y); "
              "moved = (x != menu_mouse_x || y != menu_mouse_y); "
              "menu_mouse_x = x; menu_mouse_y = y; "
              "if (Key_GetDest() == key_console) { float mx, my; "
              "SDL_GetMouseState(&mx, &my); Con_MouseMove((int)mx, (int)my); } "
              "else if ((Key_GetDest() & key_menu) && ui_mouse.integer && moved) { "
              "menu_mouse_moved = true; menu_mouse_motions++; } break; }")
    assert motion in inp, \
        "in_sdl.c's motion block changed; menu_mouse_motions must count real moves only, " \
        "against a position tracked on every motion event"
    assert " unsigned int menu_mouse_motions; " in inp, "menu_mouse_motions must be defined once"
    assert len(re.findall(r"\bmenu_mouse_motions\b", inp)) == 2, \
        "in_sdl.c touches menu_mouse_motions outside its definition and the motion block"

    # The bit the click block tests is the bit a left press sets.
    pointer_h = squash(live((ROOT / POINTER).read_text()))
    for define in ("#define MENU_MOUSE_BUTTON(b) (1 << ((b) - 1)) ",
                   "#define MENU_MOUSE_LEFT MENU_MOUSE_BUTTON(1) "):
        assert define in pointer_h, f"menu_pointer.h no longer has: {define.strip()}"


def main():
    check_wiring()
    run_harness()


if __name__ == "__main__":
    main()
