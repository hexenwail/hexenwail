/* alias.inc -- the alias family's own std140 uniform block, AliasFrag: what
 * salias_frag reads beyond the shared FragParams (#450, alias slice S2).
 *
 * Include it after uniforms.inc, with UNIFORMS_FRAG defined.  A vertex-stage
 * AliasVert companion arrives with the vertex stages (slices S3/S4 on #450);
 * the two stay split by stage for the reason uniforms.inc gives.
 *
 * ITS OWN BLOCK, NOT FragParams, AND ITS OWN NAMES.  sworld_frag still declares
 * u_turb, u_spans and u_lightview as loose per-program uniforms with different
 * per-frame values, and salias_inst_vert still declares a loose u_lightview of
 * its own.  The members here are spelled u_af* and only reach the old names
 * through the macros below, so none of those programs can collide with them at
 * link time, and a later world migration can take its own block without
 * fighting this one over a member name.
 *
 * GL binding point 11 (UBO_BINDING_ALIAS_FRAG), assigned from C because ES
 * 3.00 cannot put layout(binding=) on a block; 10 is held for AliasVert.
 * FRAG_UBO(1) is its SDL_GPU fragment-stage slot, after FragParams' 0.  The C
 * mirror is r_aliasfrag_t in gl_uniforms.h, and the buffer is a permanent one
 * flushed by R_FlushUniforms, not the GL_Upload ring, which is a stub on the
 * ES tier the alias program runs on.
 *
 * EVERY MEMBER IS vec4 OR mat4, for the reason uniforms.inc gives.  The
 * lightgrid members exist on both tiers so the layout never varies; only the
 * desktop arm of salias_frag reads them.
 *
 * Block members are GLOBAL where the loose uniforms they replace were
 * per-program state, so every draw path that uses an alias-family program
 * writes the members it reads before it draws.  Which path writes what is
 * listed on r_aliasfrag_t.
 */
#ifndef ALIAS_INC
#define ALIAS_INC

#ifdef UNIFORMS_FRAG
FRAG_UBO(1) uniform AliasFrag {
    highp mat4 u_aflightview;   /*   0  world -> eye, no entity transform (froxel grid) */
    highp vec4 u_afgrid;        /*  64  xy viewport origin, zw froxels per pixel */
    highp vec4 u_afgridz;       /*  80  xy zlogscale, zlogbias */
    highp vec4 u_afwarp;        /*  96  xy liquid warp (amplitude, time), z span cell */
    highp vec4 u_afmaterial;    /* 112  xyz normal-map and gloss intensity, gloss exponent */
    highp vec4 u_aflightdir;    /* 128  xyz model light direction, in v_matpos's frame */
};                              /* 144 bytes */

#define u_lightview       u_aflightview
#define u_lightgrid_xy    u_afgrid
#define u_lightgrid_z     u_afgridz.xy
#define u_turb            u_afwarp.xy
#define u_spans           u_afwarp.z
#define u_alias_material  u_afmaterial.xyz
#define u_alias_lightdir  u_aflightdir.xyz
#endif  /* UNIFORMS_FRAG */

#endif /* ALIAS_INC */
