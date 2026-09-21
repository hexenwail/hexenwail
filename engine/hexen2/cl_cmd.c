/*
 * cl_cmd.c -- client command forwarding to server
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
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
#include "cl_hw.h"

#if defined(H2W_INTEGRATED)
/* A HexenWorld session carries commands on its own netchan; cls.message is
 * never sent while it owns the connection.  Returns true when the command
 * was handled here (sent, or refused because the handshake is not done). */
static qboolean CL_ForwardToHexenWorld (const char *cmd, const char *args)
{
	char	line[1024];

	if (!HWCL_Active ())
		return false;
	if (cmd && args && *args)
		q_snprintf (line, sizeof(line), "%s %s", cmd, args);
	else
		q_strlcpy (line, cmd ? cmd : args, sizeof(line));
	if (!HWCL_ForwardCommand (line))
		Con_Printf ("Can't \"%s\", not connected\n", Cmd_Argv(0));
	return true;
}
#endif

/*
===================
Cmd_ForwardToServer

Sends the entire command line over to the server
===================
*/
void Cmd_ForwardToServer (void)
{
#if defined(H2W_INTEGRATED)
	if (CL_ForwardToHexenWorld (Cmd_Argv(0), Cmd_Argc() > 1 ? Cmd_Args() : NULL))
		return;
#endif
	if (cls.state != ca_connected)
	{
		Con_Printf ("Can't \"%s\", not connected\n", Cmd_Argv(0));
		return;
	}

	if (cls.demoplayback)
		return;		// not really connected

	MSG_WriteByte (&cls.message, clc_stringcmd);
	SZ_Print (&cls.message, Cmd_Argv(0));

	if (Cmd_Argc() > 1)
	{
		SZ_Print (&cls.message, " ");
		SZ_Print (&cls.message, Cmd_Args());
	}
}

// This is the command variant of the above. The only difference
// is that it doesn't forward the first argument, which is "cmd"
void Cmd_ForwardToServer_f (void)
{
#if defined(H2W_INTEGRATED)
	/* "cmd" alone sends nothing, as on the Hexen II path below. */
	if (HWCL_Active () && Cmd_Argc() < 2)
		return;
	if (CL_ForwardToHexenWorld (NULL, Cmd_Args()))
		return;
#endif
	if (cls.state != ca_connected)
	{
		Con_Printf ("Can't \"%s\", not connected\n", Cmd_Argv(0));
		return;
	}

	if (cls.demoplayback)
		return;		// not really connected

	if (Cmd_Argc() > 1)
	{
		MSG_WriteByte (&cls.message, clc_stringcmd);
		SZ_Print (&cls.message, Cmd_Args());
	}
}

void CL_Cmd_Init (void)
{
	Cmd_AddCommand ("cmd", Cmd_ForwardToServer_f);
}

