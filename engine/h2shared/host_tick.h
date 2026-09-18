/*
 * host_tick.h -- how many server ticks a render frame runs, and how long each is
 *
 * Header-only and dependency-free so engine/tests/host_tick_test.c can drive
 * the exact decision _Host_Frame makes without linking the host.
 */
#ifndef H2_HOST_TICK_H
#define H2_HOST_TICK_H

/* The longest tick the engine ever simulates: sv_physfps is clamped to >= 10,
 * and Host_FilterTime clamps a real frame to the same 0.1s. */
#define HOST_TICK_MAX_STEP		0.1
/* Bounds the work one render frame can queue.  Past HOST_TICK_MAX_STEP *
 * this (1s of game time per frame) the step grows instead, so an absurd
 * host_framerate costs a coarse simulation rather than a hung frame. */
#define HOST_TICK_MAX_SUBSTEPS		10

typedef struct
{
	int	ticks;	/* server frames to run this render frame */
	double	step;	/* host_frametime for each of them */
} host_tick_plan_t;

/*
 * frametime is host_frametime as Host_FilterTime left it.  fixed_step is true
 * when host_framerate is set.
 *
 * Fixed step: host_framerate means "every render frame advances game time by
 * exactly this much", so the frame bypasses the accumulator and advances by
 * precisely frametime.  Fed through the accumulator it was quantised to the
 * physics interval (0.01 at 72 Hz ran a whole 1/72s tick on most frames and
 * none on the rest) and silently capped at ~0.11 per frame.  A step longer than
 * any tick the engine otherwise runs is split into equal substeps, as Raven's
 * FPS_20 Host_ServerFrame did, rather than handed to SV_Physics whole.  The
 * accumulator is left untouched so turning the cvar off resumes cleanly.
 *
 * Accumulated: the fixed-timestep path, unchanged.
 */
static inline host_tick_plan_t Host_PlanTicks (double *accum, double frametime,
					       double interval, int fixed_step)
{
	host_tick_plan_t	plan;

	if (fixed_step)
	{
		plan.ticks = (int)(frametime / HOST_TICK_MAX_STEP);
		if (plan.ticks * HOST_TICK_MAX_STEP < frametime)
			plan.ticks++;
		if (plan.ticks < 1)
			plan.ticks = 1;
		if (plan.ticks > HOST_TICK_MAX_SUBSTEPS)
			plan.ticks = HOST_TICK_MAX_SUBSTEPS;
		plan.step = frametime / plan.ticks;
		return plan;
	}

	*accum += frametime;
	/* Host_FilterTime already clamps one frame's contribution to 0.1s.  Cap
	 * just above it: cutting closer would silently dilate game time on every
	 * hitch, and this still bounds one frame's catch-up (~9 ticks at 72 Hz),
	 * which drains more simulated time than a clamped frame can add. */
	if (*accum > HOST_TICK_MAX_STEP + interval)
		*accum = HOST_TICK_MAX_STEP + interval;

	plan.ticks = 0;
	plan.step = interval;
	while (*accum >= interval)
	{
		*accum -= interval;
		plan.ticks++;
	}
	return plan;
}

#endif /* H2_HOST_TICK_H */
