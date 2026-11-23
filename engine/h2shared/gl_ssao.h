/* gl_ssao.h -- r_ssao, entity contact occlusion (GitHub #188).  See gl_ssao.c. */
#ifndef GL_SSAO_H
#define GL_SSAO_H

extern cvar_t	r_ssao;

void	R_SSAO_Init (void);		/* cvars, once */
void	R_SSAO_Shutdown (void);		/* GL objects; vid_restart and quit */
/* r_ssao is on and could run here: the post chain must raise its scene FBO. */
qboolean R_SSAO_WantsSceneFBO (void);
/* Inside R_RenderScene: after the world is drawn and before anything else
 * writes depth, then after the opaque entities.  Both are no-ops unless the
 * path is on and the scene FBO is up. */
void	R_SSAO_CaptureWorld (void);
void	R_SSAO_Apply (void);

#endif
