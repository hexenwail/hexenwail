#!/usr/bin/env python3
"""Keep software alias-model motion aligned with the GL/GLES renderer.

EF_SPIN and EF_FLOAT are per-entity misc_modelpimp flags.  The GL renderer
uses them in its alias transform and lighting path; the software renderer must
consume the same flags without sharing the low-level rasterizer.  This
source-level check pins the three semantic decisions in the production
function: spin controls yaw, float controls bobbing, and either flag selects
moving-model lighting.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "engine/hexen2/r_alias.c"
GL_SOURCE = ROOT / "engine/hexen2/gl_rmain.c"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def require(pattern, source, description):
    if not re.search(pattern, source, re.S):
        raise AssertionError(f"missing software model-flag route: {description}")


def main():
    text = SOURCE.read_text()
    gl_text = GL_SOURCE.read_text()
    require(r"R_GetPimpFlags\s*\(e,\s*NULL\)\s*&\s*EF_SPIN",
            gl_text, "GL/GLES reference uses EF_SPIN")
    require(r"R_GetPimpFlags\s*\(e,\s*NULL\)\s*&\s*EF_FLOAT",
            gl_text, "GL/GLES reference uses EF_FLOAT")

    setup = function(text, "static void R_AliasSetUpTransform (int trivial_accept)\n{")
    draw = function(text, "void R_AliasDrawModel (alight_t *plighting)\n{")

    require(r"pimp_flags\s*=\s*R_GetPimpFlags\s*\(currententity,\s*NULL\)",
            setup, "read per-entity pimp flags before transforming")
    require(r"currententity->model->flags\s*&\s*EF_ROTATE.*?pimp_flags\s*&\s*EF_SPIN",
            setup, "EF_SPIN drives the rotating yaw")
    require(r"currententity->model->flags\s*&\s*EF_ROTATE.*?pimp_flags\s*&\s*EF_FLOAT",
            setup, "EF_FLOAT drives the vertical bob")

    require(r"pimp_flags\s*=\s*R_GetPimpFlags\s*\(currententity,\s*NULL\)",
            draw, "read the same flags for model lighting")
    require(r"currententity->model->flags\s*&\s*EF_ROTATE.*?pimp_flags\s*&\s*\(EF_SPIN\s*\|\s*EF_FLOAT\)",
            draw, "either flag selects moving-model lighting")

    print("PASS: software EF_SPIN/EF_FLOAT transform and lighting routes")


if __name__ == "__main__":
    main()
