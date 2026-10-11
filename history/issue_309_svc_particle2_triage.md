# Triage: HexenWorld svc_particle2 (issue #309)

**Issue**: `HWCL_ParseParticle2/3/4` cast the HexenWorld wire particle type straight to Hexen II's `ptype_t`, corrupting wire types ≥ 24.

## Evidence

Wire types 24-28 of HW don't exist in H2's ptype_t:

| HW wire type | HW meaning | Wire cast → H2 meaning |
|---|---|---|
| 24 | pt_darken | pt_acidball (wrong) |
| 25 | pt_grensmoke | pt_darken (wrong) |
| 26 | pt_redfire | pt_snow (wrong) |
| 27 | pt_acidball | pt_gravwell (wrong) |
| 28 | pt_bluestep | pt_redfire (wrong) |

The fix direction: replace the cast with a mapping table from HW wire type to H2 ptype_t. Map pt_bluestep to nearest H2 equivalent (pt_grensmoke) or port it. Out-of-range values should become a safe default.

## Affected code

- `engine/hexen2/cl_hw.c:1048` (`HWCL_ParseParticle2`)
- `engine/hexen2/cl_hw.c:1065` (`HWCL_ParseParticle3`)
- `engine/hexen2/cl_hw.c:1081` (`HWCL_ParseParticle4`)

## Conclusion

**REAL bug.** All three parsers need the same mapping-table fix. Worth implementing as a dedicated change, scoped to the particle fix only (no renderer changes).
