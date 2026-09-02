/*
 * cl_demo.c -- demo recording and playback
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

#include "quakedef.h"

static void CL_FinishTimeDemo (void);

/* vars for the mission pack intro */
qboolean	intro_playing = false;
#if 0
/* these should have been used for
 * recording the mission pack intro
 */
qboolean	skip_start = false;
int		num_intro_msg = 0;
#endif

/*
==============================================================================

DEMO CODE

When a demo is playing back, all NET_SendMessages are skipped, and
NET_GetMessages are read from the demo file.

Whenever cl.time gets past the last received message, another message is
read from the demo file.
==============================================================================
*/

/*
==============
CL_StopPlayback

Called when a demo file runs out, or the user starts a game
==============
*/
void CL_StopPlayback (void)
{
	if (!cls.demoplayback)
		return;

	Con_DPrintf("CL_StopPlayback: Stopping demo playback (timedemo=%d)\n", cls.timedemo);

	if (intro_playing)
		M_ToggleMenu_f();
	else
		Key_SetDest (key_game);

	intro_playing = false;
//	num_intro_msg = 0;

	FS_fclose (&cls.demofh);
	memset (&cls.demofh, 0, sizeof(cls.demofh));
	cls.demoplayback = false;
	cls.state = ca_disconnected;
	/* Leave basedemospeed alone -- see CL_PlayDemo_f.  These two are the
	 * per-playback state, and a stale 0 demospeed would freeze the next
	 * demo's signon reads.  uhexen2-ofl9. */
	cls.demopaused = false;
	cls.demospeed = 1.0f;

	// Execute end config if one exists for this demo
	if (cls.demofilename[0])
	{
		char cfg[MAX_OSPATH];
		q_snprintf(cfg, sizeof(cfg), "%send.cfg", cls.demofilename);
		Con_DPrintf("CL_StopPlayback: Executing demo end config '%s' if it exists\n", cfg);
		Cbuf_InsertText(va("exec %s\n", cfg));
		cls.demofilename[0] = '\0';
	}

	if (cls.timedemo)
		CL_FinishTimeDemo ();
}

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length and view angles
====================
*/
static void CL_WriteDemoMessage (void)
{
	int	len;
	int	i;
	float	f;

	len = LittleLong (net_message.cursize);
	fwrite (&len, 4, 1, cls.demofile);
//	fwrite (&len, 4, 1, cls.introdemofile);
	for (i = 0; i < 3; i++)
	{
		f = LittleFloat (cl.viewangles[i]);
		fwrite (&f, 4, 1, cls.demofile);
//		fwrite (&f, 4, 1, cls.introdemofile);
	}
	fwrite (net_message.data, net_message.cursize, 1, cls.demofile);
	fflush (cls.demofile);
//	fwrite (net_message.data, net_message.cursize, 1, cls.introdemofile);
//	fflush (cls.introdemofile);
}

/*
====================
CL_UpdateDemoSpeed

Recomputes cls.demospeed for this frame from live key state.  Ported from
Ironwail's function of the same name (Quake/cl_demo.c).  uhexen2-ofl9.

QUERIED PER FRAME RATHER THAN LATCHED ON KEY TRANSITIONS, and that is the point
of the design: the fast-forward keys are held, not pressed, so latching would
have to track every way a key can stop being held -- including pressing escape
while still holding the right arrow, which delivers no key-up to the demo at
all.  Reading keydown[] each frame has no state to get out of step.

CLAMPED AT ZERO, where upstream allows negative.  A negative speed is rewind,
and rewinding needs the recorded-frame ring phase B adds (uhexen2-i9y6); until
then the left arrow holds playback still, which is the same thing Ironwail's
demo_rewind.backstop does when it has no frames left to go back to.
====================
*/
static void CL_UpdateDemoSpeed (void)
{
	int	adjust;

	/* Not while the menu or console owns input: the arrows are navigation
	 * there, and a demo left fast-forwarding under an open menu is exactly
	 * the stuck state the per-frame query exists to prevent. */
	if (Key_GetDest () != key_game)
	{
		cls.demospeed = cls.demopaused ? 0.0f : cls.basedemospeed;
		return;
	}

	adjust = Key_IsDown (K_RIGHTARROW) - Key_IsDown (K_LEFTARROW);

	if (adjust > 0)
	{
		/* Upstream's 5x, scaled by the base speed so fast-forward stays
		 * relative to the speed the viewer picked.  Overrides pause, so
		 * the right arrow also serves as "nudge forward while paused". */
		cls.demospeed = 5.0f * (cls.basedemospeed ? cls.basedemospeed : 1.0f);
	}
	else if (adjust < 0)
	{
		cls.demospeed = 0.0f;	/* rewind is uhexen2-i9y6; hold instead */
	}
	else
	{
		cls.demospeed = cls.demopaused ? 0.0f : cls.basedemospeed;
	}

	/* Either modifier quarters whatever the above chose, which turns
	 * fast-forward into a slow crawl for frame-hunting and ordinary playback
	 * into slow motion. */
	if (Key_IsDown (K_SHIFT) || Key_IsDown (K_CTRL))
		cls.demospeed *= 0.25f;
}

/*
====================
CL_AdvanceTime

Moves cl.time on for this frame.  Split out of CL_ReadFromServer so demo
playback can run the clock at cls.demospeed instead of realtime, which is the
whole mechanism behind pause, slow motion and fast-forward: nothing else in the
client knows the demo is being scrubbed, it just sees cl.time move at a
different rate and CL_GetDemoMessage feed it messages to match.  uhexen2-ofl9.
====================
*/
void CL_AdvanceTime (void)
{
	cl.oldtime = cl.time;

	if (cls.demoplayback)
	{
		CL_UpdateDemoSpeed ();
		cl.time += cls.demospeed * host_frametime;
	}
	else
	{
		cl.time += host_frametime;
	}
}

/*
====================
CL_GetDemoMessage
====================
*/
static int CL_GetDemoMessage (void)
{
	int	r, i;
	float	f;
	static int demo_msg_count = 0;

	// decide if it is time to grab the next message
	if (cls.signon == SIGNONS)	// always grab until fully connected
	{
		if (cls.timedemo)
		{
			if (host_framecount == cls.td_lastframe)
				return 0;	// already read this frame's message
			cls.td_lastframe = host_framecount;
		// if this is the second frame, grab the real td_starttime
		// so the bogus time on the first frame doesn't count
			if (host_framecount == cls.td_startframe + 1)
				cls.td_starttime = realtime;
		}
		/* Frozen: paused, or the left arrow held with no rewind to give
		 * it (uhexen2-ofl9).  Tested before the cl.time comparison rather
		 * than left to it: at the instant of pausing cl.time can still be
		 * ahead of cl.mtime[0], and letting that read one more message
		 * would advance the world a frame AFTER the viewer paused it. */
		else if (cls.demospeed == 0.0f)
			return 0;
		else if (/* cl.time > 0 && */ cl.time <= cl.mtime[0])
			return 0;	// don't need another message yet
	}

// get the next message
	/*
	if (intro_playing && num_intro_msg > 0 && num_intro_msg < 21)
		V_DarkFlash_f();	// Fade into demo
	*/
	/*
	if (skip_start && num_intro_msg > 3)
	{
		while (num_intro_msg < 1110)
		{
			fread (&net_message.cursize, 4, 1, cls.demofile);
			VectorCopy (cl.mviewangles[0], cl.mviewangles[1]);
			for (i = 0; i < 3; i++)
			{
				r = fread (&f, 4, 1, cls.demofile);
				cl.mviewangles[0][i] = LittleFloat (f);
			}

			net_message.cursize = LittleLong (net_message.cursize);
			num_intro_msg++;
			if (net_message.cursize > MAX_MSGLEN)
				Sys_Error ("Demo message > MAX_MSGLEN");
			r = fread (net_message.data, net_message.cursize, 1, cls.demofile);
			if (r != 1)
			{
				CL_StopPlayback ();
				return 0;
			}
			if (num_intro_msg == 174 ||
				num_intro_msg == 178 ||
				num_intro_msg == 428 ||
				num_intro_msg == 553 ||
				num_intro_msg == 1012)
				break;
		}
		if (num_intro_msg == 1110)
			skip_start = false;
		goto skipit;
	}
	*/
	/* Checked on the read count rather than through ferror/feof: a
	 * memory-backed handle has no stream to fault, and a short read is the
	 * same "demo ended or is truncated" answer either way. */
	if (FS_fread (&net_message.cursize, 4, 1, &cls.demofh) != 1)
	{
		Con_DPrintf("CL_GetDemoMessage: EOF reached after %d messages\n", demo_msg_count);
		CL_StopPlayback();
		return 0;
	}

	VectorCopy (cl.mviewangles[0], cl.mviewangles[1]);
	for (i = 0 ; i < 3 ; i++)
	{
		r = FS_fread (&f, 4, 1, &cls.demofh);
		cl.mviewangles[0][i] = LittleFloat (f);
	}

	net_message.cursize = LittleLong (net_message.cursize);
//	num_intro_msg++;
	if (net_message.cursize > MAX_MSGLEN)
		Sys_Error ("Demo message > MAX_MSGLEN");

	r = FS_fread (net_message.data, net_message.cursize, 1, &cls.demofh);
	if (r != 1)
	{
		CL_StopPlayback ();
		return 0;
	}

	demo_msg_count++;

	/*
  skipit:
	if (cls.demorecording)
		CL_WriteDemoMessage ();
	*/

	return 1;
}

/*
====================
CL_GetMessage

Handles recording and playback of demos, on top of NET_ code
====================
*/
int CL_GetMessage (void)
{
	int	r;

	if (cls.demoplayback)
		return CL_GetDemoMessage ();

	while (1)
	{
		r = NET_GetMessage (cls.netcon);

		if (r != 1 && r != 2)
			return r;

	// discard nop keepalive message
		if (net_message.cursize == 1 && net_message.data[0] == svc_nop)
			Con_Printf ("<-- server to client keepalive\n");
		else
			break;
	}

	if (cls.demorecording)
		CL_WriteDemoMessage ();

	return r;
}


/*
====================
CL_Stop_f

stop recording a demo
====================
*/
void CL_Stop_f (void)
{
	if (cmd_source != src_command)
		return;

	if (!cls.demorecording)
	{
		Con_Printf ("Not recording a demo.\n");
		return;
	}

	intro_playing = false;
//	num_intro_msg = 0;

// write a disconnect message to the demo file
	SZ_Clear (&net_message);
	MSG_WriteByte (&net_message, svc_disconnect);
	CL_WriteDemoMessage ();

// finish up
//	fclose (cls.introdemofile);
//	cls.introdemofile = NULL;
	fclose (cls.demofile);
	cls.demofile = NULL;
	cls.demorecording = false;
	Con_Printf ("Completed demo\n");
}

/*
====================
CL_Record_f

record <demoname> <map> [cd track]
====================
*/
void CL_Record_f (void)
{
	int		c;
	char	name[MAX_OSPATH];
	const char	*p;
	int		track;

	if (cmd_source != src_command)
		return;

	if (cls.demorecording)
		CL_Stop_f();

	c = Cmd_Argc();
	if (c != 2 && c != 3 && c != 4)
	{
		Con_Printf ("record <demoname> [<map> [cd track]]\n");
		return;
	}

	p = Cmd_Argv(1);
	if (*p == '.' || strstr(p, ".."))
	{
		Con_Printf ("Invalid demo name.\n");
		return;
	}

	if (c == 2 && cls.state == ca_connected)
	{
		Con_Printf("Can not record - already connected to server\nClient demo recording must be started before connecting\n");
		return;
	}

// write the forced cd track number, or -1
	if (c == 4)
	{
		track = atoi(Cmd_Argv(3));
		Con_Printf ("Forcing CD track to %i\n", cls.forcetrack);
	}
	else
	{
		track = -1;
	}

	FS_MakePath_BUF (FS_USERDIR, NULL, name, sizeof(name), p);

// start the map up
	if (c > 2)
	{
		Cmd_ExecuteString ( va("map %s", Cmd_Argv(2)), src_command);
		if (cls.state != ca_connected)
			return;
	}

// open the demo file
	COM_AddExtension (name, ".dem", sizeof(name));

	Con_Printf ("recording to %s.\n", name);
	cls.demofile = fopen (name, "wb");
	if (!cls.demofile)
	{
		Con_Printf ("ERROR: couldn't create %s\n", name);
		return;
	}

	cls.forcetrack = track;
	fprintf (cls.demofile, "%i\n", cls.forcetrack);

	cls.demorecording = true;
}


/*
====================
CL_PlayDemo_f

play [demoname]
====================
*/
void CL_PlayDemo_f (void)
{
	char	name[MAX_OSPATH];

	if (cmd_source != src_command)
		return;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("playdemo <demoname> : plays a demo\n");
		return;
	}

// disconnect from server
	CL_Disconnect ();

// open the demo file
	q_strlcpy (name, Cmd_Argv(1), sizeof(name));

	intro_playing = false;
	if (gameflags & GAME_PORTALS)
	{
	/* the mission pack specific intro is actually
	 * a pre-recorded demo named t9.dem, which is
	 * plain crap, because it reserves an ordinary
	 * name for a special purpose...
	 */
		if (q_strcasecmp(name, "t9") == 0)
		{
			intro_playing = true;
		//	skip_start = true;
		}
	}

	COM_AddExtension (name, ".dem", sizeof(name));

	Con_DPrintf("CL_PlayDemo_f: Opening demo '%s' (intro=%d)\n", name, intro_playing);
	Con_Printf ("Playing demo from %s.\n", name);

	/*
	if (intro_playing)
	{
		cls.demorecording = true;
		cls.introdemofile = fopen("t9.dem", "wb");
	}
	*/

	if (FS_OpenFileHandle (name, &cls.demofh, NULL) < 0)
	{
		Con_Printf ("ERROR: couldn't open %s\n", name);
		cls.demonum = -1;	// stop demo loop
		return;
	}

	Con_DPrintf("CL_PlayDemo_f: Demo file opened successfully\n");

// ZOID, fscanf is evil
// O.S.: if a space character e.g. 0x20 (' ') follows '\n',
// fscanf skips that byte too and screws up further reads.
//	fscanf (cls.demofile, "%i\n", &cls.forcetrack);
	{
		/* Hand-rolled rather than fscanf: there is no FS_fscanf, and the
		 * comment above is right that fscanf was the wrong tool anyway.
		 * The header is one optionally-signed integer then a newline. */
		int	c, digits = 0, sign = 1;

		cls.forcetrack = 0;
		c = FS_fgetc (&cls.demofh);
		if (c == '-') { sign = -1; c = FS_fgetc (&cls.demofh); }
		while (c >= '0' && c <= '9')
		{
			cls.forcetrack = cls.forcetrack * 10 + (c - '0');
			digits++;
			c = FS_fgetc (&cls.demofh);
		}
		cls.forcetrack *= sign;

		if (!digits || c != '\n')
		{
			FS_fclose (&cls.demofh);
			memset (&cls.demofh, 0, sizeof(cls.demofh));
			cls.demonum = -1;	// stop demo loop
			Con_Printf ("ERROR: demo \"%s\" is invalid\n", name);
			return;
		}
	}

	Con_DPrintf("CL_PlayDemo_f: CD track = %d\n", cls.forcetrack);

	cls.demoplayback = true;
	cls.state = ca_connected;
	/* uhexen2-ofl9.  demospeed starts live so the signon messages are read
	 * at full rate whatever the previous demo ended at, but basedemospeed is
	 * only seeded when it has never been set: Ironwail keeps the viewer's
	 * chosen speed across demos in a loop, so a demo reel watched at 2x stays
	 * at 2x when it rolls over. */
	cls.demopaused = false;
	cls.demospeed = 1.0f;
	if (!cls.basedemospeed)
		cls.basedemospeed = 1.0f;

	Con_DPrintf("CL_PlayDemo_f: Demo playback started, state=%d\n", cls.state);

	// Store demo name and execute start config if it exists
	{
		char	base[MAX_DEMONAME];
		char	cfg[MAX_OSPATH];

		q_strlcpy(base, Cmd_Argv(1), sizeof(base));
		COM_StripExtension(base, base, sizeof(base));
		q_strlcpy(cls.demofilename, base, sizeof(cls.demofilename));

		q_snprintf(cfg, sizeof(cfg), "%sstart.cfg", base);
		Con_DPrintf("CL_PlayDemo_f: Executing demo config '%s' if it exists\n", cfg);
		Cbuf_InsertText(va("exec %s\n", cfg));
	}

// get rid of the menu and/or console
	Key_SetDest (key_game);
}

/*
====================
CL_FinishTimeDemo

====================
*/
static void CL_FinishTimeDemo (void)
{
	int	frames;
	float	time;

	Con_DPrintf("CL_FinishTimeDemo: Finishing timedemo\n");

	cls.timedemo = false;

// the first frame didn't count
	frames = (host_framecount - cls.td_startframe) - 1;
	time = realtime - cls.td_starttime;
	if (!time)
		time = 1;
	Con_Printf ("%i frames %5.1f seconds %5.1f fps\n", frames, time, frames/time);
}

/*
====================
CL_TimeDemo_f

timedemo [demoname]
====================
*/
void CL_TimeDemo_f (void)
{
	if (cmd_source != src_command)
		return;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("timedemo <demoname> : gets demo speeds\n");
		return;
	}

	Con_DPrintf("CL_TimeDemo_f: Starting timedemo '%s'\n", Cmd_Argv(1));

	CL_PlayDemo_f ();
	if (!cls.demoplayback)
		return;

// cls.td_starttime will be grabbed at the second frame of the demo, so
// all the loading time doesn't get counted

	cls.timedemo = true;
	cls.td_startframe = host_framecount;
	cls.td_lastframe = -1;	// get a new message this frame

	Con_DPrintf("CL_TimeDemo_f: Timedemo initialized (startframe=%d)\n", cls.td_startframe);
}

