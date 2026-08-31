/* es_regex.c -- a small self-contained regular expression engine.
 * Copyright (C) 2026  uHexen2 developers
 *
 * See entsearch.h for the supported syntax.  Matching is whole-string and
 * backtracking; both the patterns (a command line argument) and the subjects
 * (an entity property name or value) are short enough that nothing cleverer
 * is called for.  A step budget covers the pathological cases anyway.
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

#include "q_stdinc.h"
#include "compiler.h"
#include "arch_def.h"
#include "cmdlib.h"
#include "entsearch.h"

#define	RE_INF		0x7fffffff
#define	RE_MAXSTEPS	4000000L
#define	RE_MAXDEPTH	4000

enum
{
	RE_CHAR,	/* a single literal character */
	RE_ANY,		/* . */
	RE_CLASS,	/* [...] and the \d \w \s family */
	RE_ALT,		/* alternation; also used to wrap a (group) */
	RE_REPEAT,	/* * + ? {n,m} */
	RE_BOL,		/* ^ */
	RE_EOL		/* $ */
};

typedef struct
{
	int		type;
	int		next;		/* next node of this sequence, -1 at the end */

	int		ch;		/* RE_CHAR (already case-folded when ignoring case) */
	unsigned char	set[32];	/* RE_CLASS bitmap, one bit per byte value */

	int		child;		/* RE_REPEAT: head of the repeated sequence */
	int		min, max;	/* RE_REPEAT */
	qboolean	greedy;		/* RE_REPEAT */

	int		*branches;	/* RE_ALT: head of each alternative */
	int		numbranches;
} renode_t;

struct esregex_s
{
	renode_t	*nodes;
	int		numnodes;
	int		maxnodes;
	int		head;
	qboolean	ignorecase;

	/* per-match state */
	const char	*subject;
	long		steps;
	int		depth;
	qboolean	aborted;
};

typedef struct
{
	esregex_t	*re;
	const char	*start;
	const char	*p;
	char		*err;
	size_t		errlen;
	qboolean	failed;
} reparse_t;

/* locale independent, unlike <ctype.h> */
static int es_lower (int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int es_upper (int c)
{
	return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

static void re_error (reparse_t *ps, const char *fmt, ...)
{
	va_list argptr;

	if (ps->failed)		/* keep the first, most specific complaint */
		return;
	ps->failed = true;
	if (!ps->err || !ps->errlen)
		return;
	va_start (argptr, fmt);
	q_vsnprintf (ps->err, ps->errlen, fmt, argptr);
	va_end (argptr);
}

static int re_newnode (reparse_t *ps, int type)
{
	esregex_t	*re = ps->re;
	renode_t	*n;

	if (re->numnodes == re->maxnodes)
	{
		int	newmax = re->maxnodes ? re->maxnodes * 2 : 32;
		renode_t *grown = (renode_t *) realloc (re->nodes, newmax * sizeof(renode_t));
		if (!grown)
			COM_Error ("%s: out of memory", __thisfunc__);
		re->nodes = grown;
		re->maxnodes = newmax;
	}

	n = &re->nodes[re->numnodes];
	memset (n, 0, sizeof(*n));
	n->type = type;
	n->next = -1;
	n->child = -1;
	return re->numnodes++;
}

static void re_addbranch (reparse_t *ps, int altidx, int head)
{
	renode_t	*n = &ps->re->nodes[altidx];
	int		*grown;

	grown = (int *) realloc (n->branches, (n->numbranches + 1) * sizeof(int));
	if (!grown)
		COM_Error ("%s: out of memory", __thisfunc__);
	n->branches = grown;
	n->branches[n->numbranches++] = head;
}

static void re_setchar (reparse_t *ps, int nodeidx, int c)
{
	unsigned char	*set = ps->re->nodes[nodeidx].set;

	c &= 0xff;
	set[c >> 3] |= 1 << (c & 7);
	if (ps->re->ignorecase)
	{
		int	other = (c == es_lower(c)) ? es_upper(c) : es_lower(c);
		set[other >> 3] |= 1 << (other & 7);
	}
}

static void re_setrange (reparse_t *ps, int nodeidx, int lo, int hi)
{
	int	c;

	for (c = lo; c <= hi; c++)
		re_setchar (ps, nodeidx, c);
}

/* The \d \w \s family, in or out of a character class.  Returns false if the
 * letter after the backslash is not one of them. */
static qboolean re_setshorthand (reparse_t *ps, int nodeidx, int letter)
{
	unsigned char	*set = ps->re->nodes[nodeidx].set;
	int		c;
	qboolean	negate = false;
	int		lower = es_lower (letter);
	unsigned char	tmp[32];

	if (lower != 'd' && lower != 'w' && lower != 's')
		return false;
	if (letter != lower)		/* \D \W \S */
		negate = true;

	memset (tmp, 0, sizeof(tmp));
	for (c = 0; c < 256; c++)
	{
		qboolean in = false;

		switch (lower)
		{
		case 'd':
			in = (c >= '0' && c <= '9');
			break;
		case 'w':
			in = (c == '_' || (c >= '0' && c <= '9') ||
			      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
			break;
		case 's':
			in = (c == ' ' || c == '\t' || c == '\n' ||
			      c == '\r' || c == '\f' || c == '\v');
			break;
		}
		if (in != negate)
			tmp[c >> 3] |= 1 << (c & 7);
	}

	for (c = 0; c < 32; c++)
		set[c] |= tmp[c];
	return true;
}

/* Consumes the backslash escape at ps->p and returns the literal character it
 * stands for.  Only called once the \d \w \s shorthands are ruled out. */
static int re_escapechar (reparse_t *ps)
{
	int	c;

	ps->p++;			/* the backslash */
	c = (unsigned char) *ps->p;
	if (!c)
	{
		re_error (ps, "trailing backslash at the end of the pattern");
		return 0;
	}
	ps->p++;

	switch (c)
	{
	case 't':	return '\t';
	case 'n':	return '\n';
	case 'r':	return '\r';
	case 'f':	return '\f';
	case 'v':	return '\v';
	case '0':	return '\0';
	default:	return c;	/* \. \\ \* ... escape themselves */
	}
}

static int re_parse_class (reparse_t *ps)
{
	int		idx = re_newnode (ps, RE_CLASS);
	qboolean	negate = false;
	qboolean	first = true;
	int		i;

	ps->p++;			/* '[' */
	if (*ps->p == '^')
	{
		negate = true;
		ps->p++;
	}

	/* A ']' in the first position is a literal member, as in [][] */
	while (*ps->p && (*ps->p != ']' || first))
	{
		int	lo, hi;

		first = false;
		if (*ps->p == '\\')
		{
			if (re_setshorthand (ps, idx, (unsigned char) ps->p[1]))
			{
				ps->p += 2;
				continue;
			}
			lo = re_escapechar (ps);
			if (ps->failed)
				return -1;
		}
		else
		{
			lo = (unsigned char) *ps->p++;
		}

		hi = lo;
		if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']')
		{
			ps->p++;
			if (*ps->p == '\\')
			{
				hi = re_escapechar (ps);
				if (ps->failed)
					return -1;
			}
			else
			{
				hi = (unsigned char) *ps->p++;
			}
			if (hi < lo)
			{
				re_error (ps, "reversed range in [...] at offset %d",
					  (int)(ps->p - ps->start));
				return -1;
			}
		}
		re_setrange (ps, idx, lo, hi);
	}

	if (*ps->p != ']')
	{
		re_error (ps, "unterminated [ in the pattern");
		return -1;
	}
	ps->p++;

	if (negate)
	{
		unsigned char *set = ps->re->nodes[idx].set;
		for (i = 0; i < 32; i++)
			set[i] = (unsigned char) ~set[i];
		/* never let a negated class swallow the terminator */
		set[0] &= (unsigned char) ~1;
	}

	return idx;
}

static int re_parse_alt (reparse_t *ps);

static int re_parse_atom (reparse_t *ps)
{
	int	idx, inner, c;

	switch (*ps->p)
	{
	case '(':
		ps->p++;
		if (ps->p[0] == '?' && ps->p[1] == ':')
		{
			ps->p += 2;
		}
		else if (ps->p[0] == '?')
		{
			re_error (ps, "(?...) constructs are not supported");
			return -1;
		}
		inner = re_parse_alt (ps);
		if (ps->failed)
			return -1;
		if (*ps->p != ')')
		{
			re_error (ps, "unbalanced ( in the pattern");
			return -1;
		}
		ps->p++;
		/* Wrapped in a one-branch alternation so that a following
		 * quantifier applies to the group as a whole. */
		idx = re_newnode (ps, RE_ALT);
		re_addbranch (ps, idx, inner);
		return idx;

	case '[':
		return re_parse_class (ps);

	case '.':
		ps->p++;
		return re_newnode (ps, RE_ANY);

	case '^':
		ps->p++;
		return re_newnode (ps, RE_BOL);

	case '$':
		ps->p++;
		return re_newnode (ps, RE_EOL);

	case ')':
		re_error (ps, "unbalanced ) in the pattern");
		return -1;

	case '\\':
		if (re_setshorthand (ps, (idx = re_newnode (ps, RE_CLASS)),
				     (unsigned char) ps->p[1]))
		{
			ps->p += 2;
			return idx;
		}
		/* not a shorthand: rewind the wasted node and take the literal */
		ps->re->numnodes--;
		c = re_escapechar (ps);
		if (ps->failed)
			return -1;
		break;

	default:
		c = (unsigned char) *ps->p++;
		break;
	}

	idx = re_newnode (ps, RE_CHAR);
	ps->re->nodes[idx].ch = ps->re->ignorecase ? es_lower (c) : c;
	return idx;
}

/* {n} {n,} {n,m}.  Returns false without consuming anything when the brace
 * does not open a valid count, so that a stray '{' can still be a literal. */
static qboolean re_parse_braces (reparse_t *ps, int *minp, int *maxp)
{
	const char	*save = ps->p;
	int		min = 0, max;
	qboolean	sawdigit = false;

	ps->p++;			/* '{' */
	while (*ps->p >= '0' && *ps->p <= '9')
	{
		min = min * 10 + (*ps->p++ - '0');
		if (min > 100000)
			min = 100000;
		sawdigit = true;
	}
	if (!sawdigit)
	{
		ps->p = save;
		return false;
	}

	if (*ps->p == '}')
	{
		ps->p++;
		*minp = *maxp = min;
		return true;
	}
	if (*ps->p != ',')
	{
		ps->p = save;
		return false;
	}

	ps->p++;			/* ',' */
	if (*ps->p == '}')
	{
		ps->p++;
		*minp = min;
		*maxp = RE_INF;
		return true;
	}

	max = 0;
	sawdigit = false;
	while (*ps->p >= '0' && *ps->p <= '9')
	{
		max = max * 10 + (*ps->p++ - '0');
		if (max > 100000)
			max = 100000;
		sawdigit = true;
	}
	if (!sawdigit || *ps->p != '}')
	{
		ps->p = save;
		return false;
	}
	ps->p++;
	*minp = min;
	*maxp = max;
	return true;
}

static int re_parse_quant (reparse_t *ps, int atom)
{
	for (;;)
	{
		int	min, max, idx;

		switch (*ps->p)
		{
		case '*':
			min = 0; max = RE_INF; ps->p++;
			break;
		case '+':
			min = 1; max = RE_INF; ps->p++;
			break;
		case '?':
			min = 0; max = 1; ps->p++;
			break;
		case '{':
			if (!re_parse_braces (ps, &min, &max))
				return atom;
			break;
		default:
			return atom;
		}

		if (min > max)
		{
			re_error (ps, "{%d,%d}: the minimum exceeds the maximum", min, max);
			return -1;
		}

		idx = re_newnode (ps, RE_REPEAT);
		ps->re->nodes[idx].child = atom;
		ps->re->nodes[idx].min = min;
		ps->re->nodes[idx].max = max;
		ps->re->nodes[idx].greedy = true;
		if (*ps->p == '?')	/* lazy */
		{
			ps->p++;
			ps->re->nodes[idx].greedy = false;
		}
		atom = idx;
	}
}

static int re_parse_seq (reparse_t *ps)
{
	int	head = -1, tail = -1;

	while (*ps->p && *ps->p != '|' && *ps->p != ')')
	{
		int	idx = re_parse_atom (ps);

		if (ps->failed)
			return -1;
		idx = re_parse_quant (ps, idx);
		if (ps->failed)
			return -1;

		if (head < 0)
			head = idx;
		else
			ps->re->nodes[tail].next = idx;
		tail = idx;
	}

	return head;			/* -1 is the empty sequence */
}

static int re_parse_alt (reparse_t *ps)
{
	int	first, idx;

	first = re_parse_seq (ps);
	if (ps->failed)
		return -1;
	if (*ps->p != '|')
		return first;

	idx = re_newnode (ps, RE_ALT);
	re_addbranch (ps, idx, first);
	while (*ps->p == '|')
	{
		int	branch;

		ps->p++;
		branch = re_parse_seq (ps);
		if (ps->failed)
			return -1;
		re_addbranch (ps, idx, branch);
	}
	return idx;
}

esregex_t *ES_RegexCompile (const char *pattern, qboolean ignorecase,
			    char *errbuf, size_t errlen)
{
	esregex_t	*re;
	reparse_t	ps;

	if (!pattern)
		pattern = "";
	if (errbuf && errlen)
		*errbuf = '\0';

	re = (esregex_t *) SafeMalloc (sizeof(esregex_t));
	memset (re, 0, sizeof(*re));
	re->ignorecase = ignorecase;
	re->head = -1;

	memset (&ps, 0, sizeof(ps));
	ps.re = re;
	ps.start = pattern;
	ps.p = pattern;
	ps.err = errbuf;
	ps.errlen = errlen;

	re->head = re_parse_alt (&ps);
	if (!ps.failed && *ps.p)
		re_error (&ps, "unbalanced %c in the pattern", *ps.p);

	if (ps.failed)
	{
		ES_RegexFree (re);
		return NULL;
	}
	return re;
}

void ES_RegexFree (esregex_t *re)
{
	int	i;

	if (!re)
		return;
	for (i = 0; i < re->numnodes; i++)
	{
		if (re->nodes[i].branches)
			free (re->nodes[i].branches);
	}
	if (re->nodes)
		free (re->nodes);
	free (re);
}

/*
 * ==========================================================================
 * The matcher
 *
 * A continuation is "what still has to match after the node in hand".  It is
 * a linked list living on the C stack: either the head of a sequence still to
 * run, or the tail of a repetition that wants to go round again.
 * ==========================================================================
 */

#define	CONT_NODE	0
#define	CONT_REPEAT	1

typedef struct recont_s
{
	int			kind;
	int			node;
	int			count;		/* CONT_REPEAT: iterations so far */
	const char		*iter;		/* CONT_REPEAT: where this pass began */
	const struct recont_s	*k;
} recont_t;

static qboolean re_from (esregex_t *re, int idx, const char *s, const recont_t *k);
static qboolean re_repeat (esregex_t *re, int idx, int count, const char *s,
			   const char *iterstart, const recont_t *k);

static qboolean re_cont (esregex_t *re, const char *s, const recont_t *k)
{
	if (!k)
		return (*s == '\0');	/* the whole subject must be consumed */
	if (k->kind == CONT_NODE)
		return re_from (re, k->node, s, k->k);
	return re_repeat (re, k->node, k->count, s, k->iter, k->k);
}

static const recont_t *re_push (int next, const recont_t *k, recont_t *frame)
{
	if (next < 0)
		return k;
	frame->kind = CONT_NODE;
	frame->node = next;
	frame->k = k;
	return frame;
}

static qboolean re_node (esregex_t *re, int idx, const char *s, const recont_t *k)
{
	const renode_t	*n = &re->nodes[idx];
	const recont_t	*kk;
	recont_t	frame;
	unsigned char	c;
	int		i;

	switch (n->type)
	{
	case RE_CHAR:
		c = (unsigned char) *s;
		if (!c)
			return false;
		if (re->ignorecase)
			c = (unsigned char) es_lower (c);
		if (c != n->ch)
			return false;
		return re_from (re, n->next, s + 1, k);

	case RE_ANY:
		if (!*s)
			return false;
		return re_from (re, n->next, s + 1, k);

	case RE_CLASS:
		c = (unsigned char) *s;
		if (!c)
			return false;
		if (!(n->set[c >> 3] & (1 << (c & 7))))
			return false;
		return re_from (re, n->next, s + 1, k);

	case RE_BOL:
		if (s != re->subject)
			return false;
		return re_from (re, n->next, s, k);

	case RE_EOL:
		if (*s)
			return false;
		return re_from (re, n->next, s, k);

	case RE_ALT:
		kk = re_push (n->next, k, &frame);
		for (i = 0; i < n->numbranches; i++)
		{
			if (re_from (re, n->branches[i], s, kk))
				return true;
		}
		return false;

	case RE_REPEAT:
		return re_repeat (re, idx, 0, s, NULL, k);
	}

	return false;
}

static qboolean re_from (esregex_t *re, int idx, const char *s, const recont_t *k)
{
	qboolean	result;

	if (idx < 0)
		return re_cont (re, s, k);

	if (++re->steps > RE_MAXSTEPS || re->depth >= RE_MAXDEPTH)
	{
		re->aborted = true;
		return false;
	}

	re->depth++;
	result = re_node (re, idx, s, k);
	re->depth--;
	return result;
}

static qboolean re_repeat (esregex_t *re, int idx, int count, const char *s,
			   const char *iterstart, const recont_t *k)
{
	const renode_t	*n = &re->nodes[idx];
	recont_t	frame;

	if (++re->steps > RE_MAXSTEPS)
	{
		re->aborted = true;
		return false;
	}

	/* The last pass matched without consuming anything, so going round
	 * again can only do the same forever.  Call the repetition done. */
	if (iterstart && s == iterstart)
		return re_from (re, n->next, s, k);

	frame.kind = CONT_REPEAT;
	frame.node = idx;
	frame.count = count + 1;
	frame.iter = s;
	frame.k = k;

	if (!n->greedy && count >= n->min)
	{
		if (re_from (re, n->next, s, k))
			return true;
	}

	if (count < n->max)
	{
		if (re_from (re, n->child, s, &frame))
			return true;
	}

	if (n->greedy && count >= n->min)
		return re_from (re, n->next, s, k);

	return false;
}

qboolean ES_RegexMatch (esregex_t *re, const char *text)
{
	if (!re || !text)
		return false;

	re->subject = text;
	re->steps = 0;
	re->depth = 0;
	re->aborted = false;

	return re_from (re, re->head, text, NULL);
}
