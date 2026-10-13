#!/usr/bin/env python3
"""Regression test for the menu confirmation prompts (#316 and its follow-up).

The menus used to ask y/n through SCR_ModalMessage, which waits for a key by
blocking.  A browser delivers keys only between frames, so the wait spun inside
one wasm frame forever.  The prompts are a menu state now (m_confirm), answered
by M_ConfirmKey -- and that put them behind M_Keydown, which turns every left
click into K_ENTER.  M_ConfirmKey takes K_ENTER as yes, so one click anywhere
on the screen started a new game over the running one, or deleted a save.

A click on a confirmation now answers only when it lands on an answer row.

Part one compiles the engine's own M_Confirm, M_ConfirmDraw, M_Confirm_HitKey,
M_Confirm_ClickKey and M_ConfirmKey out of menu.c, with the draw calls recorded
instead of drawn, and for several questions sweeps every point of the menu
canvas:

  * between M_Confirm and the first draw no point answers, whatever rows the
    previous prompt left behind;
  * once drawn, a pointer click is yes exactly on the row where "Press y to
    confirm" was printed, no exactly on the row where "Press n to cancel" was,
    across the inside of the box that was drawn, and nothing anywhere else;
  * a click with no pointer (ui_mouse off) answers nowhere;
  * the web touch overlay's K_MOUSE1 is yes, as gamepad A is;
  * the row under the pointer, and only that row, changes face;
  * y, Y and K_ENTER are yes; n, N and K_ESCAPE are no; no other key answers.

Part two reads the source for the wiring the harness cannot reach, because
M_Keydown dispatches to every menu in the game and cannot be compiled alone:

  * no menu code calls SCR_ModalMessage;
  * M_Keydown opens with the click block, and the block is exactly: pointer
    layer first, then M_Confirm_ClickKey on m_confirm (dropped on 0), K_ENTER
    in every other menu;
  * M_ConfirmKey, M_Confirm_ClickKey and menu_touch_key each have the one use
    they should;
  * menu_touch_key is raised by Hexenwail_TouchKey around its Key_Event and by
    nothing else, so a real click can never pass for a touch.

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

PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int qboolean;
#define true 1
#define false 0
#define K_ENTER 13
#define K_ESCAPE 27
typedef enum { CONFIRM_NONE = 0, CONFIRM_DELETE_S } confirm_action_t;
enum m_state_e { m_load, m_confirm };
static confirm_action_t m_confirm_action;
static enum m_state_e m_confirm_prevstate, m_state;
static qboolean m_entersound;
static int load_cursor, m_confirm_cursor;
static const char *m_confirm_text;
static int m_confirm_hint_x0, m_confirm_hint_x1, m_confirm_hint_y = -1;
static struct { int integer; } ui_mouse = { 1 };

static int deleted;
static void M_ConfirmAction (confirm_action_t action) { if (action == CONFIRM_DELETE_S) deleted++; }

/* What M_ConfirmDraw put on screen, recorded instead of drawn. */
static int ptr_x, ptr_y;
static int M_PointerCanvasX (void) { return ptr_x; }
static int M_PointerCanvasY (void) { return ptr_y; }
static int box_n, box_x, box_y, box_w, box_lines;
static void M_DrawTextBox (int x, int y, int width, int lines)
{ box_n++; box_x = x; box_y = y; box_w = width; box_lines = lines; }
static struct { int white, x, y; char text[80]; } printed[16];
static int nprinted;
static void record (int white, int cx, int cy, const char *str)
{
    assert (nprinted < 16 && strlen (str) < 80);
    printed[nprinted].white = white; printed[nprinted].x = cx; printed[nprinted].y = cy;
    strcpy (printed[nprinted].text, str);
    nprinted++;
}
static void M_Print (int cx, int cy, const char *str) { record (0, cx, cy, str); }
static void M_PrintWhite (int cx, int cy, const char *str) { record (1, cx, cy, str); }
'''

TEST = r'''
/* One K_MOUSE1 against a "delete this save?" that is already up, the way
 * M_Keydown delivers it.  Returns 1 if the save was deleted, 0 if the prompt is
 * still up, -1 if it was cancelled. */
static int click (qboolean touch, qboolean pointer, int vx, int vy)
{
    int key;
    deleted = 0;
    m_confirm_action = CONFIRM_DELETE_S;
    m_confirm_prevstate = m_load;
    m_state = m_confirm;
    key = M_Confirm_ClickKey (touch, pointer, vx, vy);
    if (key)
        M_ConfirmKey (key);
    if (deleted) { assert (m_state == m_load); return 1; }
    return m_state == m_confirm ? 0 : -1;
}

static int press (int key)
{
    deleted = 0;
    m_confirm_action = CONFIRM_DELETE_S;
    m_confirm_prevstate = m_load;
    m_state = m_confirm;
    M_ConfirmKey (key);
    if (deleted) return 1;
    return m_state == m_confirm ? 0 : -1;
}

static void draw (int px, int py)
{
    ptr_x = px; ptr_y = py;
    box_n = nprinted = 0;
    M_ConfirmDraw ();
    assert (box_n == 1);
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

static void prompt (const char *question)
{
    int x, y, x0, x1, top, bottom, yes, no, yy, ny, i, want;

    /* Raised, not yet drawn: nothing on the canvas is an answer, whatever the
     * prompt before this one left behind. */
    m_state = m_load;
    M_Confirm (CONFIRM_DELETE_S, question);
    assert (m_state == m_confirm && m_confirm_prevstate == m_load);
    SWEEP (x, y)
        assert (click (false, true, x, y) == 0);

    /* Drawn.  The answer rows are wherever the two hints were actually
     * printed, across the inside of the box M_DrawTextBox was asked for (an
     * 8-unit border either side of `width` 8-unit columns). */
    draw (0, 0);
    x0 = box_x + 8;
    x1 = box_x + 8 + box_w * 8;
    top = box_y + 8;
    bottom = box_y + 8 + box_lines * 8;
    yes = find (CONFIRM_HINT1);
    no = find (CONFIRM_HINT2);
    yy = printed[yes].y;
    ny = printed[no].y;
    assert (ny == yy + 8);
    assert (x0 >= 0 && x1 <= 320 && top >= 0 && bottom <= 200);
    for (i = 0; i < nprinted; i++)
    {   /* all of it inside the box, and the question is not an answer row */
        assert (printed[i].x >= x0 && printed[i].x + 8 * (int)strlen (printed[i].text) <= x1);
        assert (printed[i].y >= top && printed[i].y + 8 <= bottom);
        if (i != yes && i != no)
            assert (!printed[i].white && printed[i].y + 8 <= yy);
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
    assert (!printed[find (CONFIRM_HINT1)].white && printed[find (CONFIRM_HINT2)].white);
    draw (x1 - 1, ny + 7);
    assert (printed[find (CONFIRM_HINT1)].white && !printed[find (CONFIRM_HINT2)].white);
    draw (x0 - 1, yy);
    assert (printed[find (CONFIRM_HINT1)].white && printed[find (CONFIRM_HINT2)].white);
    ui_mouse.integer = 0;
    draw (x0, yy);
    assert (printed[find (CONFIRM_HINT1)].white && printed[find (CONFIRM_HINT2)].white);
    ui_mouse.integer = 1;
}

int main (void)
{
    int k;

    /* The shipped questions, then one wider and one taller than the hints so
     * the box, and the rows with it, land somewhere else each time. */
    prompt ("Are you sure you want to\nstart a new game?\n");
    prompt ("Are you sure you want to\ndelete this saved game?\n");
    prompt ("A question wider than both hints\n");
    prompt ("One\ntwo\nthree\nfour\nfive\nsix\n");
    prompt ("Are you sure you want to\nstart a new game?\n");

    /* Keys: exactly these answer. */
    for (k = 0; k < 512; k++)
    {
        int want = (k == 'y' || k == 'Y' || k == K_ENTER) ? 1
                 : (k == 'n' || k == 'N' || k == K_ESCAPE) ? -1 : 0;
        assert (press (k) == want);
    }

    puts ("PASS: a click answers a confirmation only on the answer rows as drawn; "
          "touch, y/Y/Enter confirm and n/N/Escape cancel");
    return 0;
}
'''


def live(text):
    """Source with comments and #if 0 blocks removed."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"^[ \t]*#[ \t]*if[ \t]+0\b.*?^[ \t]*#[ \t]*endif[^\n]*", "", text,
                  flags=re.S | re.M)


def squash(text):
    return re.sub(r"\s+", " ", text)


def run_harness():
    hints = re.findall(r'^#define\s+CONFIRM_HINT[12]\s+"[^"\n]*"$', (ROOT / MENU).read_text(), re.M)
    assert len(hints) == 2, "CONFIRM_HINT1/2 not found in menu.c"
    source = PRELUDE + "\n".join(hints) + "\n" + "\n".join(function(MENU, sig) for sig in (
        "static int M_Confirm_HitKey (int vx, int vy)\n{",
        "static int M_Confirm_ClickKey (qboolean touch, qboolean pointer, int vx, int vy)\n{",
        "static void M_ConfirmKey (int key)\n{",
        "static void M_Confirm (confirm_action_t action, const char *question)\n{",
        "static void M_ConfirmDraw (void)\n{",
    )) + TEST
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "confirm.c")
        exe = os.path.join(tmp, "confirm")
        with open(src, "w") as f:
            f.write(source)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O1", "-Wall", "-Wextra",
                        "-Werror", src, "-o", exe], check=True)
        subprocess.run([exe], check=True)


def check_wiring():
    menu = squash(live((ROOT / MENU).read_text()))
    assert "SCR_ModalMessage" not in menu, \
        "menu.c calls SCR_ModalMessage: it blocks, and hangs the web client"

    # The click block is the first thing M_Keydown does, and this is all of it:
    # nothing may rewrite the key above it, inside it or beside it.
    keydown = squash(live(function(MENU, "void M_Keydown (int key, qboolean repeat)\n{")))
    want = ("void M_Keydown (int key, qboolean repeat) { "
            "if (key == K_MOUSE1) { "
            "if (!repeat && (menu_mouse_buttons & MENU_MOUSE_LEFT) && M_MousePress ()) return; "
            "if (m_state == m_confirm) { "
            "key = M_Confirm_ClickKey (menu_touch_key, "
            "(menu_mouse_buttons & MENU_MOUSE_LEFT) != 0, "
            "M_PointerCanvasX (), M_PointerCanvasY ()); "
            "if (!key) return; } else key = K_ENTER; } "
            # The rest of the remap chain: no wheel notch, other button or
            # pad key may become an answer the click could not be.
            "else if (key == K_MWHEELUP) key = K_UPARROW; "
            "else if (key == K_MWHEELDOWN) key = K_DOWNARROW; "
            "else if (key == K_GP_A) key = K_ENTER; "
            "else if (key == K_GP_B) key = K_ESCAPE; "
            "else if (key == K_GP_DPAD_UP || key == K_GP_DPAD_DOWN || "
            "key == K_GP_DPAD_LEFT || key == K_GP_DPAD_RIGHT) {")
    assert keydown.startswith(want), \
        "M_Keydown's click block or remap chain changed; a click on m_confirm must go " \
        "through M_Confirm_ClickKey and nothing else. Found: " + keydown[:len(want) + 80]
    assert "case m_confirm: M_ConfirmKey (key); return;" in keydown, \
        "M_Keydown must hand m_confirm's key straight to M_ConfirmKey"
    assert keydown.count("m_confirm") == 2, \
        "M_Keydown mentions m_confirm outside the click block and the dispatch"
    # Ten rewrites of the key in all: the click block's two, wheel up and
    # down, pad A and B, and the four d-pad arrows.  Two of them are Enter.
    assert len(re.findall(r"\bkey\s*(?:%s|\+\+|--)|(?:\+\+|--)\s*key\b" % ASSIGN, keydown)) == 10, \
        "M_Keydown rewrites the key somewhere new"
    assert keydown.count("key = K_ENTER;") == 2, \
        "M_Keydown turns something other than a non-confirm click and pad A into Enter"

    # The pointer layer runs ahead of the click block; it must not know the
    # prompt exists.
    press = squash(live(function(MENU, "static qboolean M_MousePress (void)\n{")))
    assert press == ("static qboolean M_MousePress (void) { m_grab = MGRAB_NONE; "
                     "if (m_state == m_mods) return M_Mods_MousePress (); "
                     "return M_Slider_MousePress (); }"), \
        "M_MousePress changed; it runs before the confirmation's click test"

    # One caller each, so there is no second way in, and nothing else that
    # answers or leaves the prompt.
    for name, uses in (("M_ConfirmKey", 3), ("M_Confirm_ClickKey", 2),
                       ("M_Confirm_HitKey", 3), ("menu_touch_key", 2),
                       ("M_ConfirmAction", 4), ("M_ConfirmDraw", 3),
                       ("m_confirm", 4), ("m_confirm_action", 5),
                       ("m_confirm_prevstate", 4),
                       ("m_confirm_hint_x0", 3), ("m_confirm_hint_x1", 3),
                       ("m_confirm_hint_y", 8)):
        found = len(re.findall(r"\b%s\b" % name, menu))
        assert found == uses, f"menu.c uses {name!r} {found} times, expected {uses}"
    assert "case m_confirm: M_ConfirmDraw (); break;" in menu, \
        "M_Draw's m_confirm case must draw the prompt and do nothing else"
    assert not re.search(r"\bmenu_mouse_buttons\s*%s" % ASSIGN, menu), \
        "menu.c writes menu_mouse_buttons; only in_sdl.c may"
    for path in (MENU, INPUT):
        for line in live((ROOT / path).read_text()).splitlines():
            assert not re.match(r"\s*#\s*(define|undef)\b.*(M_Confirm|m_confirm|menu_touch_key)", line), \
                f"{path} has a macro over the confirmation code: {line.strip()}"

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
