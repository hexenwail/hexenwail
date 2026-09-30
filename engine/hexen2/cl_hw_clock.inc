/*
 * HexenWorld client clock and entity-lerp timebase.  GitHub #308.
 * Shared with the standalone fixture scripts/hw_clock_test.c; the including
 * translation unit supplies qboolean, vec3_t and VectorCopy.
 *
 * On the Hexen II path cl.mtime[] is the server time stamped on every
 * message and CL_LerpPoint drags cl.time back into [mtime[1], mtime[0]+f].
 * HexenWorld messages carry no server time -- hwsv sends svc_time once, at
 * connect, and only for Siege -- so that scheme froze cl.time: 0 forever in
 * deathmatch and coop, and T+0.1 in Siege.  The original HexenWorld client
 * ran cl.time off realtime and never let the network pull it back.
 *
 * Here cl.time only ever moves forward, by CL_AdvanceTime, and every
 * received entity update (svc_packetentities / svc_deltapacketentities,
 * which hwsv writes in the same message as the svc_playerinfo records) is
 * stamped with that same clock.  Entities are drawn one update interval
 * behind the newest one: at the frame an update lands frac starts near 0
 * (the previous state) and reaches 1 (the newest) one interval later, and
 * holds there -- never extrapolates -- if the next update is late.
 */

typedef struct
{
	qboolean	update_pending;	/* an entity update arrived since the last latch */
	double		update_time;	/* cl.time when it arrived */
	/* svc_time minus cl.time.  The original client kept exactly this
	 * (cl_server_time_offset) and read it only for the Siege timelimit on
	 * the status bar.  Recording an offset rather than moving cl.mtime is
	 * what keeps a mid-session svc_time from stepping the clock. */
	double		server_time_offset;
} hwcl_clock_t;

/* Parse time: a complete entity update was read. */
static void HWCL_ClockEntityUpdate (hwcl_clock_t *clock, double now)
{
	clock->update_pending = true;
	clock->update_time = now;
}

static void HWCL_ClockServerTime (hwcl_clock_t *clock, double server_time,
		double now)
{
	clock->server_time_offset = server_time - now;
}

/* Once per rendered frame, before the entities are copied: if an update
 * arrived, shift it into mtime[].  Several updates inside one frame count
 * once; the entity copies only ever see the newest of them.  Returns true
 * when the caller must shift its own msg_origins/msg_angles history. */
static qboolean HWCL_ClockLatch (hwcl_clock_t *clock, double mtime[2])
{
	if (!clock->update_pending)
		return false;
	clock->update_pending = false;
	mtime[1] = mtime[0];
	mtime[0] = clock->update_time;
	return true;
}

/* An entity's msg_origins or msg_angles, [0] newest.  HWCL_ApplyState
 * runs every rendered frame, so the history may only move when an update
 * arrived: shifting it every frame, as the first port did, left [1] == [0]
 * from the second frame on and nothing ever interpolated. */
static void HWCL_ClockPushSample (vec3_t history[2], const vec3_t value,
		qboolean new_update)
{
	if (new_update)
		VectorCopy (history[0], history[1]);
	VectorCopy (value, history[0]);
}

/* The fraction between msg_origins[1] and msg_origins[0] to draw at time
 * now.  Unlike CL_LerpPoint it never writes the clock back. */
static float HWCL_ClockLerpFrac (double now, const double mtime[2],
		qboolean nolerp)
{
	double f = mtime[0] - mtime[1];
	double frac;

	if (nolerp || f <= 0)
		return 1;
	/* Same bound as CL_LerpPoint: after a stall or the first update of a
	 * level, catch up within 0.1 s instead of sliding across the gap. */
	if (f > 0.1)
		f = 0.1;
	frac = (now - mtime[0]) / f;
	if (frac < 0)
		return 0;
	if (frac > 1)
		return 1;
	return (float)frac;
}
