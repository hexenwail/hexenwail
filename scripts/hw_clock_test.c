/*
 * Unit tests for the HexenWorld client clock and entity-lerp timebase
 * (engine/hexen2/cl_hw_clock.inc).  GitHub #308.
 *
 * The bug: cl.time never advanced in a HexenWorld session (0.00 in
 * deathmatch and coop, pinned at T+0.1 in Siege), so nothing timed off it
 * expired or animated, and entities snapped between updates.  This drives
 * the .inc through the engine's own per-frame order for a HexenWorld
 * session --
 *
 *     HWCL_Frame       parse whatever arrived: HWCL_ClockEntityUpdate
 *     CL_AdvanceTime   cl.time += host_frametime
 *     HWCL_ApplyState  HWCL_ClockLatch, then HWCL_ClockPushSample per entity
 *     CL_RelinkEntities  origin = [1] + HWCL_LerpPoint () * ([0] - [1])
 *
 * -- against a simulated server moving one entity at constant speed, and
 * checks that the clock moves, that the lerp fraction stays in [0, 1], and
 * that the entity is drawn moving smoothly rather than snapping.
 *
 * Build and run:
 *     cc -Wall -Wextra -o /tmp/hwclocktest scripts/hw_clock_test.c -lm && /tmp/hwclocktest
 * Exit status 0 on pass, 1 on any failed assertion.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int qboolean;
#define true	1
#define false	0
typedef float vec_t;
typedef vec_t vec3_t[3];
#define VectorCopy(a,b)	((b)[0] = (a)[0], (b)[1] = (a)[1], (b)[2] = (a)[2])

#include "../engine/hexen2/cl_hw_clock.inc"

static int checks, failures;

static void check (int condition, const char *format, ...)
{
	va_list args;

	checks++;
	if (condition)
		return;
	failures++;
	fprintf (stderr, "FAIL: ");
	va_start (args, format);
	vfprintf (stderr, format, args);
	va_end (args);
	fputc ('\n', stderr);
}

#define CHECK(condition, ...) check ((condition), __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* a HexenWorld session in miniature                                   */
/* ------------------------------------------------------------------ */

#define SPEED	320.0	/* units per second along x: a running player */
#define MAX_SENT	4096

typedef struct
{
	/* client */
	double		realtime;
	double		cl_time;
	double		mtime[2];
	hwcl_clock_t	clock;
	vec3_t		msg_origins[2];
	qboolean	was_on;
	float		frac;
	float		drawn;		/* x drawn this frame */
	/* newest state the parser has, as HWCL_ParsePacketEntities leaves it */
	vec3_t		server_state;
	qboolean	have_state;
	/* server: a queue of updates in flight */
	double		arrive[MAX_SENT];
	float		x[MAX_SENT];
	int		sent, delivered;
} session_t;

static void session_init (session_t *s)
{
	memset (s, 0, sizeof(*s));
}

/* The server writes one update, of the entity at x, arriving at the client
 * at the given realtime. */
static void server_send (session_t *s, float x, double arrive)
{
	if (s->sent == MAX_SENT)
		return;
	s->x[s->sent] = x;
	s->arrive[s->sent] = arrive;
	s->sent++;
}

/* One host frame of a HexenWorld session, in _Host_Frame's order. */
static void client_frame (session_t *s, double frametime)
{
	qboolean new_update;

	s->realtime += frametime;

	/* HWCL_Frame: every packet that has arrived by now. */
	while (s->delivered < s->sent && s->arrive[s->delivered] <= s->realtime)
	{
		s->server_state[0] = s->x[s->delivered];
		s->have_state = true;
		HWCL_ClockEntityUpdate (&s->clock, s->cl_time);
		s->delivered++;
	}

	/* CL_AdvanceTime */
	s->cl_time += frametime;

	/* HWCL_ApplyState */
	new_update = HWCL_ClockLatch (&s->clock, s->mtime);
	if (s->have_state)
	{
		HWCL_ClockPushSample (s->msg_origins, s->server_state,
				s->was_on && new_update);
		/* First sight: CopyEntity sets forcelink, and relink snaps. */
		if (!s->was_on)
			VectorCopy (s->msg_origins[0], s->msg_origins[1]);
		s->was_on = true;
	}

	/* CL_RelinkEntities */
	s->frac = HWCL_ClockLerpFrac (s->cl_time, s->mtime, false);
	s->drawn = s->msg_origins[1][0] +
		s->frac * (s->msg_origins[0][0] - s->msg_origins[1][0]);
}

/* ------------------------------------------------------------------ */
/* cases                                                               */
/* ------------------------------------------------------------------ */

/* Deathmatch/coop: no svc_time at all, only entity updates.  cl.time must
 * run at wall-clock rate, mtime must follow the updates, and frac must stay
 * in [0, 1] -- the issue's cl.time=0.00 fourteen seconds after connect. */
static void test_clock_advances_without_svc_time (void)
{
	session_t s;
	const double ft = 1.0 / 60.0;
	double last_time = -1, last_m0 = 0;
	int frame, bad_frac = 0, backwards = 0, mtime_back = 0;
	double t;

	session_init (&s);
	for (t = 0.0; t < 20.0; t += 1.0 / 72.0)
		server_send (&s, (float)(SPEED * t), t + 0.030);

	for (frame = 0; frame < 60 * 19; frame++)
	{
		client_frame (&s, ft);
		if (!(s.cl_time > last_time))
			backwards++;
		if (s.frac < 0 || s.frac > 1)
			bad_frac++;
		if (s.mtime[0] < last_m0 || s.mtime[1] > s.mtime[0])
			mtime_back++;
		last_time = s.cl_time;
		last_m0 = s.mtime[0];
	}

	CHECK (fabs (s.cl_time - 19.0) < 1e-6,
		"cl.time is %.4f after 19 s of frames, want 19.0000", s.cl_time);
	CHECK (backwards == 0, "cl.time failed to advance on %d frame(s)", backwards);
	CHECK (bad_frac == 0, "lerp fraction left [0,1] on %d frame(s)", bad_frac);
	CHECK (mtime_back == 0, "mtime went backwards on %d frame(s)", mtime_back);
	CHECK (s.mtime[0] > 18.9 && s.mtime[0] <= s.cl_time,
		"mtime[0] %.4f does not follow the updates (cl.time %.4f)",
		s.mtime[0], s.cl_time);
	CHECK (s.mtime[0] - s.mtime[1] > 0.005 && s.mtime[0] - s.mtime[1] < 0.05,
		"mtime interval %.4f is not an update interval",
		s.mtime[0] - s.mtime[1]);
}

/* Updates slower than frames (a 20 Hz server, a 144 Hz display).  The
 * entity must be drawn moving a little every frame, never backwards, and at
 * most one update interval (plus latency and a frame) behind the server.
 * A frozen clock pins frac, and shifting the history every frame leaves
 * [1] == [0]: either way the entity sits still and then jumps a whole
 * update, which is what "entities snap between updates" looked like. */
static void test_entities_interpolate (void)
{
	session_t s;
	const double ft = 1.0 / 144.0;
	const double interval = 0.05, latency = 0.040;
	const double step_limit = 3.0 * SPEED * ft;
	double worst_step = 0, worst_lag = 0;
	float last = 0;
	int frame, partial = 0, backwards = 0;
	double t;

	session_init (&s);
	for (t = 0.0; t < 10.0; t += interval)
		server_send (&s, (float)(SPEED * t), t + latency);

	for (frame = 0; frame < 144 * 9; frame++)
	{
		client_frame (&s, ft);
		if (s.realtime < 1.0)
		{	/* let the first-update catch-up settle */
			last = s.drawn;
			continue;
		}
		if (s.frac > 0.01f && s.frac < 0.99f)
			partial++;
		if (s.drawn < last - 1e-3f)
			backwards++;
		if (s.drawn - last > worst_step)
			worst_step = s.drawn - last;
		/* where the server has the entity now, minus what we draw */
		if (SPEED * s.realtime - s.drawn > worst_lag)
			worst_lag = SPEED * s.realtime - s.drawn;
		last = s.drawn;
	}

	CHECK (partial > 144 * 4,
		"only %d frames drew an in-between position: entities snap", partial);
	CHECK (backwards == 0, "entity drawn moving backwards on %d frame(s)", backwards);
	CHECK (worst_step <= step_limit,
		"entity jumped %.2f units in one frame (limit %.2f): no interpolation",
		worst_step, step_limit);
	CHECK (worst_lag <= SPEED * (interval + latency + 2 * ft) + 1,
		"entity drawn %.2f units behind, more than one update interval",
		worst_lag);
}

/* Jittered arrivals at about the frame rate: whatever the spacing, frac
 * stays in range and the drawn position never runs backwards. */
static void test_jittered_updates (void)
{
	session_t s;
	unsigned seed = 12345;
	double t = 0, ft;
	float last = 0;
	int frame, bad_frac = 0, backwards = 0;

	session_init (&s);
	while (t < 10.0)
	{
		seed = seed * 1103515245u + 12345u;
		t += 0.005 + ((seed >> 16) % 30) * 0.001;	/* 5..34 ms */
		server_send (&s, (float)(SPEED * t),
				t + 0.020 + ((seed >> 8) % 3) * 0.001);
	}
	for (frame = 0; frame < 900; frame++)
	{
		seed = seed * 1103515245u + 12345u;
		ft = 0.004 + ((seed >> 16) % 16) * 0.001;	/* 4..19 ms */
		client_frame (&s, ft);
		if (s.frac < 0 || s.frac > 1)
			bad_frac++;
		if (frame > 10 && s.drawn < last - 1e-3f)
			backwards++;
		last = s.drawn;
	}
	CHECK (bad_frac == 0, "jitter: frac left [0,1] on %d frame(s)", bad_frac);
	CHECK (backwards == 0, "jitter: entity drawn backwards on %d frame(s)", backwards);
}

/* A late update holds the newest state; it must never extrapolate past
 * it (CL_LerpPoint allows frac up to 2, a whole interval of overshoot). */
static void test_stall_holds_newest (void)
{
	session_t s;
	const double ft = 1.0 / 144.0;
	int frame, i, overshoot = 0;

	session_init (&s);
	for (i = 0; i <= 40; i++)
		server_send (&s, (float)(SPEED * i * 0.05), i * 0.05 + 0.03);
	/* nothing more: the server stalls */
	for (frame = 0; frame < 144 * 4; frame++)
	{
		client_frame (&s, ft);
		if (s.drawn > s.msg_origins[0][0] + 1e-3f)
			overshoot++;
	}
	CHECK (overshoot == 0, "entity drawn past the newest update on %d frame(s)",
		overshoot);
	CHECK (s.frac == 1, "after a stall frac is %.3f, want 1", s.frac);
	CHECK (fabs (s.drawn - SPEED * 2.0) < 0.5,
		"after a stall entity drawn at %.2f, want the last update %.2f",
		s.drawn, SPEED * 2.0);
	CHECK (s.cl_time > 3.99, "cl.time %.3f stopped while the server stalled",
		s.cl_time);
}

/* Siege: hwsv sends svc_time once at connect.  It used to be written into
 * mtime[0], after which CL_LerpPoint pinned cl.time at T+0.1.  It must now
 * leave the clock and mtime alone, before or after entity updates, and
 * whether the server time is ahead of or behind cl.time. */
static void test_svc_time_is_not_a_clock_step (void)
{
	session_t s;
	double before_time, before_m0, before_m1;
	int frame;
	double t;

	session_init (&s);
	for (t = 0.0; t < 6.0; t += 1.0 / 72.0)
		server_send (&s, (float)(SPEED * t), t + 0.03);

	client_frame (&s, 1.0 / 60.0);
	HWCL_ClockServerTime (&s.clock, 1234.5, s.cl_time);	/* at connect */
	CHECK (fabs (s.clock.server_time_offset - (1234.5 - s.cl_time)) < 1e-9,
		"svc_time offset %.4f, want %.4f", s.clock.server_time_offset,
		1234.5 - s.cl_time);

	for (frame = 0; frame < 120; frame++)
		client_frame (&s, 1.0 / 60.0);
	before_time = s.cl_time;
	before_m0 = s.mtime[0];
	before_m1 = s.mtime[1];
	HWCL_ClockServerTime (&s.clock, 0.25, s.cl_time);	/* behind: a resync */
	CHECK (s.cl_time == before_time && s.mtime[0] == before_m0 &&
			s.mtime[1] == before_m1,
		"svc_time moved the clock: time %.4f->%.4f mtime0 %.4f->%.4f",
		before_time, s.cl_time, before_m0, s.mtime[0]);

	for (frame = 0; frame < 120; frame++)
		client_frame (&s, 1.0 / 60.0);
	CHECK (s.cl_time > before_time + 1.99,
		"cl.time %.4f froze after svc_time (was %.4f)", s.cl_time, before_time);
	CHECK (s.mtime[0] > before_m0 + 1.9,
		"mtime[0] %.4f froze after svc_time (was %.4f)", s.mtime[0], before_m0);
}

/* Several updates inside one rendered frame shift mtime once; a frame with
 * no update does not shift it at all. */
static void test_latch_once_per_frame (void)
{
	hwcl_clock_t clock;
	double mtime[2] = { 3.0, 2.9 };

	memset (&clock, 0, sizeof(clock));
	CHECK (!HWCL_ClockLatch (&clock, mtime) && mtime[0] == 3.0 && mtime[1] == 2.9,
		"latch without an update moved mtime to %.3f/%.3f", mtime[0], mtime[1]);

	HWCL_ClockEntityUpdate (&clock, 3.1);
	HWCL_ClockEntityUpdate (&clock, 3.1);
	CHECK (HWCL_ClockLatch (&clock, mtime), "latch missed a pending update");
	CHECK (mtime[0] == 3.1 && mtime[1] == 3.0,
		"two updates in one frame gave mtime %.3f/%.3f, want 3.100/3.000",
		mtime[0], mtime[1]);
	CHECK (!HWCL_ClockLatch (&clock, mtime) && mtime[1] == 3.0,
		"a latched update latched twice");
}

static void test_lerp_frac_edges (void)
{
	double level_start[2] = { 5.0, 0.0 };	/* first update after CL_ClearState */
	double same[2] = { 2.0, 2.0 };
	double normal[2] = { 1.05, 1.0 };

	CHECK (HWCL_ClockLerpFrac (5.05, level_start, false) == 0.5f,
		"first update of a level: gap not capped at 0.1 (frac %.3f)",
		HWCL_ClockLerpFrac (5.05, level_start, false));
	CHECK (HWCL_ClockLerpFrac (2.5, same, false) == 1,
		"no interval yet: frac is not 1");
	CHECK (HWCL_ClockLerpFrac (1.0, normal, false) == 0,
		"clock behind the newest update: frac is not clamped to 0");
	CHECK (HWCL_ClockLerpFrac (1.075, normal, false) > 0.49f &&
			HWCL_ClockLerpFrac (1.075, normal, false) < 0.51f,
		"half an interval on: frac %.3f, want 0.5",
		HWCL_ClockLerpFrac (1.075, normal, false));
	CHECK (HWCL_ClockLerpFrac (1.075, normal, true) == 1,
		"cl_nolerp: frac is not 1");
}

int main (void)
{
	test_clock_advances_without_svc_time ();
	test_entities_interpolate ();
	test_jittered_updates ();
	test_stall_holds_newest ();
	test_svc_time_is_not_a_clock_step ();
	test_latch_once_per_frame ();
	test_lerp_frac_edges ();

	printf ("hw_clock_test: %d checks, %d failure(s)\n", checks, failures);
	return failures != 0;
}
