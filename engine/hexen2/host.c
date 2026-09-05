/*
 * host.c -- coordinates spawning and killing of local servers
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2001 contributors of the Anvil of Thyrion project
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
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

#include "quakedef.h"
#include "cfgfile.h"
#include "debuglog.h"
#include "bgmusic.h"

extern int VID_MenuGetVSync (void);
#include "cdaudio.h"
#include <setjmp.h>

/*
 * A server can always be started, even if the system started out as a
 * client to a remote system.
 *
 * A client can NOT be started if the system is started as a dedicated
 * server.
 *
 * Memory is cleared / released when a server or client begins, not when
 * they end.
 */

void Host_WriteConfiguration (const char *fname);

quakeparms_t	*host_parms;

qboolean	host_initialized;		// true if into command execution

static jmp_buf	host_abort;

double		host_frametime;
double		realtime;			// without any filtering or bounding
static double	oldrealtime;			// last frame run

// config.cfg flush debounce, see the end of _Host_Frame.  uhexen2-ghv0
#define CONFIG_FLUSH_DELAY	2.0		// seconds
static double	config_flush_time;
int		host_framecount;

int		host_hunklevel;

client_t	*host_client;			// current client

byte		*host_basepal;
byte		*host_colormap;
long		host_colormapsize;

cvar_t		sys_ticrate = {"sys_ticrate", "0.05", CVAR_NONE};		// dedicated server frame interval only
/* Server/physics tick rate.  NOT ARCHIVED, and that is the point of it.
 *
 * 72 reproduces r6303's movement and physics exactly: r6303 runs one
 * Host_ServerFrame per render frame at the real frametime, capped by
 * host_maxfps, whose default is also 72 (its sys_adaptive arm only substeps
 * once a frame exceeds 0.05s, which 72 fps never does).  Every movement cvar
 * we ship matches r6303's default and flags, and sv_user.c is its file
 * verbatim, so at the defaults the feel is upstream's by construction.
 *
 * It is a knob game feel depends on, though, because parts of the movement
 * code count TICKS rather than seconds -- SV_AirAccelerate gains at most
 * `addspeed` per tick, a figure that does not scale with frametime, so air
 * acceleration is directly proportional to the rate.  A value that survives
 * into config.cfg therefore changes how the game plays, silently, forever,
 * and nobody reads a config.cfg to find out why running feels wrong.  It got
 * there the easy way once already: a headless session set it, the debounced
 * config flush (uhexen2-ghv0) wrote it out, and every launch after that was a
 * 250 Hz world nobody asked for.
 *
 * So it is session-scoped, like map_checks and sv_protocol and for the same
 * reason: a mode you put the engine in for a reason you currently have, not a
 * preference. Both escape hatches in docs/MODDING_TICKRATE.md still work --
 * a mod's own .cfg and cvar_set() both set it for as long as that mod is
 * loaded -- they just cannot leak into the player's config any more.
 * uhexen2-skjv */
cvar_t		sv_physfps = {"sv_physfps", "72", CVAR_NONE};		// server/physics tick rate
cvar_t		cl_fixangle_hold = {"cl_fixangle_hold", "0.06", CVAR_ARCHIVE};	// how long a forced view angle survives render-rate input; 0 = off
static	cvar_t	sys_adaptive = {"sys_adaptive", "1", CVAR_ARCHIVE};
static	cvar_t	host_framerate = {"host_framerate", "0", CVAR_NONE};	// set for slow motion
/* Slow motion / fast forward as a MULTIPLIER, which is the intuitive knob
 * host_framerate is not: 0.25 is quarter speed at whatever the real framerate
 * happens to be, where host_framerate pins the step to an absolute value and so
 * couples speed to frame rate.  0 means OFF, not "time stops" -- matching
 * upstream, and the reason it is not simply multiplied unconditionally. */
static	cvar_t	host_timescale = {"host_timescale", "0", CVAR_NONE};

/*
================
Host_WriteConfig_f

writeconfig [name] -- flush the config now rather than waiting for the
debounce in _Host_Frame or for shutdown.  Useful when you are about to make
the engine crash on purpose.
================
*/
static void Host_WriteConfig_f (void)
{
	char	name[MAX_QPATH];

	q_strlcpy (name, (Cmd_Argc() >= 2) ? Cmd_Argv(1) : "config.cfg", sizeof(name));
	if (!strstr(name, "."))
		q_strlcat (name, ".cfg", sizeof(name));

	Host_WriteConfiguration (name);
	Con_Printf ("Wrote %s\n", name);
}

/*
================
Host_Mapname_f
================
*/
static void Host_Mapname_f (void)
{
	if (sv.active)
	{
		Con_Printf ("\"mapname\" is \"%s\"\n", sv.name);
		return;
	}

	if (cls.state == ca_connected)
	{
		Con_Printf ("\"mapname\" is \"%s\"\n", cl.mapname);
		return;
	}

	Con_Printf ("no map loaded\n");
}
cvar_t		host_maxfps = {"host_maxfps", "72", CVAR_ARCHIVE};		// cap client framerate
static	cvar_t	host_speeds = {"host_speeds", "0", CVAR_NONE};		// set for running times

static	cvar_t	serverprofile = {"serverprofile", "0", CVAR_NONE};

cvar_t	fraglimit = {"fraglimit", "0", CVAR_NOTIFY|CVAR_SERVERINFO};
cvar_t	timelimit = {"timelimit", "0", CVAR_NOTIFY|CVAR_SERVERINFO};
cvar_t	teamplay = {"teamplay", "0", CVAR_NOTIFY|CVAR_SERVERINFO};

cvar_t	samelevel = {"samelevel", "0", CVAR_NONE};
cvar_t	noexit = {"noexit", "0", CVAR_NOTIFY|CVAR_SERVERINFO};

cvar_t	developer = {"developer", "0", CVAR_ARCHIVE};
/* Mapper mode: stop hiding authoring mistakes.  Ironwail's map_checks, name
 * and default (0, CVAR_NONE) verbatim.
 *
 * The interesting half is not extra warnings, it is the three renderer
 * workarounds it switches OFF.  gl_zfix, order-independent transparency and
 * alpha sorting all exist to make a map with coplanar faces or badly ordered
 * translucent surfaces look acceptable anyway -- which is the opposite of what
 * the person authoring that map needs to see.  With this on, the mapper gets
 * the raw picture their brushwork actually produces.
 *
 * Not archived, and deliberately: it is a mode you turn on for a session, not
 * a setting.  uhexen2-a5nn.34 */
cvar_t	map_checks = {"map_checks", "0", CVAR_NONE};

/* Autosave.  Ironwail's names and defaults verbatim.  It does not save on a
 * timer -- see Host_CheckAutosave for what the interval actually scales.
 * uhexen2-a5nn.31
 *
 * sv_autoload is deliberately NOT here.  Upstream's third cvar reloads the
 * last save on death, and that half has not been written; registering it now
 * would archive a value that reads back correctly and does nothing, which is
 * the exact thing uhexen2-a5nn.38 had to go back and fix. */
cvar_t	sv_autosave = {"sv_autosave", "1", CVAR_ARCHIVE};
cvar_t	sv_autosave_interval = {"sv_autosave_interval", "30", CVAR_ARCHIVE};

cvar_t	skill = {"skill", "1", CVAR_NONE};		// 0 - 3
cvar_t	startmap = {"startmap", "", CVAR_NONE};	// mods can override in autoexec.cfg
cvar_t	coop = {"coop", "0", CVAR_NONE};		// 0 or 1
cvar_t	deathmatch = {"deathmatch", "0", CVAR_NONE};	// 0, 1, or 2
cvar_t	randomclass = {"randomclass", "0", CVAR_NONE};	// 0, 1, or 2

cvar_t	pausable = {"pausable", "1", CVAR_NONE};

cvar_t	temp1 = {"temp1", "0", CVAR_NONE};

/* Puts the level, the skill and the kill/secret counts in the window title, so
 * a second monitor or a taskbar entry says where you are without the game
 * having to be visible.  Ironwail's name, default and CVAR_ARCHIVE verbatim, so
 * a config written by either engine means the same thing in both.
 *
 * The bead that queued this called it a "stats readout on the title/demo
 * screens"; it is the OS window title.  uhexen2-a5nn.34 */
cvar_t	cl_titlestats = {"cl_titlestats", "1", CVAR_ARCHIVE};


/*
===============================================================================

SAVEGAME FILES HANDLING

===============================================================================
*/

void Host_RemoveGIPFiles (const char *path)
{
	const char	*name;
	char	tempdir[MAX_OSPATH], *p;
	size_t	len;
	fsfind_t	find;

	if (path)
		q_strlcpy(tempdir, path, MAX_OSPATH);
	else	q_strlcpy(tempdir, FS_GetUserdir(), MAX_OSPATH);

	len = strlen(tempdir);
	p = tempdir + len;
	len = sizeof(tempdir) - len;

	name = Sys_FindFirstFile (&find, tempdir, "*.gip");
	while (name)
	{
		q_snprintf (p, len, "/%s", name);
		Sys_unlink (tempdir);
		*p = '\0';
		name = Sys_FindNextFile(&find);
	}

	Sys_FindClose(&find);
}

void Host_DeleteSave (const char *savepath)
{
	char	tmppath[MAX_OSPATH];

	if (strstr(savepath, FS_GetUserdir()) != savepath)
		return;

	Host_RemoveGIPFiles (savepath);
	q_snprintf (tmppath, sizeof(tmppath), "%s/info.dat", savepath);
	Sys_unlink (tmppath);
	Sys_rmdir (savepath);
}


/* NOTE: reachable from the background save worker (SaveThread_f), so it must
 * not touch the console directly and must not Host_Error — host_abort is the
 * main thread's setjmp target. Report through Host_PrintAsync and return the
 * error; main-thread callers escalate it themselves. */
int Host_CopyFiles (const char *source, const char *pat, const char *dest)
{
	const char	*name;
	char	tempdir[MAX_OSPATH], tempdir2[MAX_OSPATH];
	int	error;
	fsfind_t	find;

	name = Sys_FindFirstFile(&find, source, pat);
	error = 0;

	while (name)
	{
		if (q_snprintf(tempdir, sizeof(tempdir),"%s/%s", source, name) >= (int)sizeof(tempdir) ||
		    q_snprintf(tempdir2, sizeof(tempdir2),"%s/%s", dest, name) >= (int)sizeof(tempdir2))
		{
			Host_PrintAsync ("%s: string buffer overflow!\n", __thisfunc__);
			error = -1;
			goto error_out;
		}

		error = FS_CopyFile (tempdir, tempdir2);
		if (error)
		{
			Host_PrintAsync ("Error copying %s to %s\n", tempdir, tempdir2);
			goto error_out;
		}

		name = Sys_FindNextFile(&find);
	}

error_out:
	Sys_FindClose(&find);

	return error;
}


//============================================================================

/*
================
Map_Checks_f -- called when map_checks changes

Says once what the mode costs.  Three cvars silently reading back a value they
are no longer being asked for is exactly the shape of a bug report about
gl_zfix being broken.
================
*/
static void Map_Checks_f (cvar_t *var)
{
	static qboolean	showed_message = false;

	if (!var->integer || showed_message)
		return;
	showed_message = true;
	Con_SafePrintf ("Note: %s overrides gl_zfix, r_oit and r_alphasort\n", var->name);
}

/*
================
SV_PhysFPS_f -- called when sv_physfps changes

Says that a non-default tick is a gameplay change, because nothing else does.

Air acceleration is proportional to the tick count -- SV_AirAccelerate gains at
most `addspeed` per tick and that figure does not scale with frametime -- so
moving this cvar moves how the game plays.  The report it produces is "running
feels wrong", which points at nothing, and the value can arrive from a mod's
cfg or a console line typed an hour ago.  One line in the console is the whole
difference between that and a question with an answer.
================
*/
static void SV_PhysFPS_f (cvar_t *var)
{
	if (var->value == 72.0f)
		return;
	Con_SafePrintf ("Note: sv_physfps %s is not the stock tick; "
			"movement and physics feel differs from 72\n", var->string);
}

/*
================
Host_EndGame

Does not return either due to Sys_Error() or longjmp()
================
*/
void Host_EndGame (const char *message, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,message);
	q_vsnprintf (string, sizeof(string), message, argptr);
	va_end (argptr);
	Con_DPrintf ("%s: %s\n", __thisfunc__, string);

	if (sv.active)
		Host_ShutdownServer (false);

	if (cls.state == ca_dedicated)
		Sys_Error ("%s: %s", __thisfunc__, string);	// dedicated servers exit

	if (cls.demonum != -1 && !cls.timedemo)
		CL_NextDemo ();
	else
		CL_Disconnect ();

	longjmp (host_abort, 1);
}

/*
================
Host_Error

This shuts down both the client and server
Does not return either due to Sys_Error() or longjmp()
================
*/
void Host_Error (const char *error, ...)
{
	va_list		argptr;
	char		string[1024];
	static	qboolean inerror = false;

	if (inerror)
		Sys_Error ("%s: recursive error!", __thisfunc__);
	inerror = true;

	SCR_EndLoadingPlaque ();		// reenable screen updates

	va_start (argptr,error);
	q_vsnprintf (string, sizeof(string), error, argptr);
	va_end (argptr);
	Con_Printf ("%s: %s\n", __thisfunc__, string);

	if (sv.active)
		Host_ShutdownServer (false);

	if (cls.state == ca_dedicated)
		Sys_Error ("%s: %s", __thisfunc__, string);	// dedicated servers exit

	CL_Disconnect ();
	cls.demonum = -1;

	inerror = false;

	longjmp (host_abort, 1);
}

/*
================
Host_FindMaxClients
================
*/
static void Host_FindMaxClients (void)
{
	int		i;

	svs.maxclients = 1;

	i = COM_CheckParm ("-dedicated");
	if (i)
	{
		cls.state = ca_dedicated;
		if (i != (com_argc - 1))
		{
			svs.maxclients = atoi (com_argv[i+1]);
			if (svs.maxclients < 2)
				svs.maxclients = 8;
		}
		else
		{
			svs.maxclients = 8;
		}
	}
	else
		cls.state = ca_disconnected;

	i = COM_CheckParm ("-listen");
	if (i)
	{
		if (cls.state == ca_dedicated)
			Sys_Error ("Only one of -dedicated or -listen can be specified");
		if (i != (com_argc - 1))
		{
			svs.maxclients = atoi (com_argv[i+1]);
			if (svs.maxclients < 2)
				svs.maxclients = 8;
		}
		else
		{
			svs.maxclients = 8;
		}
	}
	if (svs.maxclients < 1)
		svs.maxclients = 8;
	else if (svs.maxclients > MAX_CLIENTS)
		svs.maxclients = MAX_CLIENTS;

	svs.maxclientslimit = svs.maxclients;
	if (svs.maxclientslimit < 4)
		svs.maxclientslimit = 4;
	svs.clients = (client_t *) Hunk_AllocName (svs.maxclientslimit*sizeof(client_t), "clients");

	if (svs.maxclients > 1)
		Cvar_SetQuick (&deathmatch, "1");
	else
		Cvar_SetQuick (&deathmatch, "0");
}

/*
===============
Host_SaveConfig_f
===============
*/
static void Host_SaveConfig_f (void)
{
	const char	*p;

	if (cmd_source != src_command)
		return;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("saveConfig <savename> : save a config file\n");
		return;
	}

	p = Cmd_Argv(1);
	if (*p == '.' || strstr(p, ".."))
	{
		Con_Printf ("Invalid config name.\n");
		return;
	}

	Host_WriteConfiguration (p);
}

static void Host_Version_f (void)
{
	Con_Printf ("Version %4.2f\n", ENGINE_VERSION);
	Con_Printf ("Exe: " __TIME__ " " __DATE__ "\n");
}

/* cvar callback functions : */
void Host_Callback_Notify (cvar_t *var)
{
	if (sv.active)
		SV_BroadcastPrintf ("\"%s\" changed to \"%s\"\n", var->name, var->string);
}

/*
=======================
Host_InitLocal
======================
*/
static void Host_InitLocal (void)
{
	Cmd_AddCommand ("saveconfig", Host_SaveConfig_f);
	Cmd_AddCommand ("version", Host_Version_f);

	Host_InitCommands ();

	Cvar_RegisterVariable (&developer);
	Cvar_RegisterVariable (&map_checks);
	Cvar_SetCallback (&map_checks, Map_Checks_f);
	Cvar_RegisterVariable (&sv_autosave);
	Cvar_RegisterVariable (&sv_autosave_interval);
	if (COM_CheckParm("-developer"))
	{
		Cvar_Set ("developer", "1");
		Cvar_LockVar ("developer");
	}

	Cvar_RegisterVariable (&sys_nostdout);
	Cvar_RegisterVariable (&sys_throttle);

	Cvar_RegisterVariable (&sys_ticrate);
	Cvar_RegisterVariable (&sys_adaptive);
	Cvar_RegisterVariable (&sv_physfps);
	Cvar_SetCallback (&sv_physfps, SV_PhysFPS_f);
	Cvar_RegisterVariable (&cl_fixangle_hold);

	Cvar_RegisterVariable (&host_framerate);
	Cvar_RegisterVariable (&host_timescale);
	Cmd_AddCommand ("writeconfig", Host_WriteConfig_f);
	Cmd_AddCommand ("mapname", Host_Mapname_f);
	Cvar_RegisterVariable (&host_maxfps);
	Cvar_RegisterVariable (&host_speeds);

	Cvar_RegisterVariable (&serverprofile);

	Cvar_RegisterVariable (&fraglimit);
	Cvar_RegisterVariable (&timelimit);
	Cvar_RegisterVariable (&teamplay);
	Cvar_SetCallback (&fraglimit, Host_Callback_Notify);
	Cvar_SetCallback (&timelimit, Host_Callback_Notify);
	Cvar_SetCallback (&teamplay, Host_Callback_Notify);
	Cvar_RegisterVariable (&samelevel);
	Cvar_RegisterVariable (&noexit);
	Cvar_SetCallback (&noexit, Host_Callback_Notify);
	Cvar_RegisterVariable (&skill);
	Cvar_RegisterVariable (&cl_titlestats);
	Cvar_RegisterVariable (&startmap);
	Cvar_RegisterVariable (&coop);
	Cvar_RegisterVariable (&deathmatch);
	Cvar_RegisterVariable (&randomclass);

	Cvar_RegisterVariable (&pausable);

	Cvar_RegisterVariable (&temp1);

	Host_FindMaxClients ();
}

/*
===============
Host_WriteConfiguration

Writes key bindings and archived cvars to config.cfg
===============
*/
void Host_WriteConfiguration (const char *fname)
{
	FILE	*f;
	char	path[MAX_OSPATH], temppath[MAX_OSPATH];
	int	err = 0, patherr = 0;

// dedicated servers initialize the host but don't parse and set the
// config.cfg cvars
	if (host_initialized && !isDedicated && !host_parms->errstate)
	{
		FS_MakePath_BUF (FS_USERDIR, &patherr, path, sizeof(path), fname);
		FS_MakePath_VABUF (FS_USERDIR, &err, temppath, sizeof(temppath), "%s.tmp", fname);
		if (patherr || err)
		{
			Con_Printf ("Couldn't write %s: path too long.\n", fname);
			return;
		}

	// Write to a temporary and rename it into place.  Since uhexen2-ghv0
	// this file is rewritten during play rather than only at shutdown, so
	// fopen("w") on the real thing would leave a window where an unclean
	// exit finds a truncated config -- the exact loss this was meant to
	// prevent.  Sys_rename replaces the target in one step on both POSIX
	// and Windows, so config.cfg is never absent, only old or new.
		f = fopen (temppath, "w");
		if (!f)
		{
			Con_Printf ("Couldn't write %s.\n",fname);
			return;
		}

		Key_WriteBindings (f);
		Cvar_WriteVariables (f);
		/* No "+mlook" line any more.  Mouselook is the archived `freelook`
		 * cvar now, which Cvar_WriteVariables has just emitted; appending
		 * the command as well would set a state the Options row cannot
		 * clear on the next run, which is precisely the trap that kept
		 * freelook from being addable at all.  An older config that still
		 * carries the line migrates itself: the bare form now sets the
		 * cvar (cl_input.c, IN_MLookDown) and the line is not rewritten.
		 * uhexen2-a5nn.25 */

		err = ferror (f);
		if (fclose (f) != 0)
			err = 1;

		if (err || Sys_rename (temppath, path) != 0)
		{
			Con_Printf ("Couldn't write %s.\n", fname);
			Sys_unlink (temppath);
			return;
		}

		// Whatever prompted this write, the file now matches memory, so
		// the periodic flush in _Host_Frame has nothing left to do.
		// uhexen2-ghv0
		Cvar_ConfigWritten ();
	}
}


/*
=================
SV_ClientPrintf

Sends text across to be displayed 
FIXME: make this just a stuffed echo?
=================
*/
void SV_ClientPrintf (unsigned int unused, const char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,fmt);
	q_vsnprintf (string, sizeof (string), fmt, argptr);
	va_end (argptr);

	MSG_WriteByte (&host_client->message, svc_print);
	MSG_WriteString (&host_client->message, string);
}

/*
=================
SV_BroadcastPrintf

Sends text to all active clients
=================
*/
void SV_BroadcastPrintf (const char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	int			i;

	va_start (argptr,fmt);
	q_vsnprintf (string, sizeof (string), fmt, argptr);
	va_end (argptr);

	for (i = 0; i < svs.maxclients; i++)
	{
		if (svs.clients[i].active && svs.clients[i].spawned)
		{
			MSG_WriteByte (&svs.clients[i].message, svc_print);
			MSG_WriteString (&svs.clients[i].message, string);
		}
	}
}

/*
=================
Host_ClientCommands

Send text over to the client to be executed
=================
*/
void Host_ClientCommands (const char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,fmt);
	q_vsnprintf (string, sizeof (string), fmt, argptr);
	va_end (argptr);

	MSG_WriteByte (&host_client->message, svc_stufftext);
	MSG_WriteString (&host_client->message, string);
}

/*
=====================
SV_DropClient

Called when the player is getting totally kicked off the host
if (crash = true), don't bother sending signofs
=====================
*/
void SV_DropClient (qboolean crash)
{
	int		saveSelf;
	int		i;
	client_t *client;

	if (!crash)
	{
		// send any final messages (don't check for errors)
		if (NET_CanSendMessage (host_client->netconnection))
		{
			MSG_WriteByte (&host_client->message, svc_disconnect);
			NET_SendMessage (host_client->netconnection, &host_client->message);
		}

		if (host_client->edict && host_client->spawned)
		{
		// call the prog function for removing a client
		// this will set the body to a dead frame, among other things
			saveSelf = *sv_globals.self;
			*sv_globals.self = EDICT_TO_PROG(host_client->edict);
			PR_ExecuteProgram (*sv_globals.ClientDisconnect, "ClientDisconnect");
			*sv_globals.self = saveSelf;
		}

		Sys_Printf ("Client %s removed\n",host_client->name);
	}

// break the net connection
	NET_Close (host_client->netconnection);
	host_client->netconnection = NULL;

// free the client (the body stays around)
	host_client->active = false;
	host_client->name[0] = 0;
	host_client->old_frags = -999999;
	memset(&host_client->old_v,0,sizeof(host_client->old_v));
	ED_ClearEdict(host_client->edict);
	host_client->send_all_v = true;
	net_activeconnections--;

// send notification to all clients
	for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++)
	{
		if (!client->active)
			continue;
		MSG_WriteByte (&client->message, svc_updatename);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteString (&client->message, "");
		MSG_WriteByte (&client->message, svc_updatefrags);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteShort (&client->message, 0);
		MSG_WriteByte (&client->message, svc_updatecolors);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteByte (&client->message, 0);
	}
}

/*
==================
Host_ShutdownServer

This only happens at the end of a game, not between levels
==================
*/
void Host_ShutdownServer(qboolean crash)
{
	int		i;
	int		count;
	sizebuf_t	buf;
	byte		message[4];
	double	start;

	if (!sv.active)
		return;

	Host_WaitForSaveThread();

	sv.active = false;

// stop all client sounds immediately
	if (cls.state == ca_connected)
		CL_Disconnect ();

// flush any pending messages - like the score!!!
	start = Sys_DoubleTime();
	do
	{
		count = 0;
		for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
		{
			if (host_client->active && host_client->message.cursize)
			{
				if (NET_CanSendMessage (host_client->netconnection))
				{
					NET_SendMessage(host_client->netconnection, &host_client->message);
					SZ_Clear (&host_client->message);
				}
				else
				{
					NET_GetMessage(host_client->netconnection);
					count++;
				}
			}
		}
		if ((Sys_DoubleTime() - start) > 3.0)
			break;
	}
	while (count);

// make sure all the clients know we're disconnecting
	SZ_Init (&buf, message, sizeof(message));
	MSG_WriteByte(&buf, svc_disconnect);
	count = NET_SendToAll (&buf, 5.0);
	if (count)
		Con_Printf("%s: NET_SendToAll failed for %d clients\n", __thisfunc__, count);

	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
	{
		if (host_client->active)
			SV_DropClient(crash);
	}

// clear structures
//	memset (&sv, 0, sizeof(sv)); // ServerSpawn already do this by Host_ClearMemory
	memset (svs.clients, 0, svs.maxclientslimit*sizeof(client_t));
}


/*
================
Host_ClearMemory

This clears all the memory used by both the client and server, but does
not reinitialize anything.
================
*/
void Host_ClearMemory (void)
{
	Con_DPrintf ("Clearing memory\n");
	D_FlushCaches ();
	Mod_ClearAll ();
/* host_hunklevel MUST be set at this point */
	Hunk_FreeToLowMark (host_hunklevel);

	cls.signon = 0;
	memset (&sv, 0, sizeof(sv));
	memset (&cl, 0, sizeof(cl));
}


//============================================================================


/*
===================
Host_FilterTime

Returns false if the time is too short to run a frame
===================
*/
static qboolean Host_FilterTime (float time)
{
	realtime += time;

	// enforce minimum frame time — skip sub-millisecond frames
	// instead of clamping, which would inflate the physics accumulator
	if (realtime - oldrealtime < 0.001)
		return false;

	/* Skip the host_maxfps filter when VSync is active — the frame
	 * pacer in sys_sdl.c already handles rate limiting, and this
	 * secondary filter causes frame drops when vsync delivers a
	 * frame slightly early. */
	if (!cls.timedemo && host_maxfps.value > 0 &&
	    VID_MenuGetVSync() != 0 &&
	    realtime - oldrealtime < 1.0/host_maxfps.value)
		return false;		// framerate is too high

	host_frametime = realtime - oldrealtime;
	oldrealtime = realtime;

	if (host_framerate.value > 0)
		host_frametime = host_framerate.value;
	else
	{	// don't allow really long frames
		if (host_frametime > 0.1)
			host_frametime = 0.1;
	}

	/* After the clamp, so slow motion is not silently capped and fast forward
	 * is not re-clamped back down to 0.1s. */
	if (host_timescale.value > 0)
		host_frametime *= host_timescale.value;

	return true;
}


/*
===================
Host_GetConsoleCommands

Add them exactly as if they had been typed at the console
===================
*/
static void Host_GetConsoleCommands (void)
{
	const char	*cmd;

	if (!isDedicated)
		return;	// no stdin necessary in graphical mode

	while (1)
	{
		cmd = Sys_ConsoleInput ();
		if (!cmd)
			break;
		Cbuf_AddText (cmd);
	}
}


/*
==================
Host_CheckAutosave

Ironwail's autosave, which is not a timer.  It scores the moment and saves when
the score crosses 1, so it lands when you are standing still with your health
up rather than mid-fight, and sv_autosave_interval scales that score rather
than counting down to anything.  uhexen2-a5nn.31

THE ARMOUR TERM IS THE ONE REAL ADAPTATION, and it is the difference between
porting this and appearing to.  Upstream weights the score by
health + armortype * armorvalue.  Both fields exist in our entvars because they
are inherited from Quake, and Raven's gamecode never writes either: Hexen II
armour is four independent additive pieces (armor_amulet, armor_bracer,
armor_breastplate, armor_helmet), which is why Sbar draws four "+N"s.  Ported
literally the term is always zero, the score collapses to health/100, and the
"save less often while hurt and unarmoured" behaviour disappears without
anything looking wrong.  So the four are summed instead.

Upstream already special-cases Hexen II here for found_secrets and did NOT do
so for armour, so this is not something that could have been copied across.
found_secrets needs no indirection on our side -- it is a real global.

The autosave never waits on the threaded save path.  A save already in flight
means this turn is skipped, not stalled: the score will cross again in a
moment, and a server frame blocking on disk is a worse outcome than a save that
happens two seconds later.
==================
*/
static void Host_CheckAutosave (void)
{
	float	health_change, speed, armour;
	double	elapsed;
	float	score;
	edict_t	*ply;

	if (!sv.active || !sv_autosave.integer || sv_autosave_interval.value <= 0.0f)
		return;
	if (svs.maxclients != 1 || sv.paused || cl.intermission)
		return;

	ply = sv_player;
	if (!ply || ply->free || ply->v.health <= 0.0f)
		return;

	if (cls.signon == SIGNONS)
	{
		/* Track new secrets */
		if (*sv_globals.found_secrets != sv.autosave.prev_secrets)
		{
			sv.autosave.prev_secrets = *sv_globals.found_secrets;
			sv.autosave.secret_boost = 1.0f;
		}
		else
			sv.autosave.secret_boost = q_max (0.0f,
				sv.autosave.secret_boost - (float)host_frametime / 1.5f);
	}

	/* Track health changes */
	if (!sv.autosave.prev_health)
		sv.autosave.prev_health = ply->v.health;
	health_change = ply->v.health - sv.autosave.prev_health;
	if (health_change < 0.0f)
	{
		if (health_change < -3.0f || ply->v.health < 100.0f ||
		    ply->v.watertype == CONTENTS_SLIME || ply->v.watertype == CONTENTS_LAVA)
			sv.autosave.hurt_time = sv.time;
	}
	sv.autosave.prev_health = ply->v.health;

	/* Track attacking */
	if (ply->v.button0)
		sv.autosave.shoot_time = sv.time;

	/* Time spent with cheats active doesn't count */
	if (ply->v.movetype == MOVETYPE_NOCLIP || ((int)ply->v.flags & (FL_GODMODE|FL_NOTARGET)))
	{
		sv.autosave.cheat += host_frametime;
		return;
	}

	/* Don't save if the player has been hurt recently */
	if (sv.time - sv.autosave.hurt_time < 3.0)
		return;

	/* Don't save if the player has fired recently */
	if (sv.time - sv.autosave.shoot_time < 3.0)
		return;

	/* Only save when the player slows down a bit */
	speed = VectorLength (ply->v.velocity);
	if (speed > 100.0f)
		return;

	/* Copper's func_void holds the player at the bottom for a bit before
	 * inflicting damage, so being no longer in the air is not proof it is
	 * safe to save. */
	if ((int)ply->v.movetype == MOVETYPE_NONE)
		return;

	/* Don't save too often */
	elapsed = sv.time - sv.autosave.time - sv.autosave.cheat;
	if (elapsed < 3.0)
		return;

	/* A save already running: skip this turn rather than block the frame. */
	if (Host_IsSaving())
		return;

	/* Base value is the fraction of the autosave interval already passed */
	score = (float)elapsed / sv_autosave_interval.value;
	/* Scale down when health + armour is below 100 (save less often when hurt).
	 * The four Hexen II pieces in place of Quake's armortype * armorvalue. */
	armour = ply->v.armor_amulet + ply->v.armor_bracer +
		 ply->v.armor_breastplate + ply->v.armor_helmet;
	score *= q_min (100.0f, ply->v.health + armour) / 100.0f;
	/* Boost right after picking up health */
	score += q_max (0.0f, health_change) / 100.0f;
	/* Lower a bit with speed (favour standing still) */
	score -= (speed / 100.0f) * 0.25f;
	/* Boost after finding a secret */
	score += sv.autosave.secret_boost * 0.25f;
	/* Boost after teleporting */
	{
		float	tele = 1.0f - (float)(sv.time - ply->v.teleport_time) / 1.5f;
		score += q_max (0.0f, q_min (tele, 1.0f)) * 0.5f;
	}

	if (score < 1.0f)
		return;

	sv.autosave.time = sv.time;
	sv.autosave.cheat = 0;

	/* A save is a DIRECTORY here, not a file, and Sys_mkdir does not make
	 * parents -- so "autosave/<map>" needs its parent first or the save
	 * fails on a clean userdir every time. */
	{
		char	dir[MAX_OSPATH], name[MAX_QPATH];
		int	err;

		FS_MakePath_BUF (FS_USERDIR, &err, dir, sizeof(dir), "autosave");
		if (err)
			return;
		Sys_mkdir (dir, false);	/* may already exist; the save below reports real failures */

		q_snprintf (name, sizeof(name), "autosave/%s", sv.name);
		if (Host_SaveGameToName (name, true))
			Con_DPrintf ("Autosaved to %s (score %.2f)\n", name, score);
	}
}


//#define FPS_20

/*
==================
Host_ServerFrame

==================
*/
#ifdef FPS_20

static void _Host_ServerFrame (void)
{
// run the world state
	*sv_globals.frametime = host_frametime;

// read client messages
	SV_RunClients ();

// move things around and think
// always pause in single player if in console or menus
	if (!sv.paused && (svs.maxclients > 1 || Key_GetDest() == key_game))
	{
		SV_Physics ();
	}
}

static void Host_ServerFrame (void)
{
	float	save_host_frametime;
	float	temp_host_frametime;

// run the world state
	*sv_globals.frametime = host_frametime;

// set the time and clear the general datagram
	SV_ClearDatagram ();

// check for new clients
	SV_CheckForNewClients ();

	temp_host_frametime = save_host_frametime = host_frametime;
	while (temp_host_frametime > (1.0/72.0))
	{
		if (temp_host_frametime > 0.05)
			host_frametime = 0.05;
		else
			host_frametime = temp_host_frametime;
		temp_host_frametime -= host_frametime;
		_Host_ServerFrame ();
	}
	host_frametime = save_host_frametime;

// send all messages to the clients
	SV_SendClientMessages ();

	Host_CheckAutosave ();
}

#else

static void Host_ServerFrame (void)
{
// run the world state
	*sv_globals.frametime = host_frametime;

// set the time and clear the general datagram
	SV_ClearDatagram ();

// check for new clients
	SV_CheckForNewClients ();

// read client messages
	SV_RunClients ();

// move things around and think
// always pause in single player if in console or menus
	if (!sv.paused && (svs.maxclients > 1 || Key_GetDest() == key_game))
		SV_Physics ();

// send all messages to the clients
	SV_SendClientMessages ();

	Host_CheckAutosave ();
}

#endif

/*
==================
Host_Frame

Runs all active servers
==================
*/
/*
==================
Host_PlainText

Copies a game string into a buffer an OS window title can carry.  Two things
have to go: the high bit, which in this lineage's charset selects the second
(gold) glyph row rather than a different character, and anything below space,
which a title bar has no way to draw.  Everything else passes through, so a
level name reaches the taskbar as the words the map author wrote.
==================
*/
static void Host_PlainText (char *dst, size_t dstsize, const char *src)
{
	size_t	n = 0;

	if (!dstsize)
		return;
	while (*src && n + 1 < dstsize)
	{
		int c = *(const unsigned char *)src++ & 0x7f;
		dst[n++] = (c < ' ') ? ' ' : (char)c;
	}
	dst[n] = '\0';
}

/*
==================
Host_UpdateDevEdicts

The one devstats counter that belongs to the server: how many edicts are alive
rather than merely allocated.  Walked once a frame and only while the overlay
is on, because sv.num_edicts is a ceiling that max_edicts raised to 8192 and
this is a linear scan of it.

Left at its last value when no local server is running, which is the honest
answer for a client connected to a remote one: this process does not know.
==================
*/
static void Host_UpdateDevEdicts (void)
{
	int	i, active;

	if (!devstats.integer || !sv.active)
		return;

	for (i = active = 0; i < sv.num_edicts; i++)
		if (!EDICT_NUM(i)->free)
			active++;

	dev_stats.edicts = active;
}

/*
==================
Host_UpdateWindowTitle

Ironwail's cl_titlestats.  Rebuilt at most eight times a second and written
only when the text actually changes: SDL_SetWindowTitle is a round trip to the
window manager on X11, and the numbers behind it move on kill and secret
pickups, not on frames.

Hexen II carries all four of the stats upstream shows -- STAT_MONSTERS /
STAT_TOTALMONSTERS / STAT_SECRETS / STAT_TOTALSECRETS are written by the server
in host_cmd.c and bumped on the client by svc_killedmonster / svc_foundsecret --
so this is a straight port rather than an adaptation.  The secrets pair is kept
even though Hexen II leans on puzzle pieces instead: a map that sets no secrets
reads 0/0, which is the truth about that map.

`skill` is the local cvar, so on a remote server it is what this client last
asked for rather than what that server is running.  Upstream has the same hole
and it is not worth a protocol field.
==================
*/
static void Host_UpdateWindowTitle (void)
{
	static float	timeleft = 0.0f;
	static char	last[1024];
	char		title[1024];

	timeleft -= (float)host_frametime;
	if (timeleft > 0.0f)
		return;
	timeleft = 0.125f;

	if (!cl_titlestats.integer || cls.state != ca_connected ||
	    cls.signon != SIGNONS || !cl.mapname[0])
	{
		title[0] = '\0';
	}
	else
	{
		char	levelname[sizeof(cl.levelname)];

		Host_PlainText (levelname, sizeof(levelname), cl.levelname);
		if (levelname[0])
			q_snprintf (title, sizeof(title),
				    "%s (%s)  |  skill %d  |  %d/%d kills  |  %d/%d secrets  -  ",
				    levelname, cl.mapname, skill.integer,
				    cl.stats[STAT_MONSTERS], cl.stats[STAT_TOTALMONSTERS],
				    cl.stats[STAT_SECRETS], cl.stats[STAT_TOTALSECRETS]);
		else
			q_snprintf (title, sizeof(title),
				    "%s  |  skill %d  |  %d/%d kills  |  %d/%d secrets  -  ",
				    cl.mapname, skill.integer,
				    cl.stats[STAT_MONSTERS], cl.stats[STAT_TOTALMONSTERS],
				    cl.stats[STAT_SECRETS], cl.stats[STAT_TOTALSECRETS]);
	}

	if (!strcmp (title, last))
		return;
	q_strlcpy (last, title, sizeof(last));

	/* The build's own name is the tail, and the empty string means "nothing
	 * but that name" -- which is the video layer's business, not ours, since
	 * HexenWorld and Hexen II do not share it. */
	VID_SetWindowTitle (title[0] ? title : NULL);
}

static void _Host_Frame (float time)
{
	static double		time1 = 0;
	static double		time2 = 0;
	static double		time3 = 0;
	int			pass1, pass2, pass3;
	static double		phys_accum = 0;
	double			phys_interval;
	double			render_frametime;
	double			fixangle_hold;

	if (setjmp(host_abort))
		return;			// something bad happened, or the server disconnected

// keep the random time dependent
	rand ();

// decide the simulation time
	if (!Host_FilterTime (time))
		return;			// don't run too fast, or packets will flood out

// get new key events
	Sys_SendKeyEvents ();

// allow mice or other external controllers to add commands
	IN_Commands ();

// process console commands
	Cbuf_Execute ();

	NET_Poll();
	AsyncQueue_Drain();

// check for commands typed to the host
	Host_GetConsoleCommands ();

// Fixed-timestep accumulator for the server/physics tick.
//
// This is deliberately NOT sys_ticrate.  sys_ticrate is the dedicated-server
// frame interval -- sys_sdl.c only reads it under isDedicated -- and its 0.05
// default meant the listen server thought at 20 Hz.  That is a third of the
// rate the game was written for and it is visible in play: SV_RunThink runs at
// most one think per server frame, so every chain scheduled shorter than the
// interval (the common "nextthink = time" mod idiom, monster attack frames)
// got capped at 20 Hz; and SV_AirAccelerate clamps wishspd to 30 and gains at
// most addspeed per tick, a figure that does not scale with frametime, so
// strafe-jump acceleration is directly proportional to the tick count.
//
// Upstream uHexen2 ran Host_ServerFrame once per render frame at the real
// frametime, i.e. up to host_maxfps.  Raven's own FPS_20 arm above substeps
// the world "while (temp_host_frametime > 1.0/72.0)".  72 Hz is the historical
// rate; it also keeps SV_SendClientMessages at the vanilla packet rate, which
// a faster tick would not.  uhexen2-skjv
	phys_interval = 1.0 / CLAMP(10.0, sv_physfps.value, 250.0);

// Sample input every render frame for smooth view angles; movement deltas
// accumulate into cl.pendingcmd and merge at the physics tick.
//
// A view the server is forcing with .fixangle has to survive that sampling.
// svc_setangle only arrives when the server sends, and the QC that asks for
// it thinks at its own cadence, so between two forces there are render frames
// with no setangle to overwrite what the mouse just did -- and the camera a
// mod meant to pin drifts instead.  Re-apply the latched angle rather than
// suppressing the input, so pendingcmd still gets its movement deltas: it is
// the view that is pinned, not the player.  uhexen2-g8lb
	if (cls.signon == SIGNONS && Key_GetDest() == key_game)
	{
		CL_AdjustAngles ();
		IN_Move (&cl.pendingcmd);

		fixangle_hold = cl_fixangle_hold.value;
		if (fixangle_hold > 0 && fixangle_hold < 2.0 * phys_interval)
			fixangle_hold = 2.0 * phys_interval;	/* never shorter than the tick it spans */

		if (CL_FixAngleHeld (fixangle_hold))
			VectorCopy (cl.fixangle_angles, cl.viewangles);
	}

	render_frametime = host_frametime;
	phys_accum += host_frametime;

	// Host_FilterTime already clamps one frame's contribution to 0.1s, so the
	// accumulator can gain at most that per frame.  Cap just above it: cutting
	// closer would silently dilate game time on every hitch, and this still
	// bounds one render frame's catch-up (~9 ticks at 72 Hz), which drains
	// more simulated time than a clamped frame can add.
	if (phys_accum > 0.1 + phys_interval)
		phys_accum = 0.1 + phys_interval;

	while (phys_accum >= phys_interval)
	{
		host_frametime = phys_interval;

		if (sv.active)
			CL_SendCmd ();

		if (sv.active)
			Host_ServerFrame ();

		if (!sv.active)
			CL_SendCmd ();

		phys_accum -= phys_interval;
	}

	host_frametime = render_frametime;

// update particles at render rate for smooth animation
	R_UpdateParticles ();

// read from server and interpolate entities every render frame
	if (cls.state == ca_connected)
		CL_ReadFromServer ();

// update video
	if (host_speeds.integer)
		time1 = Sys_DoubleTime ();

	SCR_UpdateScreen ();

	if (host_speeds.integer)
		time2 = Sys_DoubleTime ();

// update audio
	BGM_Update();	// adds music raw samples and/or advances midi driver
	if (cls.signon == SIGNONS)
	{
		S_Update (r_origin, vpn, vright, vup);
		CL_DecayLights ();
	}
	else
		S_Update (vec3_origin, vec3_origin, vec3_origin, vec3_origin);

	CDAudio_Update();

// Flush config.cfg while we are still running.  It used to be written only by
// Host_Shutdown, the saveConfig command and a gamedir change, so an alt-F4 or
// a crash threw away every setting touched that session -- which is how most
// people leave a game.  Debounced rather than written on each change: dragging
// a menu slider fires a set per frame, and this is a whole-file rewrite.
// uhexen2-ghv0
	if (Cvar_ConfigDirty () && realtime - config_flush_time >= CONFIG_FLUSH_DELAY)
	{
		Host_WriteConfiguration ("config.cfg");
		config_flush_time = realtime;
	}

	Host_UpdateWindowTitle ();
	Host_UpdateDevEdicts ();

	if (host_speeds.integer)
	{
		pass1 = (time1 - time3)*1000;
		time3 = Sys_DoubleTime ();
		pass2 = (time2 - time1)*1000;
		pass3 = (time3 - time2)*1000;
		Con_Printf ("%3i tot %3i server %3i gfx %3i snd\n",
					pass1+pass2+pass3, pass1, pass2, pass3);
	}

	host_framecount++;
}

void Host_Frame (float time)
{
	double	time1, time2;
	static double	timetotal;
	static int	timecount;
	int		i, c, m;

	if (!serverprofile.integer)
	{
		_Host_Frame (time);
		return;
	}

	time1 = Sys_DoubleTime ();
	_Host_Frame (time);
	time2 = Sys_DoubleTime ();

	timetotal += time2 - time1;
	timecount++;

	if (timecount < 1000)
		return;

	m = timetotal*1000/timecount;
	timecount = 0;
	timetotal = 0;
	c = 0;
	for (i = 0; i < svs.maxclients; i++)
	{
		if (svs.clients[i].active)
			c++;
	}

	Con_Printf ("serverprofile: %2i clients %2i msec\n", c, m);
}

//============================================================================

/*
====================
Host_Init
====================
*/
void Host_Init (void)
{
	Sys_Printf ("Host_Init\n");

	Memory_Init (host_parms->membase, host_parms->memsize);
	Cbuf_Init ();
	Cmd_Init ();
	COM_Init ();
	SV_Init ();
	FS_Init ();
	CL_Cmd_Init ();
	Host_RemoveGIPFiles(NULL);
	CFG_OpenConfig ("config.cfg");
	Host_InitLocal ();
	PR_Init ();
	Mod_Init ();
	NET_Init ();
	AsyncQueue_Init();
	Host_InitSave();

	Con_Printf ("Exe: " __TIME__ " " __DATE__ "\n");
	Con_Printf ("%4.1f megabyte heap\n", host_parms->memsize/(1024*1024.0));

	R_InitTextures ();		// needed even for dedicated servers

	if (cls.state != ca_dedicated)	// decided in Host_InitLocal() by calling Host_FindMaxClients()
	{
		V_Init ();
		Chase_Init ();
		W_LoadWadFile ("gfx.wad");
		Key_Init ();
		Con_Init ();
		M_Init ();

		host_basepal = (byte *)FS_LoadHunkFile ("gfx/palette.lmp", NULL);
		if (!host_basepal)
			Sys_Error ("Couldn't load gfx/palette.lmp");

		host_colormap = (byte *)FS_LoadHunkFile ("gfx/colormap.lmp", NULL);
		if (!host_colormap)
			Sys_Error ("Couldn't load gfx/colormap.lmp");
		/* Nothing downstream bounds-checks this table -- the software
		 * lighting ramps, and PP_BuildColormapLUT for r_softemu 3, both
		 * index all VID_GRADES rows on faith.  A mod shipping a short
		 * colormap would over-read every one of them, so refuse it here
		 * while we still have the length.  fs_filesize describes the call
		 * immediately above and nothing else, so it must be read now.
		 * uhexen2-me41. */
		if (fs_filesize < 256 * VID_GRADES)
			Sys_Error ("gfx/colormap.lmp is %ld bytes, need at least %d",
					fs_filesize, 256 * VID_GRADES);
		host_colormapsize = fs_filesize;

		VID_Init (host_basepal);
		Draw_Init ();
		SCR_Init ();
		R_Init ();
		Sbar_Init();

		S_Init ();
		CDAudio_Init();
		MIDI_Init();
		BGM_Init();

		CL_Init();
		IN_Init();
	}

	CFG_CloseConfig();

// move commands and cvars used by progs to the front for faster access
	Cmd_MoveToFront ("bf");
	Cvar_MoveToFront ("teamplay");
	Cvar_MoveToFront ("skill");
	Cvar_MoveToFront ("registered");

#ifdef GLQUAKE
/* analogous to host_hunklevel, this will mark OpenGL texture
 * beyond which everything will need to be purged on new map */
	gl_texlevel = numgltextures;
#endif

	Hunk_AllocName (0, "-HOST_HUNKLEVEL-");
	host_hunklevel = Hunk_LowMark ();

	host_initialized = true;
	Con_Printf("\n======== Hexen II Initialized =========\n\n");

/* execute the hexen.rc file: a valid file runs default.cfg, config.cfg
 * and autoexec.cfg in this order, then processes the cmdline arguments
 * by sending "stuffcmds". */
	if (cls.state != ca_dedicated)
	{
		Cbuf_InsertText (Cmd_StartupScript ());
		if (!setjmp(host_abort))		/* in case exec fails with a longjmp(), e.g. Host_Error() */
			Cbuf_Execute ();
	}

	Cvar_UnlockAll ();				/* unlock the early-set cvars after init */

	if (cls.state == ca_dedicated)
	{
		Cmd_StuffCmds_f ();				/* process command line arguments */
		Cbuf_Execute ();
		if (!sv.active)
			Cbuf_AddText ("map demo1\n");
	}
	else
	{
	/* -map <name>: bypass menus and load directly into the given map.
	 * Useful for TrenchBroom / mapping workflow. */
		int i = COM_CheckParm ("-map");
		if (i && i + 1 < com_argc)
		{
			char cmd[256];
			q_snprintf (cmd, sizeof(cmd), "map %s\n", com_argv[i + 1]);
			Cbuf_AddText (cmd);
		}
	}
}

/*
===============
Host_Shutdown

FIXME: this is a callback from Sys_Quit and Sys_Error.  It would be better
to run quit through here before the final handoff to the sys code.
===============
*/
void Host_Shutdown(void)
{
	static qboolean isdown = false;

	if (isdown)
	{
		printf ("recursive shutdown\n");
		return;
	}
	isdown = true;

	Host_WaitForSaveThread();
	Host_ShutdownSave();

// keep Con_Printf from trying to update the screen
	scr_disabled_for_loading = true;

	Host_WriteConfiguration ("config.cfg");

	NET_Shutdown ();

	if (cls.state != ca_dedicated)
	{
		BGM_Shutdown();
		CDAudio_Shutdown ();
		MIDI_Cleanup();
		S_Shutdown();
		IN_Shutdown ();
		VID_Shutdown();
	}

// after the subsystems, not before: the audio backends own threads that post
// APCs (the Windows MIDI callback does, uhexen2-99v0), and tearing the queue
// down first left them writing into a zeroed queue whose mutex is gone.
	AsyncQueue_Destroy();

	LOG_Close ();
}

