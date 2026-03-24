/*
 * debuglog.c -- logging console output to a file.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2008-2010  O.Sezer <sezero@users.sourceforge.net>
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
#include "debuglog.h"
#include <sys/stat.h>
#include <fcntl.h>
#ifdef PLATFORM_WINDOWS
#include <io.h>		/* write() */
#include "io_msvc.h"
#endif
#if defined(PLATFORM_OS2)
#include <io.h>		/* write() */
#endif
#if defined(PLATFORM_UNIX) ||	\
    defined(__DJGPP__) ||	\
    defined(PLATFORM_RISCOS)
#include <unistd.h>	/* write() */
#endif

#include "filenames.h"

unsigned int		con_debuglog	= LOG_NONE;

static int			log_fd = -1;
static char		logfilename[MAX_OSPATH];	/* current logfile name	*/
static char		logbuff[MAX_PRINTMSG];		/* our log text buffer	*/

static const char	separator_line[] = "=======================================\n";

void LOG_Print (const char *logdata)
{
	if (!logdata || !*logdata)
		return;
	if (log_fd == -1)
		return;

	write (log_fd, logdata, strlen(logdata));
}

void LOG_Printf (const char *fmt, ...)
{
	va_list		argptr;

	va_start (argptr, fmt);
	q_vsnprintf (logbuff, sizeof(logbuff), fmt, argptr);
	va_end (argptr);
	LOG_Print (logbuff);
}

static void LOG_PrintVersion (void)
{
/* repeating the PrintVersion() messages from main() here */
	LOG_Printf("Hexenwail %s (%s)\n", HW_VERSION, PLATFORM_STRING);
	LOG_Printf("based on Hexen II engine %4.2f / Hammer of Thyrion 1.5.10\n", ENGINE_VERSION);
}

void LOG_Init (quakeparms_t *parms)
{
	int			i, j;
	char		session[24];

	/* always log — makes crash/bug reporting easy without extra flags */
	con_debuglog |= LOG_NORMAL;

	if (COM_CheckParm("-devlog"))	/* also log Con_DPrintf and Sys_DPrintf */
		con_debuglog |= LOG_DEVEL;

	/* write to userdir/qconsole.log — predictable, writable, easy to find */
	j = q_strlcpy (logfilename, parms->userdir, sizeof(logfilename));
	if (j && !IS_DIR_SEPARATOR(logfilename[j - 1]))
		q_strlcat(logfilename, DIR_SEPARATOR_STR, sizeof(logfilename));
	q_strlcat(logfilename, "qconsole.log", sizeof(logfilename));
	Sys_DateTimeString (session);

	/*
	Sys_unlink (logfilename);
	*/
	log_fd = open (logfilename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (log_fd == -1)
	{
		con_debuglog = LOG_NONE;
		Sys_PrintTerm ("Error: Unable to create log file\n");
		return;
	}

	LOG_Printf("LOG started on: %s - LOG LEVEL: %s\n", session, (con_debuglog & LOG_DEVEL) ? "full" : "normal");

	/* build the commandline args as a string */
	q_strlcpy (logbuff, "Command line: ", sizeof(logbuff));
	for (i = 0, j = 0; i < parms->argc; i++)
	{
		if (parms->argv[i][0] && parms->argv[i][0] != ' ')
		{
			q_strlcat (logbuff, parms->argv[i], sizeof(logbuff));
			q_strlcat (logbuff, " ", sizeof(logbuff));
			j++;
		}
	}
	if (j)
	{
		logbuff[sizeof(logbuff)-2] = 0;
		q_strlcat (logbuff, "\n", sizeof(logbuff));
		LOG_Print (logbuff);
	}
	else
	{
		q_strlcat (logbuff, "(none)\n", sizeof(logbuff));
		LOG_Print (logbuff);
	}

	/* print the version information to the log */
	LOG_PrintVersion ();
	LOG_Print (separator_line);
}

void LOG_Close (void)
{
	if (log_fd == -1)
		return;
	close (log_fd);
	log_fd = -1;
}
