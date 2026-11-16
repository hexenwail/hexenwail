/* alias.inc -- the alias family's own std140 uniform blocks: AliasFrag, what
 * salias_frag reads beyond the shared FragParams (#450, alias slice S2), and
 * AliasVert, what the vertex stages read beyond VertParams (slice S3).
 *
 * Include it after uniforms.inc, with UNIFORMS_FRAG or UNIFORMS_VERT defined;
 * each stage gets only its own block, split by stage for the reason
 * uniforms.inc gives.
 *
 * ITS OWN BLOCKS, NOT FragParams/VertParams, AND ITS OWN NAMES.  sworld_frag
 * reads u_turb, u_spans and u_lightview too, with different values, from its
 * own WorldFrag (world.inc, #450 W2).  The members here are spelled u_af* and
 * u_av* and only reach the old names through the macros below, so the two
 * families' blocks never fight over a member name, and the two stages'
 * u_lightview -- one in each block -- are different uniforms to the linker.
 *
 * GL binding point 11 (UBO_BINDING_ALIAS_FRAG) for AliasFrag and 10
 * (UBO_BINDING_ALIAS_VERT) for AliasVert, assigned from C because ES 3.00
 * cannot put layout(binding=) on a block.  FRAG_UBO(1) and VERT_UBO(1) are
 * their SDL_GPU slots, after FragParams' and VertParams' 0.  The C mirrors are
 * r_aliasfrag_t and r_aliasvert_t in gl_uniforms.h, and the buffers are
 * permanent ones flushed by R_FlushUniforms, not the GL_Upload ring, which is
 * a stub on the ES tier the alias program runs on.
 *
 * WHY AliasVert IS ITS OWN BLOCK and not a VertParams member: its first
 * member is the froxel grid's world -> eye matrix, written once a frame, while
 * the instanced draw re-flushes VertParams once per batch for u_inst_base.
 * Kept out, it rides that per-batch upload zero times instead of every time,
 * and VertParams, which every block program on every tier reads, does not
 * grow.  The skeletal stage's lighting (slice S4) is here for the second half
 * of that reason: three values only the skeletal programs read, which
 * VertParams has no spare lanes for.
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

#ifdef UNIFORMS_VERT
/* u_avlightview is salias_inst_vert's: that stage builds gl_Position from
 * u_viewproj and a world-space vertex, so it has no u_modelview to take eye
 * space from and reconstructs it with this for the froxel lookup
 * (uhexen2-waum).  The same lc_view as AliasFrag's u_aflightview, written by
 * the same R_SetAliasLightGrid call, so the two stages cannot disagree.
 *
 * u_avshade and u_avlight are sskeletal_vert's per-draw lighting (#450 S4):
 * GL_DrawAliasSkeletal writes both, as one call, before every skeletal draw.
 * Nothing else reads them. */
VERT_UBO(1) uniform AliasVert {
    highp mat4 u_avlightview;   /*   0  world -> eye, no entity transform (froxel grid) */
    highp vec4 u_avshade;       /*  64  xyz model-space shade vector, w fullbright pass */
    highp vec4 u_avlight;       /*  80  rgb light (tint and scale folded in), a entity alpha */
};                              /*  96 bytes */

#define u_lightview       u_avlightview
#define u_shadevector     u_avshade.xyz
#define u_fullbright      u_avshade.w
#define u_lightcolor      u_avlight
#endif  /* UNIFORMS_VERT */

#endif /* ALIAS_INC */
