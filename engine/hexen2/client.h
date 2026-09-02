/* hexen2/client.h -- client main header
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __HX2_CLIENT_H
#define __HX2_CLIENT_H

#define	MAX_SCOREBOARDNAME	32
typedef struct
{
	char		name[MAX_SCOREBOARDNAME];
	float		entertime;
	int		frags;
	int		colors;			// two 4 bit fields
	byte		translations[VID_GRADES*256];
	float		playerclass;
} scoreboard_t;

typedef struct
{
	int		destcolor[3];
	int		percent;		// 0-256
} cshift_t;

#define	CSHIFT_CONTENTS		0
#define	CSHIFT_DAMAGE		1
#define	CSHIFT_BONUS		2
#define	CSHIFT_POWERUP		3
#define	CSHIFT_INTERVENTION	4
#define	NUM_CSHIFTS		5

#define	NAME_LENGTH		64

//
// client_state_t should hold all pieces of the client state
//

#define	SIGNONS			4	// signon messages to receive before connected

/* Raised from 32 (uhexen2-liqz).  Measured peak CL_AllocDlight demand on SoT
 * is 74/frame on meso, 32 on palace, 26 on keep; at 32 the pool was saturated
 * permanently on meso and every over-quota light was dropped, which is the
 * flicker in uhexen2-ck6h.  64 clears palace and keep outright and cuts meso's
 * shortfall from 42 to 10.
 *
 * 128 now covers meso's measured 74 with headroom for combat.  Raising it
 * further is safe as far as the mask goes -- add words to dlightbits[] in the
 * three msurface_t copies and bump the assert -- but the per-entity lighting
 * loops in gl_rmain.c and r_main.c walk 0..MAX_DLIGHTS for every visible
 * model, so the cost is linear in this number.  uhexen2-liqz */
#define	MAX_DLIGHTS		128

/* surf->dlightbits is a bitmap, not a scalar: MAX_DLIGHTS outgrew a single
 * word at 64 and R_MarkLights now takes the light INDEX and does the word/bit
 * itself, rather than every caller passing an ever-wider value around.  The
 * array in msurface_t is spelled with a literal 4 because the three copies of
 * that struct (render.h, model.h, gl_model.h) do not see this header; the
 * COMPILE_TIME_ASSERT in gl_rlight.c is what keeps the two in step.
 * uhexen2-liqz */
#define	DLIGHTBITS_WORDS	((MAX_DLIGHTS + 31) / 32)
#define	DLIGHTBIT_TEST(b,i)	((b)[(i) >> 5] & (1u << ((i) & 31)))
#define	DLIGHTBIT_SET(b,i)	((b)[(i) >> 5] |= (1u << ((i) & 31)))
#define	DLIGHTBITS_CLEAR(b)	memset ((b), 0, sizeof(unsigned int) * DLIGHTBITS_WORDS)
typedef struct
{
	vec3_t		origin;
	float		radius;
	float		die;		// stop lighting after this time
	float		decay;		// drop this each second
	float		minlight;	// don't add when contributing less
	int		key;
	qboolean	dark;		// subtracts light instead of adding
	float		color[4];	// LordHavoc: colored lights support
} dlight_t;

typedef struct
{
	int		length;
	char		map[MAX_STYLESTRING];
	/* Precomputed by CL_SetLightstyle for r_flatlightstyles, which holds a
	 * style at a constant level instead of animating it.  Both are the raw
	 * 'a'..'z' map characters, not the *22 light values, and both are
	 * computed once when the style arrives rather than per frame.
	 * uhexen2-a5nn.11. */
	char		average;
	char		peak;
} lightstyle_t;

#define	MAX_EFRAGS		32768

#define	MAX_MAPSTRING		2048
#define	MAX_DEMOS		8
#define	MAX_DEMONAME		16

typedef enum
{
	ca_dedicated,		// a dedicated server with no ability to start a client
	ca_disconnected,	// full screen console with no connection
	ca_connected,		// valid netcon, talking to a server
	ca_active = ca_connected	// simply an alias for hexenworld compatibility
} cactive_t;

//
// the client_static_t structure is persistant through an arbitrary number
// of server connections
//
typedef struct
{
	cactive_t	state;

// personalization data sent to server
	char		spawnparms[MAX_MAPSTRING];	// to restart a level

// demo loop control
	int		demonum;		// -1 = don't play demos
	char		demos[MAX_DEMOS][MAX_DEMONAME];	// when not playing

// demo recording info must be here, because record is started before
// entering a map (and clearing client_state_t)
	qboolean	demorecording;
	qboolean	demoplayback;
	qboolean	timedemo;
	int		forcetrack;		// -1 = use normal cd track
	FILE		*demofile;	/* recording only -- writes need a stream */
	/* Playback reads through this instead, so a demo can live in a deflated
	 * .pk3 entry, which has no FILE * to hand back.  Recording still writes a
	 * loose file, which is why the two are not one field.  uhexen2-pzha. */
	fshandle_t	demofh;
	/* Basename of the demo being played, extension stripped.  Drives the
	 * per-demo start/end configs and the scr_demobar_timeout overlay, both
	 * of which want a name and not a file handle.  Empty when idle. */
	char		demofilename[MAX_DEMONAME];
	/* realtime of the last viewer input that reached the demo during playback
	 * -- key_dest == key_game only, so the menu and console keys that most of
	 * a demo's keypresses turn into do not count (keys.c).  Only the demo bar
	 * reads it; it is what "no interaction" in scr_demobar_timeout measures
	 * from.  Lives here rather than in the renderer so keys.c can set it
	 * without the software build needing a demo bar of its own. */
	double		demoactivity;
	/* Playback speed (uhexen2-ofl9), Ironwail's cls.demospeed /
	 * .basedemospeed / .demopaused.
	 *
	 * basedemospeed is what the viewer chose with the up/down arrows: 0.25
	 * to 8, halving and doubling.  demospeed is what THIS frame actually
	 * runs at -- base while nothing is held, 5x base while fast-forwarding,
	 * a quarter of either with shift or ctrl down, and 0 while paused.  It
	 * is recomputed every frame from live key state rather than latched on
	 * key transitions, which is why holding right and then opening the menu
	 * cannot leave playback stuck at 5x (Ironwail's note on the same code).
	 *
	 * NEVER NEGATIVE HERE, though upstream's is: a negative speed means
	 * rewind, and rewinding a .dem means being able to get back to a frame
	 * you have already consumed.  That needs the recorded frame ring phase B
	 * adds; until then the left arrow clamps to 0, which freezes playback --
	 * exactly what Ironwail's demo_rewind.backstop does when it runs out of
	 * recorded frames, just permanently.  uhexen2-i9y6. */
	float		demospeed;
	float		basedemospeed;
	qboolean	demopaused;
//	FILE		*introdemofile;
	int		td_lastframe;		// to meter out one message a frame
	int		td_startframe;		// host_framecount at start
	float		td_starttime;		// realtime at second frame of timedemo

// connection information
	int		signon;			// 0 to SIGNONS
	struct qsocket_s	*netcon;
	sizebuf_t	message;		// writing buffer to send to server

} client_static_t;

extern client_static_t	cls;

//
// the client_state_t structure is wiped completely at every
// server signon
//
typedef struct
{
	int		movemessages;		// since connecting to this server
						// throw out the first couple, so the player
						// doesn't accidentally do something the 
						// first frame

	usercmd_t	cmd;			// last command sent to the server

// information for local display
	int		stats[MAX_CL_STATS];	// health, etc
	int		inv_count, inv_startpos, inv_selected;
	int		inv_order[MAX_INVENTORY];	// standard inventory order (protocol 19)
	int		items;				// inventory bit flags
	float	faceanimtime;		// use anim frame if cl.time < this

	entvars_t	v;		// NOTE: not every field will be update
					// you must specifically add them in
					// functions SV_WriteClientdatatToMessage()
					// and CL_ParseClientdata()

	cshift_t	cshifts[NUM_CSHIFTS];	// color shifts for damage, powerups
	cshift_t	prev_cshifts[NUM_CSHIFTS];	// and content types

	char		puzzle_pieces[8][10];	// puzzle piece names

// the client maintains its own idea of view angles, which are
// sent to the server each frame.  The server sets punchangle when
// the view is temporarliy offset, and an angle reset commands at the start
// of each level and after teleporting.

	vec3_t		mviewangles[2];		// during demo playback viewangles is lerped
						// between these
	vec3_t		viewangles;

	/* Last view angle the server forced with svc_setangle, and when it
	 * arrived.  QC that sets .fixangle every frame means to pin the view;
	 * the client has to hold that between messages now that input is
	 * sampled at render rate.  uhexen2-g8lb */
	vec3_t		fixangle_angles;
	double		fixangle_time;

	vec3_t		mvelocity[2];		// update by server, used for lean+bob
						// (0 is newest)
	vec3_t		velocity;		// lerped between mvelocity[0] and [1]

	vec3_t		punchangle;		// temporary offset
	double		punchtime;		// cl.time of the last punchangle change, for v_gunkick 2

	float		idealroll;
	float		rollvel;

// pitch drifting vars
	float		idealpitch;
	float		pitchvel;
	qboolean	nodrift;
	float		driftmove;
	double		laststop;

	float		viewheight;
	float		crouch;			// local amount for smoothing stepups

	qboolean	paused;			// send over by server
	qboolean	onground;
	qboolean	inwater;

// intermissions: setup by CL_SetupIntermission() and run by SB_IntermissionOverlay()
	int		intermission;		// don't change view angle, full screen, etc
	int		completed_time;		// latched at intermission start
	int		message_index;
	int		intermission_flags;
	const char	*intermission_pic;
	int		lasting_time;
	int		intermission_next;

	double		mtime[2];		// the timestamp of last two messages
	double		time;			// clients view of time, should be between
						// servertime and oldservertime to generate
						// a lerp point for other data

	double		oldtime;		// previous cl.time, time-oldtime is used
						// to decay light values and smooth step ups

	double		lerpfrac;		// fraction into current physics tick
						// for render-time interpolation

	usercmd_t	pendingcmd;		// accumulated movement between physics ticks

	float		zoom;		/* 0=normal FOV, 1=fully zoomed to zoom_fov */
	float		zoomdir;	/* +1=zooming in, -1=zooming out, 0=stopped */

	float		last_received_message;	// (realtime) for net trouble icon

//
// information that is static for the entire time connected to a server
//
	struct qmodel_s	*model_precache[MAX_MODELS];
	struct sfx_s	*sound_precache[MAX_SOUNDS];
	struct ex_inventory_page_s ex_inventory[MAX_INVENTORY_EX_PAGES]; //ex_inventory_page_t *ex_inventory; 	// [cl.maxclients * (MAX_ITEMS_EX / 32)]
	ex_item_t	*ex_items; // [MAX_ITEMS_EX or current count?]
	int num_ex_items;
	int next_page_id;

	char		mapname[40];
	char		levelname[40];		// for display on solo scoreboard
	int		viewentity;		// cl_entitites[cl.viewentity] = player
	int		maxclients;
	int		gametype;

// refresh related state
	struct qmodel_s	*worldmodel;		// cl_entitites[0].model
	struct efrag_s	*free_efrags;
	int		num_entities;		// held in cl_entities array
	int		num_statics;		// held in cl_staticentities array
	entity_t	viewent;		// the gun model
	struct EffectT	Effects[MAX_EFFECTS];

	int		cdtrack, looptrack;	// cd audio
	char		midi_name[128];		// midi file name
	char		mod_name[128];		// server's gamedir, from svc_mod_name
	byte		current_frame, last_frame, reference_frame;
	byte		current_sequence, last_sequence;
	byte		need_build;

// frag scoreboard
	scoreboard_t	*scores;	// [cl.maxclients]

// light level at player's position including dlights
// this is sent back to the server each frame
// architectually ugly but it works
	int		light_level;

	client_frames2_t frames[3];	// 0 = base, 1 = building, 2 = 0 & 1 merged
	short		RemoveList[MAX_CLIENT_STATES], NumToRemove;

// mission pack, objectives strings
	unsigned int	info_mask, info_mask2;
} client_state_t;


//
// cvars
//
extern	cvar_t	cl_name;
extern	cvar_t	cl_color;
extern	cvar_t	cl_playerclass;

extern	cvar_t	cl_upspeed;
extern	cvar_t	cl_forwardspeed;
extern	cvar_t	cl_backspeed;
extern	cvar_t	cl_sidespeed;

extern	cvar_t	cl_movespeedkey;
extern	cvar_t	cl_alwaysrun;
extern	cvar_t	cl_maxpitch;
extern	cvar_t	cl_minpitch;

extern	cvar_t	cl_yawspeed;
extern	cvar_t	cl_pitchspeed;

extern	cvar_t	cl_anglespeedkey;

extern	cvar_t	cl_shownet;
extern	cvar_t	cl_nolerp;
extern	cvar_t	r_lerpmove;

extern	cvar_t	cl_showunbound;

extern	cvar_t	cfg_unbindall;

extern	cvar_t	cl_pitchdriftspeed;
extern	cvar_t	lookspring;
extern	cvar_t	lookstrafe;
extern	cvar_t	sensitivity;

extern	cvar_t	m_pitch;
extern	cvar_t	m_yaw;
extern	cvar_t	m_forward;
extern	cvar_t	m_side;
extern	cvar_t	m_filter;


#define	MAX_STATIC_ENTITIES	2048		// torches, etc

extern	client_state_t	cl;

// FIXME, allocate dynamically
extern	efrag_t		cl_efrags[MAX_EFRAGS];
extern	entity_t	cl_entities[MAX_EDICTS];
extern	entity_t	cl_static_entities[MAX_STATIC_ENTITIES];
extern	lightstyle_t	cl_lightstyle[MAX_LIGHTSTYLES];
extern	dlight_t	cl_dlights[MAX_DLIGHTS];

//=============================================================================

//
// cl_main
//
dlight_t *CL_AllocDlight (int key);
void	CL_DecayLights (void);

void CL_Init (void);

void CL_ClearState (void);
void CL_SetLightstyleLevels (lightstyle_t *ls);	/* r_flatlightstyles, uhexen2-a5nn.11 */

void CL_EstablishConnection (const char *host);
void CL_SignonReply (void);

int  CL_ReadFromServer (void);

void CL_Disconnect (void);
void CL_Disconnect_f (void);
void CL_NextDemo (void);

/* Raised from the legacy 256 to match Ironwail.  At 256, dense maps
 * (SoT, large coliseum levels) silently drop entities once the per-frame
 * PVS count exceeds the cap — manifests as models/brush ents flickering
 * in and out as the player moves, since which 256 win depends on
 * insertion order.  Multi-reporter regression: uhexen2-l0ac. */
#define	MAX_VISEDICTS		16384
extern	int		cl_numvisedicts;
extern	entity_t	*cl_visedicts[MAX_VISEDICTS];

//
// cl_cmd
//
void Cmd_ForwardToServer (void);
void CL_Cmd_Init (void);

//
// cl_input
//
typedef struct
{
	int		down[2];	// key nums holding it down
	int		state;		// low bit is down state
} kbutton_t;

extern	kbutton_t	in_mlook, in_klook;
extern	cvar_t		freelook;
qboolean CL_MouseLookActive (void);
// "is the mouse looking right now" -- the +mlook button held, or freelook set.
// One predicate so the input, lookspring and menu paths cannot drift apart.
// uhexen2-a5nn.25
extern	kbutton_t	in_strafe;
extern	kbutton_t	in_speed;

extern	int		in_impulse;
extern	qboolean	info_up;

void CL_InitInput (void);
void CL_SendCmd (void);
void CL_AdjustAngles (void);
void CL_BaseMove (usercmd_t *cmd);
void CL_SendMove (const usercmd_t *cmd);

//
// cl_demo.c
//
void CL_StopPlayback (void);
void CL_AdvanceTime (void);	/* moves cl.time on, at cls.demospeed during playback (uhexen2-ofl9) */
int CL_GetMessage (void);

void CL_Stop_f (void);
void CL_Record_f (void);
void CL_PlayDemo_f (void);
void CL_TimeDemo_f (void);

extern	qboolean	intro_playing;	/* whether the mission pack intro is playing */
extern	qboolean	skip_start;	/* for the mission pack intro */
extern	int		num_intro_msg;	/* for the mission pack intro */
					/* skip_start and num_intro_msg are not used at present - O.S */

//
// cl_string.c
//
extern	int	puzzle_string_count;

void CL_LoadPuzzleStrings (void);
const char *CL_FindPuzzleString (const char *shortname);

/* mission pack objectives strings */
extern	int	info_string_count;

void CL_LoadInfoStrings (void);
const char *CL_GetInfoString (int idx);

//
// cl_interlude.c
//
#define	INTERMISSION_NOT_CONNECTED	(1<<0)	/* can not use cl.time, use realtime */
#define	INTERMISSION_NO_MENUS		(1<<1)	/* don't allow drawing the menus */
#define	INTERMISSION_NO_MESSAGE		(1<<2)	/* doesn't need a valid message index */
#define	INTERMISSION_PRINT_TOP		(1<<3)	/* print centered in top half of screen */
#define	INTERMISSION_PRINT_TOPMOST	(1<<4)	/* print at top-most side of the screen */
		/* without either of the above two, prints centered on the whole screen */
#define	INTERMISSION_PRINT_WHITE	(1<<5)	/* print in white, not in red */
#define	INTERMISSION_PRINT_DELAY	(1<<6)	/* delay message print for ca. 2.5s */

void CL_SetupIntermission (int n);

//
// cl_parse.c
//
void CL_ParseServerMessage (void);

extern	int		cl_protocol;	/* protocol version used by the server */

//
// view
//
void V_StartPitchDrift (void);
void V_StopPitchDrift (void);

void V_RenderView (void);
void V_UpdatePalette (void);
void V_Register (void);
void V_ParseDamage (void);
void V_SetContentsColor (int contents);

//
// cl_effect
//
void CL_InitEffects (void);
void CL_ClearEffects (void);
void CL_EndEffect (void);
void CL_ParseEffect (void);
void CL_UpdateEffects (void);
extern	cvar_t	v_gunkick;
extern	vec3_t	v_punchangles[2];
void CL_LatchFixAngle (void);
qboolean CL_FixAngleHeld (double hold);

//
// cl_tent
//
void CL_InitTEnts (void);
void CL_PrecacheTEntSounds (void);
void CL_ClearTEnts (void);
void CL_ParseTEnt (void);
void CL_UpdateTEnts (void);

//
// chase
//
extern	cvar_t	chase_active;

void Chase_Init (void);
void Chase_Reset (void);
void Chase_Update (void);

#endif	/* __HX2_CLIENT_H */

