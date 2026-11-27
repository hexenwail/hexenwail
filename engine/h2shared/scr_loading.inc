/*
 * Live loading plaque: a status line and an elapsed-seconds ticker under
 * LOADING, redrawn while the plaque is up.  Textually included by gl_screen.c
 * and the software screen.c, inside their !H2W blocks, so both renderers keep
 * one copy of the throttle and the text.  GitHub #571.
 *
 * The plaque used to be one frame: SCR_BeginLoadingPlaque drew it and set
 * scr_disabled_for_loading, after which SCR_UpdateScreen drew nothing until
 * signon finished.  A remote join (handshake, server info, precache, signon,
 * HexenWorld downloads) sat on that one frame for many seconds.
 *
 * Vanilla had progress bars for this (DRAW_PROGRESSBARS), drawn by
 * D_ShowLoadingSize straight into GL_FRONT mid-load.  A double-buffered GL 4.3
 * core context, GLES / WebGL2 and SDL_GPU have no front-buffer drawing, so
 * those stay compiled out; this redraws whole frames through the normal 2D
 * path instead.
 */

/* No live frame until the plaque has been up this long: a quick local map
 * load keeps its single plaque frame, pixel for pixel. */
#define SCR_LOADING_GRACE	0.5
/* At most ten live frames a second. */
#define SCR_LOADING_INTERVAL	0.1
/* And at most this share of the load's wall-clock time.  Inside a blocking
 * load every live frame is time taken from the load, and what a frame costs
 * depends on the renderer: about a millisecond on a GPU, tens of them under
 * llvmpipe or behind a vsync wait.  So the gap after each frame is sized from
 * what that frame took, and a slow renderer gets fewer frames, not a slower
 * load. */
#define SCR_LOADING_MAXSHARE	0.05
/* But never a longer gap than this: one hitch (a first-use pipeline, a vsync
 * stall) must not freeze the clock for seconds.  The cost the gap is sized
 * from is also smoothed over frames, for the same reason. */
#define SCR_LOADING_MAXGAP	2.0

static char	scr_loadstatus[64];
/* Sys_DoubleTime, not realtime: realtime only advances in Host_Frame, and
 * the precache loops that tick this run inside a single frame. */
static double	scr_loadstart;
static double	scr_loaddrawstart;	/* when the current live frame began */
static double	scr_loadnextdraw;	/* no live frame before this */
static double	scr_loaddrawn;		/* seconds spent drawing live frames */
static double	scr_loadcost;		/* smoothed cost of one live frame */
static int	scr_loadframes;
static qboolean	scr_loadinglive;	/* the frame being drawn is a live one */
/* Set by SCR_LoadingBegin only.  scr_disabled_for_loading alone is not a
 * load: vid_restart and Draw_ReInit set it as a draw-nothing guard. */
static qboolean	scr_loadactive;
/* Inside SCR_LoadingTick, i.e. mid-load.  The load timeout must not fire
 * there: dropping the plaque mid-precache would let the next Con_Printf
 * render a half-built world. */
static qboolean	scr_loadingintick;

void SCR_SetLoadingStatus (const char *fmt, ...)
{
	char	text[sizeof(scr_loadstatus)];
	va_list	argptr;
	const char	*a, *b;

	va_start (argptr, fmt);
	q_vsnprintf (text, sizeof(text), fmt, argptr);
	va_end (argptr);
	if (!strcmp (text, scr_loadstatus))
		return;
	/* One log line per stage, not one per model: a change in the digits
	 * alone is a counter moving. */
	for (a = text, b = scr_loadstatus; ; a++, b++)
	{
		while (*a >= '0' && *a <= '9')
			a++;
		while (*b >= '0' && *b <= '9')
			b++;
		if (*a != *b)
		{
			Con_DPrintf ("loading: %s\n", text);
			break;
		}
		if (!*a)
			break;
	}
	q_strlcpy (scr_loadstatus, text, sizeof(scr_loadstatus));
	/* Progress restarts the 25 s "load timeout": it is there for a load that
	 * has stopped, and a long download or precache has not. */
	scr_disabled_time = Sys_DoubleTime ();
}

/* For a blocking loop, between its steps.  Draws only if the plaque is up and
 * a live frame is due, so it is free to call from anywhere outside a draw. */
void SCR_LoadingTick (void)
{
	if (!scr_disabled_for_loading || block_drawing || scr_loadingintick)
		return;
	scr_loadingintick = true;
	SCR_UpdateScreen ();
	scr_loadingintick = false;
}

/* SCR_BeginLoadingPlaque, before its frame.  A second Begin while the plaque
 * is already up (Host_Connect_f, then Host_Reconnect_f) keeps the stage and
 * the clock it already has. */
static void SCR_LoadingBegin (void)
{
	if (scr_disabled_for_loading && scr_loadactive)
		return;
	scr_loadactive = true;
	scr_loadstatus[0] = 0;
	scr_loadstart = Sys_DoubleTime ();
	scr_loadnextdraw = 0;
	scr_loaddrawn = 0;
	scr_loadcost = 0;
	scr_loadframes = 0;
}

static void SCR_LoadingEnd (void)
{
	double	took;

	/* What the live frames cost this load: the evidence for MAXSHARE. */
	if (scr_loadframes)
	{
		took = Sys_DoubleTime () - scr_loadstart;
		Con_DPrintf ("loading: done in %.2f s, %d live frames, %.0f ms drawing them (%.1f%%)\n",
				took, scr_loadframes, scr_loaddrawn * 1000.0,
				(took > 0) ? scr_loaddrawn * 100.0 / took : 0.0);
	}
	scr_loadframes = 0;
	scr_loadstatus[0] = 0;
	scr_loadactive = false;
	/* Host_Error can longjmp out of a live frame before SCR_UpdateScreen
	 * restores these; it ends the plaque first, so reset them here. */
	scr_loadinglive = false;
	scr_drawloading = false;
}

/* SCR_UpdateScreen with the plaque up: draw a live frame now, or not. */
static qboolean SCR_LoadingFrameDue (void)
{
	double	now = Sys_DoubleTime ();

	if (!scr_loadactive || now - scr_loadstart < SCR_LOADING_GRACE ||
	    now < scr_loadnextdraw)
		return false;
	scr_loaddrawstart = now;
	return true;
}

/* After a live frame: space the next one by what this one cost. */
static void SCR_LoadingFrameDone (void)
{
	double	cost = Sys_DoubleTime () - scr_loaddrawstart;
	double	gap;

	scr_loaddrawn += cost;
	scr_loadcost = scr_loadframes ? 0.5 * (scr_loadcost + cost) : cost;
	scr_loadframes++;

	gap = scr_loadcost / SCR_LOADING_MAXSHARE;
	if (gap < SCR_LOADING_INTERVAL)
		gap = SCR_LOADING_INTERVAL;
	if (gap > SCR_LOADING_MAXGAP)
		gap = SCR_LOADING_MAXGAP;
	scr_loadnextdraw = scr_loaddrawstart + gap;
}

/* Under the plaque, on live frames only, so the plaque's first frame is the
 * same as it always was.  Two centred lines, the stage and the elapsed time,
 * on a dark box: what is behind them is the busy HEXEN II logo. */
static void SCR_DrawLoadingStatus (int y)
{
	char	status[sizeof(scr_loadstatus)];
	char	clock[16];
	size_t	cols = (vid.width > 16) ? (size_t)(vid.width / 8) - 1 : 1;
	size_t	len;
	int	elapsed, w;

	if (!scr_loadinglive)
		return;
	q_strlcpy (status, scr_loadstatus, (cols + 1 < sizeof(status)) ? cols + 1 : sizeof(status));
	elapsed = (int)(Sys_DoubleTime () - scr_loadstart);
	q_snprintf (clock, sizeof(clock), "%d:%02d", elapsed / 60, elapsed % 60);

	len = (strlen (status) > strlen (clock)) ? strlen (status) : strlen (clock);
	w = (int)len * 8 + 8;
#if defined(GLQUAKE) || defined(WEBQUAKE)
	Draw_FillAlpha ((vid.width - w) / 2, y - 3, w, 26, 0, 0, 0, 0.6f);
#endif
	if (status[0])
		Draw_String ((vid.width - (int)strlen (status) * 8) / 2, y, status);
	Draw_String ((vid.width - (int)strlen (clock) * 8) / 2, y + 12, clock);
}
