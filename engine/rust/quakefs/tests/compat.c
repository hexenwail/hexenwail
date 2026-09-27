/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * compat.c -- the three engine functions the harness's substrate needs, copied
 * verbatim from engine/h2shared/common.c rather than linking that whole file.
 *
 * common.c drags in the client state (`cls`), the `developer` cvar and the
 * endian globals, none of which belongs in a harness whose subject is
 * quakefs.c.  These three are self-contained, and being copies they are
 * identical for both arms, which is the property that matters: the harness
 * compares quakefs, not the tokenizer.
 */
#include "quakedef.h"
#include "q_ctype.h"	/* q_tolower, used by the copied q_strcasecmp */
#include "filenames.h"	/* IS_DIR_SEPARATOR, used by the copied COM_StripExtension */

char		com_token[1024];


const char *COM_Parse (const char *data)
{
	int		c;
	int		len;

	len = 0;
	com_token[0] = 0;

	if (!data)
		return NULL;

// skip whitespace
skipwhite:
	while ((c = *data) <= ' ')
	{
		if (c == 0)
			return NULL;	// end of file
		data++;
	}

// skip // comments
	if (c == '/' && data[1] == '/')
	{
		while (*data && *data != '\n')
			data++;
		goto skipwhite;
	}

// skip /*..*/ comments
	if (c == '/' && data[1] == '*')
	{
		data += 2;
		while (*data && !(*data == '*' && data[1] == '/'))
			data++;
		if (*data)
			data += 2;
		goto skipwhite;
	}

// handle quoted strings specially
	if (c == '\"')
	{
		data++;
		while (1)
		{
			if ((c = *data) != 0)
				++data;
			if (c == '\"' || !c)
			{
				com_token[len] = 0;
				return data;
			}
			if (len < (int)sizeof(com_token) - 1)
				com_token[len++] = c;
		}
	}

#if 0
// parse single characters
	if (c == '{' || c == '}' || c == '(' || c == ')' || c == '\'' || c == ':')
	{
		com_token[len] = c;
		len++;
		com_token[len] = 0;
		return data+1;
	}
#endif

// parse a regular word
	do
	{
		if (len < (int)sizeof(com_token) - 1)
			com_token[len++] = c;
		data++;
		c = *data;
#if 0
		if (c == '{' || c == '}' || c == '(' || c == ')' || c == '\'' || c == ':')
			break;
#endif
	} while (c > 32);

	com_token[len] = 0;
	return data;
}

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

int q_strncasecmp(const char *s1, const char *s2, size_t n)
{
	const char * p1 = s1;
	const char * p2 = s2;
	char c1, c2;

	if (p1 == p2 || n == 0)
		return 0;

	do
	{
		c1 = q_tolower (*p1++);
		c2 = q_tolower (*p2++);
		if (c1 == '\0' || c1 != c2)
			break;
	} while (--n > 0);

	return (int)(c1 - c2);
}


size_t qerr_strlcpy (const char *caller, int linenum,
		     char *dst, const char *src, size_t size)
{
	size_t	ret = q_strlcpy (dst, src, size);
	if (ret >= size)
		Sys_Error("%s: %d: string buffer overflow!", caller, linenum);
	return ret;
}

int COM_StrCompare (const void *arg1, const void *arg2)
{
	return q_strcasecmp ( *(char **) arg1, *(char **) arg2);
}

void COM_FileBase (const char *in, char *out, size_t outsize)
{
	COM_StripExtension (COM_SkipPath(in), out, outsize);
}

int COM_CheckParm (const char *parm)
{
	int		i;

	for (i = 1; i < com_argc; i++)
	{
		if (!com_argv[i])
			continue;		// NEXTSTEP sometimes clears appkit vars.
		if (!strcmp (parm,com_argv[i]))
			return i;
	}

	return 0;
}


/* Two stand-ins rather than copies: the originals lean on common.c internals
 * (get_va_buffer, VA_BUFFERLEN, IS_DIR_SEPARATOR) that would drag the rest of
 * that file in.  Both are identical for the two arms, which is what matters --
 * the harness compares quakefs, not these. */
static char va_buffer[1024];

char *va (const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(va_buffer, sizeof va_buffer, format, ap);
	va_end(ap);
	return va_buffer;
}

const char *COM_SkipPath (const char *pathname)
{
	const char *last = pathname;

	while (*pathname) {
		if (*pathname == '/' || *pathname == '\\')
			last = pathname + 1;
		pathname++;
	}
	return last;
}

void COM_StripExtension (const char *in, char *out, size_t outsize)
{
	int	length;

	if (!*in)
	{
		*out = '\0';
		return;
	}
	if (in != out)	/* copy when not in-place editing */
		q_strlcpy (out, in, outsize);
	length = (int)strlen(out) - 1;
	while (length > 0 && out[length] != '.')
	{
		--length;
		if (IS_DIR_SEPARATOR(out[length]))
			return;	/* no extension */
	}
	if (length > 0)
		out[length] = '\0';
}
