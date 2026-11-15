/* world.inc -- the world family's own std140 uniform block: WorldFrag, what
 * sworld_frag and sworld_opaque_frag read beyond the shared FragParams (#450,
 * world slice W2).
 *
 * Include it after uniforms.inc, with UNIFORMS_FRAG defined.  Never in the same
 * stage as alias.inc: both spell u_turb, u_spans and the froxel-lookup names,
 * each for its own block, and the preprocessor rejects the second definition.
 *
 * WHAT LIVES WHERE.  The values the world shares with FragParams' existing
 * slots stay there, because those slots already mean the same thing: fog
 * (u_ffogv), the alpha-test pair (u_fmask.xy), lightmap overbright and bicubic
 * (u_fmask.zw), the world caustics (u_fcaustics.xy) and the r_fullbright /
 * r_lightmap debug views (u_fdebug.xy).  Everything else only the world reads,
 * and is here, for the reason alias.inc gives for AliasFrag: FragParams is the
 * block every file-backed fragment stage on every tier reads, and it has no
 * spare lanes for nine more values.  The members mirror AliasFrag's where the
 * meaning is the same -- the froxel lookup first, then the warp with its span
 * cell in .z -- so the two families' C writers read alike.
 *
 * GL binding point 12 (UBO_BINDING_WORLD_FRAG), assigned from C because ES
 * 3.00 cannot put layout(binding=) on a block.  FRAG_UBO(1) is its SDL_GPU
 * slot, after FragParams' 0; a world stage never also declares AliasFrag, so
 * the two do not compete for it.  The C mirror is r_worldfrag_t in
 * gl_uniforms.h, flushed by R_FlushUniforms with glBufferData like the others.
 *
 * EVERY MEMBER IS vec4 OR mat4, for the reason uniforms.inc gives.  The
 * froxel-lookup and lightstyle members exist on both tiers so the layout never
 * varies; only the desktop arm of each stage reads them.
 *
 * A block member is GLOBAL where the loose uniforms these replaced were
 * per-program state, so every path that draws with a world program writes the
 * whole of WorldFrag, and every FragParams member the stages read, before it
 * flushes and draws -- one R_SetWorldFrag call.  Which path passes what is
 * listed on r_worldfrag_t.
 */
#ifndef WORLD_INC
#define WORLD_INC

#ifdef UNIFORMS_FRAG
FRAG_UBO(1) uniform WorldFrag {
    highp mat4 u_wflightview;   /*   0  world -> eye, no entity transform (froxel grid) */
    highp vec4 u_wfgrid;        /*  64  xy viewport origin, zw froxels per pixel */
    highp vec4 u_wfgridz;       /*  80  xy zlogscale, zlogbias; z dynamic-light scale (0 = off) */
    highp vec4 u_wfwarp;        /*  96  xy liquid warp (amplitude, time), z span cell */
    highp vec4 u_wfmaterial;    /* 112  xyz normal-map and gloss intensity, gloss exponent */
    highp vec4 u_wfsoftemu;     /* 128  software emulation: texture dither, banding, screen dither, any live */
    highp vec4 u_wflmstyles;    /* 144  x 1 = GPU lightstyles, y the CPU shift as a factor */
};                              /* 160 bytes */

#define u_lightview     u_wflightview
#define u_lightgrid_xy  u_wfgrid
#define u_lightgrid_z   u_wfgridz.xy
#define u_dlight_scale  u_wfgridz.z
#define u_turb          u_wfwarp.xy
#define u_spans         u_wfwarp.z
#define u_material      u_wfmaterial.xyz
#define u_softemu       u_wfsoftemu
#define u_lmstyles      u_wflmstyles.xy
#endif  /* UNIFORMS_FRAG */

#endif /* WORLD_INC */
