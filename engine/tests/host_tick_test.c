#include <stdio.h>

#include "host_tick.h"

/*
 * Host_PlanTicks is the whole per-frame tick decision in _Host_Frame, so this
 * drives it frame by frame the way the host does.  Issue #139: host_framerate
 * must advance game time by exactly its value every render frame.
 */

#define INTERVAL_72HZ	(1.0 / 72.0)
#define EPSILON		1e-9

static int failures;

static void Check (int ok, const char *what)
{
	if (!ok)
	{
		fprintf(stderr, "host_tick: FAIL: %s\n", what);
		failures++;
	}
}

static double Abs (double x)
{
	return x < 0 ? -x : x;
}

/* Every frame runs, and the steps sum to exactly frametime. */
static void CheckFixed (double frametime, int expect_ticks, const char *what)
{
	double			accum = 0.005;
	host_tick_plan_t	plan;

	plan = Host_PlanTicks(&accum, frametime, INTERVAL_72HZ, 1);
	if (plan.ticks != expect_ticks)
		fprintf(stderr, "host_tick: %s: %d ticks, expected %d\n",
			what, plan.ticks, expect_ticks);
	Check(plan.ticks == expect_ticks, what);
	Check(Abs(plan.ticks * plan.step - frametime) < EPSILON, what);
	Check(accum == 0.005, "fixed step leaves the accumulator alone");
}

int main (void)
{
	double			accum, sv_time;
	host_tick_plan_t	plan;
	int			frame, idle_frames, ticks;

	/* The issue's case: 0.01 per frame for 100 frames is exactly 1s of game
	 * time, one server frame per render frame, never an idle frame. */
	accum = 0;
	sv_time = 0;
	idle_frames = 0;
	for (frame = 0; frame < 100; frame++)
	{
		plan = Host_PlanTicks(&accum, 0.01, INTERVAL_72HZ, 1);
		if (plan.ticks != 1)
			idle_frames++;
		sv_time += plan.ticks * plan.step;
	}
	Check(idle_frames == 0, "host_framerate 0.01 runs one tick every frame");
	Check(Abs(sv_time - 1.0) < EPSILON, "host_framerate 0.01 x 100 frames == 1.0s");

	/* The same input through the accumulator is what the bug looked like:
	 * quantised to 1/72 with idle frames, and short of frames*0.01.  90
	 * frames, because 100 happens to land on a whole 72nd. */
	accum = 0;
	sv_time = 0;
	idle_frames = 0;
	for (frame = 0; frame < 90; frame++)
	{
		plan = Host_PlanTicks(&accum, 0.01, INTERVAL_72HZ, 0);
		if (plan.ticks == 0)
			idle_frames++;
		sv_time += plan.ticks * plan.step;
	}
	Check(idle_frames > 0, "accumulated 0.01 frames idle some frames (control)");
	Check(sv_time < 0.9 - EPSILON, "accumulated 0.01 x 90 lags 0.9s (control)");

	/* Substeps: nothing longer than the longest tick reaches SV_Physics,
	 * including the floating-point edges of the division. */
	CheckFixed(0.001, 1, "tiny step is one tick");
	CheckFixed(0.1, 1, "0.1 is one tick");
	CheckFixed(0.2, 2, "0.2 is two ticks");
	CheckFixed(0.25, 3, "0.25 is three ticks");
	CheckFixed(0.3, 3, "0.3 is three ticks, not two");
	CheckFixed(1.0, 10, "1.0 is ten ticks");
	/* Past the substep budget the step grows rather than the frame hanging;
	 * total game time is still exact. */
	CheckFixed(50.0, HOST_TICK_MAX_SUBSTEPS, "50.0 is capped at the substep budget");

	/* host_framerate 0: the accumulator path is unchanged.  One real second
	 * at 60 fps is 72 ticks at 72 Hz, give or take the float residue, and no
	 * game time is lost. */
	accum = 0;
	ticks = 0;
	for (frame = 0; frame < 60; frame++)
	{
		plan = Host_PlanTicks(&accum, 1.0 / 60.0, INTERVAL_72HZ, 0);
		Check(plan.step == INTERVAL_72HZ, "accumulated step is the interval");
		ticks += plan.ticks;
	}
	Check(ticks == 71 || ticks == 72, "60 fps for 1s runs 72 ticks");
	Check(Abs(ticks * INTERVAL_72HZ + accum - 1.0) < EPSILON,
	      "accumulator conserves game time");

	/* A hitch is capped just above one clamped frame, as before. */
	accum = 0;
	plan = Host_PlanTicks(&accum, 0.5, INTERVAL_72HZ, 0);
	Check(plan.ticks == 8, "a 0.5s hitch catches up at most 8 ticks at 72 Hz");

	if (failures)
		return 1;
	printf("host_tick: all cases passed\n");
	return 0;
}
