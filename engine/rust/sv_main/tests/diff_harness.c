/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diff_harness.c -- engine/rust/src/sv_main_sound.rs against the C original
 * of Hexen II's sv_main.c sound and particle messages, in one binary.
 *
 * run_diff_harness.sh builds this against sv_main_sound_h2.c (-DSERVERONLY,
 * as h2ded compiles it), with the real engine/rust/world_target.c and
 * engine/rust/sv_main_target.c compiled in.
 *
 * Each case sets up what these functions read -- sv_protocol, sv.datagram
 * (its contents, cursize, maxsize, allowoverflow and overflowed flags),
 * sv.sound_precache, the entity's origin, mins and maxs -- then calls one of
 * the ten exported functions once on the C and once on the Rust, from the
 * same snapshot, and compares byte for byte: the whole datagram buffer,
 * cursize and overflowed, the return value, and every console line with its
 * print level.
 *
 * The inputs sit on the edges the code tests:
 *   - cursize at each message's MAX_DATAGRAM - room threshold, +/-2;
 *   - every protocol the limits name (18..21, 100) and the values between
 *     and around them;
 *   - entities 8191, 8192 and 8193, where (ent<<3)|channel stops fitting;
 *   - precache lists of every length a split threshold reaches (255..257,
 *     511..513, 767..769, full with no terminator), samples at those
 *     indices, duplicates, the unprecached, misc/null.wav in any case;
 *   - volume 255 and not, attenuation exactly 1.0, the ulp either side,
 *     0, 4, NaN, and out of range; channels 0..7 and outside;
 *   - directions at 127/16 and -128/16 and the ulp beyond, box and radius
 *     bytes past 255 and negative, NaN and infinities everywhere a float
 *     goes, ints past a byte and a short.
 *   - a datagram whose maxsize is too small for the message, with and
 *     without allowoverflow, so the shared SZ_GetSpace's overflow paths run
 *     under both callers.
 *
 * Host_Error and Sys_Error do not return.  A case either can reach runs
 * forked, once per side, and the child reports its log and the state it
 * reached; a fatal call in an in-process case is a harness bug and aborts.
 *
 * WHAT THE REFERENCE CALLS.  MSG_Write* and SZ_GetSpace are the
 * already-ported Rust, held to their C by their own harnesses; NUM_FOR_EDICT
 * and q_strcasecmp are copied below; strcmp is the C library's.  So this
 * compares sv_main.c's own logic over shared callees.
 */

#include "quakedef.h"
#include "q_ctype.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void	c_SV_StartParticle (vec3_t org, vec3_t dir, int color, int count);
void	c_SV_StartParticle2 (vec3_t org, vec3_t dmin, vec3_t dmax, int color, int effect, int count);
void	c_SV_StartParticle3 (vec3_t org, vec3_t box, int color, int effect, int count);
void	c_SV_StartParticle4 (vec3_t org, float radius, int color, int effect, int count);
void	c_SV_StopSound (edict_t *entity, int channel);
void	c_SV_UpdateSoundPos (edict_t *entity, int channel);
void	c_SV_StartSound (edict_t *entity, int channel, const char *sample, int volume, float attenuation);
int	c_SV_MaxSounds (void);
int	c_SV_MaxDatagram (void);
int	c_SV_MaxReliableMessage (void);

/* the engine's storage */
server_t	sv;
int		sv_protocol;
int		pr_edict_size = sizeof (edict_t);
sv_globals_t	sv_globals;
areanode_t	sv_areanodes[AREA_NODES];
sizebuf_t	net_message;	/* msg_io.rs's readers; nothing here reads */

/* sv_main_target.c is one translation unit for every sv_main.rs slice, so
 * #469 slice B's entity-update accessors are compiled in beside slice A's
 * sound ones and their globals must exist to link.  Nothing here reads
 * them; ents_harness.c sets them up for the slice that does. */
server_static_t	svs;
cvar_t	sv_update_player = { "sv_update_player", "1", CVAR_NONE };
cvar_t	sv_update_monsters = { "sv_update_monsters", "1", CVAR_NONE };
cvar_t	sv_update_missiles = { "sv_update_missiles", "1", CVAR_NONE };
cvar_t	sv_update_misc = { "sv_update_misc", "1", CVAR_NONE };
cvar_t	sv_netsort = { "sv_netsort", "1", CVAR_NONE };

/* MAX_EDICTS + 1: every entity the engine can have, so the large forms are
 * held to the whole short range they write, not just its start */
#define NE	(MAX_EDICTS + 1)

/* ------------------------------------------------------------------------ */

static uint64_t	rng = 0x9e3779b97f4a7c15ULL;

static uint32_t rnd (void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (uint32_t) ((rng * 0x2545f4914f6cdd1dULL) >> 32);
}

static int chance (int pct)	{ return (int) (rnd () % 100) < pct; }
static float frand (float lo, float hi)	{ return lo + (hi - lo) * ((float) rnd () / 4294967296.0f); }

/* ------------------------------------------------------------------------ */
/* the log, and the fatal calls                                             */
/* ------------------------------------------------------------------------ */

static char	logbuf[1 << 16];
static size_t	loglen;
static int	in_child = -1;	/* the pipe's write end in a forked case */

static void logv (const char *tag, const char *fmt, va_list ap)
{
	int	n;
	size_t	start = loglen, r, w;
	n = snprintf (logbuf + loglen, sizeof (logbuf) - loglen, "%s|", tag);
	if (n > 0)
		loglen += (size_t) n;
	n = vsnprintf (logbuf + loglen, sizeof (logbuf) - loglen, fmt, ap);
	if (n > 0)
		loglen += (size_t) n;
	if (loglen < sizeof (logbuf) - 1)
		logbuf[loglen++] = '\n';
	if (loglen >= sizeof (logbuf))
		loglen = sizeof (logbuf) - 1;
	/* the reference's __func__ is its renamed name: c_SV_StartSound reads
	 * as SV_StartSound */
	for (r = w = start; r < loglen; )
	{
		if (loglen - r >= 5 && !memcmp (logbuf + r, "c_SV_", 5))
			r += 2;
		logbuf[w++] = logbuf[r++];
	}
	loglen = w;
}

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list	ap;
	char	tag[16];
	snprintf (tag, sizeof (tag), "con%u", flags);
	va_start (ap, fmt);
	logv (tag, fmt, ap);
	va_end (ap);
}

static void report_and_exit (void);

static FUNC_NORETURN void fatal (const char *tag, const char *fmt, va_list ap)
{
	logv (tag, fmt, ap);
	if (in_child >= 0)
		report_and_exit ();
	fprintf (stderr, "harness bug: %.*s", (int) loglen, logbuf);
	abort ();
}

FUNC_NORETURN void Host_Error (const char *error, ...)
{
	va_list	ap;
	va_start (ap, error);
	fatal ("Host_Error", error, ap);
}

FUNC_NORETURN void Sys_Error (const char *error, ...)
{
	va_list	ap;
	va_start (ap, error);
	fatal ("Sys_Error", error, ap);
}

void *Hunk_AllocName (int size, const char *name)
{
	(void) size; (void) name;
	fprintf (stderr, "harness bug: Hunk_AllocName\n");
	abort ();
}

/* the archive's world_hull objects; nothing here traces */
const char *PR_GetString (int num)	{ (void) num; return ""; }
eval_t *GetEdictFieldValue (edict_t *ed, const char *field)	{ (void) ed; (void) field; return NULL; }
void PR_ExecuteProgram (func_t fnum, const char *funcname)	{ (void) fnum; (void) funcname; abort (); }

/* pr_edict.c's NUM_FOR_EDICT, without its bad-pointer report: every case
 * passes a real edict */
int NUM_FOR_EDICT (edict_t *e)
{
	int	b = (byte *) e - (byte *) sv.edicts;
	b = b / pr_edict_size;
	if (b < 0 || b >= sv.num_edicts)
	{
		fprintf (stderr, "harness bug: NUM_FOR_EDICT %d\n", b);
		abort ();
	}
	return b;
}

/* common.c's q_strcasecmp, verbatim */
int q_strcasecmp(const char * s1, const char * s2)
{
	const char * p1 = s1;
	const char * p2 = s2;
	char c1, c2;

	if (p1 == p2)
		return 0;

	do
	{
		c1 = q_tolower (*p1++);
		c2 = q_tolower (*p2++);
		if (c1 == '\0')
			break;
	} while (c1 == c2);

	return (int)(c1 - c2);
}

/* ------------------------------------------------------------------------ */
/* the cases                                                                */
/* ------------------------------------------------------------------------ */

enum { F_P1, F_P2, F_P3, F_P4, F_STOP, F_UPDATE, F_SOUND, F_MAXSOUNDS, F_MAXDGRAM, F_MAXREL, F_COUNT };
static const char *fn_name[F_COUNT] = {
	"SV_StartParticle", "SV_StartParticle2", "SV_StartParticle3", "SV_StartParticle4",
	"SV_StopSound", "SV_UpdateSoundPos", "SV_StartSound",
	"SV_MaxSounds", "SV_MaxDatagram", "SV_MaxReliableMessage"
};
/* each message's `MAX_DATAGRAM - room' */
static const int fn_room[F_COUNT] = { 16, 36, 15, 13, 4, 4, 16, 0, 0, 0 };

typedef struct
{
	int	fn;
	vec3_t	org, a, b;
	float	radius, attn;
	int	color, effect, count;
	int	ent, channel, volume;
	char	sample[32];
	int	protocol;
	int	cursize, maxsize, allowoverflow, overflowed;
	int	nprecache;	/* entries 1..n-1 set; n == MAX_SOUNDS: no terminator */
	int	dup;		/* an index whose name repeats an earlier one, or 0 */
	vec3_t	e_origin, e_mins, e_maxs;
} call_t;

typedef struct
{
	int	cursize, overflowed, ret;
	size_t	loglen;
} outcome_t;

static byte		init_buf[sizeof (sv.datagram_buf)];
static char		names[MAX_SOUNDS][16];
static byte		*edict_store;

static edict_t *edict (int n)	{ return (edict_t *) (edict_store + (size_t) n * pr_edict_size); }

static float odd_float (void)
{
	static const float	v[] = { 0.0f, -0.0f, INFINITY, -INFINITY, NAN, -NAN,
		1e9f, -1e9f, 3e38f, -3e38f, 2147483648.0f, -2147483904.0f, 268435456.0f, 1e-40f };
	return v[rnd () % (sizeof (v) / sizeof (v[0]))];
}

static float coordish (void)
{
	if (chance (4))
		return odd_float ();
	if (chance (20))
		return (float) ((int) (rnd () % 65536) - 32768) / 8.0f;
	return frand (-4096, 4096);
}

/* a particle direction: dir*16 at and around the clamps */
static float dirish (void)
{
	static const float	v[] = { 7.9375f, 7.99f, 8.0f, 8.0625f, -8.0f, -8.0625f, -8.125f,
		-7.99f, 0.03125f, -0.0625f, 0.0f, -0.0f };
	if (chance (5))
		return odd_float ();
	if (chance (40))
	{
		float	x = v[rnd () % (sizeof (v) / sizeof (v[0]))];
		if (chance (30))
			x = nextafterf (x, chance (50) ? 1000.0f : -1000.0f);
		return x;
	}
	return frand (-20, 20);
}

/* a byte-sized float: box, radius */
static float byteish (void)
{
	static const float	v[] = { 0, 255, 255.99f, 256, 257, -1, -0.5f, 127.5f, 1000, 65535, 65536 };
	if (chance (5))
		return odd_float ();
	if (chance (30))
		return v[rnd () % (sizeof (v) / sizeof (v[0]))];
	return frand (-20, 300);
}

static int intish (void)
{
	static const int	v[] = { 0, 1, 127, 128, 255, 256, 32767, 32768, 65535, 65536, -1, -128, -129, -32768, 0x7fffffff, (int) 0x80000000 };
	if (chance (35))
		return v[rnd () % (sizeof (v) / sizeof (v[0]))];
	return (int) (rnd () % 512) - 64;
}

static int pick_protocol (void)
{
	static const int	common[] = { 18, 19, 21, 100 };
	static const int	odd[] = { 15, 17, 20, 22, 50, 99, 101, 1000, 0, -1 };
	if (chance (80))
		return common[rnd () % 4];
	return odd[rnd () % (sizeof (odd) / sizeof (odd[0]))];
}

static int pick_nprecache (void)
{
	static const int	v[] = { 1, 2, 10, 255, 256, 257, 258, 511, 512, 513, 514, 767, 768, 769, 770, MAX_SOUNDS - 1, MAX_SOUNDS };
	if (chance (85))
		return v[rnd () % (sizeof (v) / sizeof (v[0]))];
	return 1 + (int) (rnd () % MAX_SOUNDS);
}

static void gen_call (call_t *c)
{
	int	k;

	memset (c, 0, sizeof (*c));
	c->fn = (int) (rnd () % F_COUNT);
	/* the sound messages get most of the cases: they hold most branches */
	if (chance (35))
		c->fn = F_SOUND;
	c->protocol = pick_protocol ();

	for (k = 0; k < 3; k++)
	{
		c->org[k] = coordish ();
		c->a[k] = (c->fn == F_P1) ? dirish () : (c->fn == F_P3 ? byteish () : (chance (5) ? odd_float () : frand (-500, 500)));
		c->b[k] = chance (5) ? odd_float () : frand (-500, 500);
		c->e_origin[k] = coordish ();
		c->e_mins[k] = chance (3) ? odd_float () : -frand (0, 64);
		c->e_maxs[k] = chance (3) ? odd_float () : frand (0, 64);
	}
	c->radius = byteish ();
	c->color = intish ();
	c->effect = intish ();
	c->count = intish ();

	/* the entity: around 8192, where the packed form stops */
	switch (rnd () % 6)
	{
	case 0: c->ent = 8191 + (int) (rnd () % 3); break;
	case 1: c->ent = (int) (rnd () % 3); break;
	case 2:
	{
		static const int	v[] = { 16383, 16384, 16385, NE - 1, NE - 2, 24576, 31999 };
		c->ent = v[rnd () % (sizeof (v) / sizeof (v[0]))];
		break;
	}
	default: c->ent = (int) (rnd () % NE); break;
	}
	if (chance (93))
		c->channel = (int) (rnd () % 8);
	else
	{
		static const int	v[] = { -1, 8, 9, 15, 100, -8, 0x7fffffff };
		c->channel = v[rnd () % (sizeof (v) / sizeof (v[0]))];
	}
	if (chance (60))
		c->volume = DEFAULT_SOUND_PACKET_VOLUME;
	else if (chance (93))
		c->volume = (int) (rnd () % 256);
	else
	{
		static const int	v[] = { -1, 256, 1000, -255 };
		c->volume = v[rnd () % 4];
	}
	switch (rnd () % 12)
	{
	case 0: case 1: case 2: c->attn = 1.0f; break;
	case 3: c->attn = chance (50) ? nextafterf (1.0f, 2.0f) : nextafterf (1.0f, 0.0f); break;
	case 4: c->attn = chance (50) ? 0.0f : 4.0f; break;
	case 5: c->attn = chance (50) ? -0.0f : 0.015625f * (float) (rnd () % 257); break;
	case 6:
		if (chance (40))
			c->attn = NAN;
		else
		{
			static const float	v[] = { 4.0000005f, -1e-7f, -1.0f, 5.0f, INFINITY, -INFINITY };
			c->attn = v[rnd () % 6];
		}
		break;
	default: c->attn = frand (0, 4); break;
	}

	c->nprecache = pick_nprecache ();
	c->dup = (chance (10) && c->nprecache > 3) ? 2 + (int) (rnd () % (unsigned) (c->nprecache - 2)) : 0;
	switch (rnd () % 10)
	{
	case 0:
	{
		/* misc/null.wav, in some case */
		static const char	*v[] = { "misc/null.wav", "MISC/NULL.WAV", "Misc/Null.wav", "misc/null.wa", "misc/null.wavx" };
		snprintf (c->sample, sizeof (c->sample), "%s", v[rnd () % 5]);
		break;
	}
	case 1:
		snprintf (c->sample, sizeof (c->sample), chance (50) ? "zz/unknown.wav" : "s0000.wav");
		break;
	case 4:
		/* past 767: both overflow bits, where the protocol allows them */
		snprintf (c->sample, sizeof (c->sample), "s%04d.wav", 768 + (int) (rnd () % (MAX_SOUNDS - 768)));
		break;
	case 2: case 3:
	{
		/* an index at a split threshold */
		static const int	v[] = { 1, 254, 255, 256, 257, 510, 511, 512, 513, 766, 767, 768, 769, MAX_SOUNDS - 2, MAX_SOUNDS - 1 };
		snprintf (c->sample, sizeof (c->sample), "s%04d.wav", v[rnd () % (sizeof (v) / sizeof (v[0]))]);
		break;
	}
	default:
		snprintf (c->sample, sizeof (c->sample), "s%04d.wav", 1 + (int) (rnd () % (unsigned) (c->nprecache > 1 ? c->nprecache : 2)));
		break;
	}
	if (c->dup && chance (50))
		/* the repeated name: the first entry wins */
		snprintf (c->sample, sizeof (c->sample), "s%04d.wav", c->dup - (chance (70) ? 1 : 0));
	if (chance (5))
	{
		/* the same name in another case: strcmp, not strcasecmp, finds it */
		char	*p;
		for (p = c->sample; *p; p++)
			if (*p == 's')
				*p = 'S';
	}

	/* the datagram: at the message's threshold, or anywhere */
	if (chance (45) && fn_room[c->fn])
		c->cursize = MAX_DATAGRAM - fn_room[c->fn] + (int) (rnd () % 5) - 2;
	else
		c->cursize = (int) (rnd () % (MAX_DATAGRAM + 8));
	if (chance (88))
	{
		c->maxsize = (int) sizeof (sv.datagram_buf);
		c->allowoverflow = 0;
	}
	else
	{
		/* too small for the message */
		c->cursize = (int) (rnd () % 64);
		c->maxsize = c->cursize + (int) (rnd () % 40);
		c->allowoverflow = chance (75);
	}
	c->overflowed = chance (5);
}

static int is_null_wav (const char *s)
{
	return q_strcasecmp (s, "misc/null.wav") == 0;
}

/* a case that may reach Host_Error or Sys_Error */
static int may_be_fatal (const call_t *c)
{
	/* a short buffer: SZ_GetSpace's Sys_Error without allowoverflow, or for
	 * a write larger than the whole buffer with it */
	if (c->maxsize != (int) sizeof (sv.datagram_buf))
		return 1;
	if (c->fn != F_SOUND || is_null_wav (c->sample))
		return 0;
	return c->volume < 0 || c->volume > 255 || c->attn < 0 || c->attn > 4
		|| c->channel < 0 || c->channel > 7;
}

static void setup (const call_t *c)
{
	int	i;
	edict_t	*e;

	sv_protocol = c->protocol;
	memcpy (sv.datagram_buf, init_buf, sizeof (sv.datagram_buf));
	sv.datagram.data = sv.datagram_buf;
	sv.datagram.maxsize = c->maxsize;
	sv.datagram.cursize = c->cursize;
	sv.datagram.allowoverflow = c->allowoverflow;
	sv.datagram.overflowed = c->overflowed;
	sv.datagram.name = "sv.datagram";

	for (i = 0; i < MAX_SOUNDS; i++)
		sv.sound_precache[i] = (i < c->nprecache) ? names[i] : NULL;
	/* the member after the array, as the engine has it set: a search that
	 * ran off a full precache would read a name here, not a NULL */
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
		sv.lightstyles[i] = "m";
	if (c->dup)
		sv.sound_precache[c->dup] = names[c->dup - 1];

	e = edict (c->ent);
	VectorCopy (c->e_origin, e->v.origin);
	VectorCopy (c->e_mins, e->v.mins);
	VectorCopy (c->e_maxs, e->v.maxs);
	loglen = 0;
}

static int call_side (int rust, const call_t *c)
{
	vec3_t	org, a, b;
	edict_t	*e = edict (c->ent);

	VectorCopy (c->org, org);
	VectorCopy (c->a, a);
	VectorCopy (c->b, b);
	switch (c->fn)
	{
	case F_P1: rust ? SV_StartParticle (org, a, c->color, c->count) : c_SV_StartParticle (org, a, c->color, c->count); return 0;
	case F_P2: rust ? SV_StartParticle2 (org, a, b, c->color, c->effect, c->count) : c_SV_StartParticle2 (org, a, b, c->color, c->effect, c->count); return 0;
	case F_P3: rust ? SV_StartParticle3 (org, a, c->color, c->effect, c->count) : c_SV_StartParticle3 (org, a, c->color, c->effect, c->count); return 0;
	case F_P4: rust ? SV_StartParticle4 (org, c->radius, c->color, c->effect, c->count) : c_SV_StartParticle4 (org, c->radius, c->color, c->effect, c->count); return 0;
	case F_STOP: rust ? SV_StopSound (e, c->channel) : c_SV_StopSound (e, c->channel); return 0;
	case F_UPDATE: rust ? SV_UpdateSoundPos (e, c->channel) : c_SV_UpdateSoundPos (e, c->channel); return 0;
	case F_SOUND: rust ? SV_StartSound (e, c->channel, c->sample, c->volume, c->attn) : c_SV_StartSound (e, c->channel, c->sample, c->volume, c->attn); return 0;
	case F_MAXSOUNDS: return rust ? SV_MaxSounds () : c_SV_MaxSounds ();
	case F_MAXDGRAM: return rust ? SV_MaxDatagram () : c_SV_MaxDatagram ();
	default: return rust ? SV_MaxReliableMessage () : c_SV_MaxReliableMessage ();
	}
}

/* FNV-1a over the datagram buffer */
static uint64_t buf_hash (void)
{
	uint64_t	h = 0xcbf29ce484222325ULL;
	size_t		i;
	for (i = 0; i < sizeof (sv.datagram_buf); i++)
		h = (h ^ sv.datagram_buf[i]) * 0x100000001b3ULL;
	return h;
}

static char	child_report[sizeof (logbuf) + 128];

static void report_and_exit (void)
{
	int	n = snprintf (child_report, sizeof (child_report), "%d|%d|%016llx|%.*s",
			sv.datagram.cursize, sv.datagram.overflowed,
			(unsigned long long) buf_hash (), (int) loglen, logbuf);
	ssize_t	w = write (in_child, child_report, (size_t) n);
	(void) w;
	exit (0);	/* exit, not _exit: the coverage run's counters are written at exit */
}

/* one side of a fatal-capable case, in a child; returns what it reported */
static int forked_side (int rust, const call_t *c, char *out, size_t outsize)
{
	int	fd[2], status;
	pid_t	pid;
	size_t	got = 0;
	ssize_t	r;

	if (pipe (fd) != 0)
	{
		perror ("pipe");
		exit (2);
	}
	fflush (NULL);
	pid = fork ();
	if (pid < 0)
	{
		perror ("fork");
		exit (2);
	}
	if (pid == 0)
	{
		close (fd[0]);
		in_child = fd[1];
		setup (c);
		call_side (rust, c);
		report_and_exit ();
	}
	close (fd[1]);
	while (got < outsize - 1 && (r = read (fd[0], out + got, outsize - 1 - got)) > 0)
		got += (size_t) r;
	out[got] = 0;
	close (fd[0]);
	waitpid (pid, &status, 0);
	if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
	{
		snprintf (out + got, outsize - got, "|DIED %d", status);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------------ */

static long	n_cases, n_forked, n_problems;
static long	o_dropped_full, o_large, o_large_dropped, o_not_precached, o_violation;
static long	o_overflow1, o_overflow2, o_overflow_both, o_vol, o_attn, o_null;
static long	o_fatal, o_sz_overflow, o_clamped, o_found_dup, o_large_high;

static void problem (long i, const call_t *c, const char *what)
{
	n_problems++;
	if (n_problems <= 10)
		printf ("MISMATCH case #%ld (%s): %s: %s\n", i, fn_name[c->fn], what,
			may_be_fatal (c) ? "forked" : "in-process");
}

/* what the C side's output says happened, for the outcome floors */
static void count_outcome (const call_t *c, int before, const char *log)
{
	if (strstr (log, "not precached"))
		o_not_precached++;
	if (strstr (log, "violation"))
		o_violation++;
	if (strstr (log, "SZ_GetSpace"))
		o_sz_overflow++;
	if (c->fn >= F_MAXSOUNDS)
		return;
	if (fn_room[c->fn] && c->cursize > MAX_DATAGRAM - fn_room[c->fn])
		o_dropped_full++;
	if ((c->fn == F_STOP || c->fn == F_UPDATE || c->fn == F_SOUND) && c->ent >= 8192)
	{
		if (sv.datagram.cursize == before && !log[0])
			o_large_dropped++;
		else
		{
			o_large++;
			if (c->ent > 16383)
				o_large_high++;
		}
	}
	if (c->fn == F_SOUND && is_null_wav (c->sample))
		o_null++;
	if (c->fn == F_SOUND && sv.datagram.cursize > before && before < c->maxsize
	    && sv.datagram_buf[before] == svc_sound)
	{
		int	mask = sv.datagram_buf[before + 1];
		if ((mask & SND_OVERFLOW) && (mask & SND_OVERFLOW2))
			o_overflow_both++;
		else if (mask & SND_OVERFLOW)
			o_overflow1++;
		else if (mask & SND_OVERFLOW2)
			o_overflow2++;
		if (mask & SND_VOLUME)
			o_vol++;
		if (mask & SND_ATTENUATION)
			o_attn++;
		if (c->dup)
			o_found_dup++;
	}
	if (c->fn == F_P1)
	{
		int	k;
		for (k = 0; k < 3; k++)
			if (isfinite (c->a[k]) && fabsf (c->a[k] * 16) >= 127)
				o_clamped++;
	}
}

static void floor_check (const char *what, long have, long want)
{
	if (have < want)
	{
		printf ("COVERAGE: %s reached %ld time(s), floor %ld\n", what, have, want);
		n_problems++;
	}
}

int main (void)
{
	const long	NCASES = 120000;
	static byte	out_c[sizeof (sv.datagram_buf)];
	static char	log_c[sizeof (logbuf)];
	static char	fork_c[sizeof (child_report) + 64], fork_r[sizeof (child_report) + 64];
	long		i;
	size_t		k;

	edict_store = calloc (NE, (size_t) pr_edict_size);
	if (!edict_store)
		return 2;
	sv.edicts = (edict_t *) edict_store;
	sv.num_edicts = NE;
	for (k = 0; k < MAX_SOUNDS; k++)
		snprintf (names[k], sizeof (names[k]), "s%04d.wav", (int) k);
	for (k = 0; k < sizeof (init_buf); k++)
		init_buf[k] = (byte) rnd ();

	for (i = 0; i < NCASES; i++)
	{
		call_t		c;
		outcome_t	oc, orr;
		int		before;

		gen_call (&c);
		n_cases++;

		if (may_be_fatal (&c))
		{
			int	okc, okr;
			n_forked++;
			okc = forked_side (0, &c, fork_c, sizeof (fork_c));
			okr = forked_side (1, &c, fork_r, sizeof (fork_r));
			if (!okc || !okr || strcmp (fork_c, fork_r) != 0)
			{
				problem (i, &c, "forked");
				if (n_problems <= 10)
					printf ("    C    \"%.200s\"\n    Rust \"%.200s\"\n", fork_c, fork_r);
			}
			if (strstr (fork_c, "Host_Error|") || strstr (fork_c, "Sys_Error|"))
				o_fatal++;
			if (strstr (fork_c, "SZ_GetSpace"))
				o_sz_overflow++;
			continue;
		}

		setup (&c);
		before = sv.datagram.cursize;
		oc.ret = call_side (0, &c);
		oc.cursize = sv.datagram.cursize;
		oc.overflowed = sv.datagram.overflowed;
		oc.loglen = loglen;
		memcpy (out_c, sv.datagram_buf, sizeof (out_c));
		memcpy (log_c, logbuf, loglen);
		log_c[loglen] = 0;
		count_outcome (&c, before, log_c);

		setup (&c);
		orr.ret = call_side (1, &c);
		orr.cursize = sv.datagram.cursize;
		orr.overflowed = sv.datagram.overflowed;
		orr.loglen = loglen;

		if (oc.ret != orr.ret)
			problem (i, &c, "return value");
		else if (oc.cursize != orr.cursize || oc.overflowed != orr.overflowed)
			problem (i, &c, "cursize/overflowed");
		else if (memcmp (out_c, sv.datagram_buf, sizeof (out_c)) != 0)
			problem (i, &c, "datagram bytes");
		else if (oc.loglen != orr.loglen || memcmp (log_c, logbuf, loglen) != 0)
		{
			problem (i, &c, "console");
			if (n_problems <= 10)
				printf ("    C    \"%.200s\"\n    Rust \"%.*s\"\n", log_c, (int) (loglen > 200 ? 200 : loglen), logbuf);
		}
	}

	printf ("outcomes: dropped (datagram full) %ld, large entity sent %ld, large dropped %ld,\n"
		"          not precached %ld, protocol violation %ld, null.wav %ld, duplicate found %ld,\n"
		"          SND_OVERFLOW %ld, SND_OVERFLOW2 %ld, both %ld, volume byte %ld, attenuation byte %ld,\n"
		"          direction clamps %ld, SZ_GetSpace overflow %ld, fatal %ld\n",
		o_dropped_full, o_large, o_large_dropped, o_not_precached, o_violation, o_null, o_found_dup,
		o_overflow1, o_overflow2, o_overflow_both, o_vol, o_attn, o_clamped, o_sz_overflow, o_fatal);

	floor_check ("a dropped message", o_dropped_full, 3000);
	floor_check ("a large-entity message", o_large, 1000);
	floor_check ("a large entity dropped", o_large_dropped, 1000);
	floor_check ("a large entity past 16383", o_large_high, 500);
	floor_check ("an unprecached sound", o_not_precached, 1000);
	floor_check ("a protocol violation", o_violation, 300);
	floor_check ("misc/null.wav", o_null, 1000);
	floor_check ("a duplicate precache entry", o_found_dup, 100);
	floor_check ("SND_OVERFLOW alone", o_overflow1, 300);
	floor_check ("SND_OVERFLOW2 alone", o_overflow2, 300);
	floor_check ("both overflow bits", o_overflow_both, 300);
	floor_check ("a volume byte", o_vol, 1000);
	floor_check ("an attenuation byte", o_attn, 1000);
	floor_check ("a direction clamp", o_clamped, 1000);
	floor_check ("an SZ_GetSpace overflow", o_sz_overflow, 300);
	floor_check ("a fatal call", o_fatal, 1000);

	if (n_problems)
	{
		printf ("sv_main_sound differential harness (hexen2): FAIL, %ld problem(s) in %ld cases\n", n_problems, n_cases);
		return 1;
	}
	printf ("sv_main_sound differential harness (hexen2): PASS (%ld cases: %ld in-process, %ld forked)\n",
		n_cases, n_cases - n_forked, n_forked);
	return 0;
}
