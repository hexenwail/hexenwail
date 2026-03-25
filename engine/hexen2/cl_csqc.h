/* cl_csqc.h -- Client-Side QuakeC (HUD-only, Ironwail-style)
 *
 * Copyright (C) 2025  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef __CL_CSQC_H
#define __CL_CSQC_H

extern qboolean		csqc_active;
extern cvar_t		cl_nocsqc;

/* lifecycle */
void	CSQC_RegisterCvars (void);	/* register cvar — call from CL_Init */
void	CL_LoadCSProgs (void);		/* load csprogs.dat after signon */
void	CL_ShutdownCSProgs (void);	/* shutdown + free */

/* entry points called each frame */
qboolean CSQC_DrawHud (void);		/* returns true if CSQC drew the HUD */
qboolean CSQC_DrawScores (void);	/* returns true if CSQC drew scores */

/* CSQC builtins (defined in pr_csqc.c) */
extern const builtin_t	*csqc_builtins;
extern const int	csqc_numbuiltins;
void	CSQC_InitBuiltins (void);	/* fill builtin table — call from CL_Init */

#endif /* __CL_CSQC_H */
