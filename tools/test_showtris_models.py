#!/usr/bin/env python3
"""r_showtris outlines alias models, sprites, particles and the viewmodel (#81).

Ironwail re-issues each draw with a show-tris mode; ours cannot (the alias
draw carries a dozen blend branches, lighting, shadows and viewmodel
diagnostics), so each pass re-issues the GEOMETRY from what the draw used:

  - alias: R_ShowTris_Alias poses the model on the CPU with the pose pair and
    blend R_AliasResolveLerp gives the draw, under the matrix
    R_AliasModelTransform builds for it (R_DrawAliasModel calls the same
    helper; the instanced path keeps its own copy of that math, which agrees
    for every drawflags value that matches a case);
  - sprites: R_ShowTris_Sprite takes the axes and frame from R_SpriteAxes,
    which R_DrawSpriteModel also uses;
  - particles: R_DrawParticles_ShowTris is the particle draw itself, on the
    flat shader, white, triangles even in square mode, no blend state;
  - the viewmodel: R_ViewModelVisible / R_ViewModelBegin / R_ViewModelEnd are
    shared with R_DrawViewModel, depth slice in mode 2 only;
  - GLES: no glPolygonMode there, so R_ShowTris returns rather than paint
    everything solid white.

Live evidence is in the commit: mode 0 frames identical to master outside the
real-time sky; mode 1/2 outline the tree, statue, gravestones, projectile,
sprite and weapon; with live particles the particle pass issues GL_TRIANGLES
on gl_shader_flat.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


src = strip((ROOT / "engine/hexen2/gl_rmain.c").read_text())


def body(sig):
    m = re.search(re.escape(sig) + r"\s*\{(.*?)\n\}", src, re.S)
    if not m:
        errors.append(f"gl_rmain.c: no {sig}")
        return ""
    return m.group(1)


draw = body("static void R_DrawAliasModel (entity_t *e)")
alias = body("static void R_ShowTris_Alias (entity_t *e)")
for name, b in (("R_DrawAliasModel", draw), ("R_ShowTris_Alias", alias)):
    if "R_AliasModelTransform (e, paliashdr, tmatrix, mdl_t, mdl_s)" not in b and \
       "R_AliasModelTransform (e, hdr, tmatrix, mdl_t, mdl_s)" not in b:
        errors.append(f"{name} must build its matrix with R_AliasModelTransform")
if "tmatrix[0]" in draw or "tmatrix[1]" in draw or "tmatrix[2]" in draw:
    errors.append("R_DrawAliasModel still builds its own matrix; it must share R_AliasModelTransform")
for want in ("R_AliasResolveLerp (e, hdr, &pose1, &pose2, &blend);", "&gl_shader_flat",
             "PV_MD3", "R_RotateForEntity2 (e);"):
    if want not in alias:
        errors.append(f"R_ShowTris_Alias: missing {want}")
# Each caller keeps its own static tmatrix: the draw inherits leftovers for
# unmatched drawflags, as it always did, and the outline pass must not feed it.
for name, b in (("R_DrawAliasModel", draw), ("R_ShowTris_Alias", alias)):
    if not re.search(r"static\s+float\s+tmatrix\[3\]\[4\];", b):
        errors.append(f"{name} must pass R_AliasModelTransform its own static tmatrix")
if "r_entdist" not in alias:
    errors.append("R_ShowTris_Alias must honour r_entdist as the draw does")

sprite_draw = body("static void R_DrawSpriteModel (entity_t *e)")
sprite_tris = body("static void R_ShowTris_Sprite (entity_t *e)")
if "R_SpriteAxes (e, &r_spritedesc)" not in sprite_draw or "R_SpriteAxes (e, &desc)" not in sprite_tris:
    errors.append("the sprite draw and its wireframe must share R_SpriteAxes")
axes = body("static mspriteframe_t *R_SpriteAxes (entity_t *e, spritedesc_t *desc)")
if "modelorg" in axes:
    errors.append("R_SpriteAxes must not read the global modelorg (it is the last model's, not this sprite's)")

view = body("static void R_DrawViewModel (void)")
if "R_ViewModelVisible ()" not in view or "R_ViewModelBegin (true);" not in view or "R_ViewModelEnd ();" not in view:
    errors.append("R_DrawViewModel must use the shared viewmodel helpers")

tris = body("static void R_ShowTris (void)")
for want in ("R_ShowTris_Alias (e);", "R_ShowTris_Sprite (e);", "R_ViewModelBegin (r_showtris.value != 1);",
             "R_ShowTris_Alias (&cl.viewent);", "R_DrawParticles_ShowTris ();"):
    if want not in tris:
        errors.append(f"R_ShowTris: missing {want}")
raw = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
rt = raw[raw.index("static void R_ShowTris (void)"):]
if not re.search(r"#ifdef USE_GLES\s*/\*.*?\*/\s*return;\s*#endif", rt[:rt.index("glPolygonMode_fp")], re.S):
    errors.append("R_ShowTris must return on GLES, where there is no line polygon mode")

part = strip((ROOT / "engine/hexen2/r_part.c").read_text())
real = re.search(r"static void R_DrawParticles_Real \(qboolean showtris\)\s*\{(.*?)\n\}", part, re.S)
if not real:
    errors.append("r_part.c: no R_DrawParticles_Real")
else:
    b = real.group(1)
    for want in ("!gl_particles.integer && !showtris", "showtris ? &gl_shader_flat",
                 "if (showtris)\n                GL_ImmColor4f(1.0f, 1.0f, 1.0f, 1.0f);"):
        if want not in b:
            errors.append(f"R_DrawParticles_Real: missing {want!r}")
if not re.search(r"void R_DrawParticles_ShowTris \(void\)\s*\{\s*R_DrawParticles_Real \(true\);", part):
    errors.append("R_DrawParticles_ShowTris must be the particle draw in show-tris mode")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS r_showtris outlines alias models, sprites, particles and the viewmodel (#81)")
