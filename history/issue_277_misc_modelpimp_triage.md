# Triage: misc_modelpimp glow orb inference (issue #277)

**Issue**: misc_modelpimp still infers a glow orb on new maps (spawnflag 0/1 + glow_color but no PIMP_SF_GLOW)

## Evidence

Root cause: `PF_pimpmodel` in `engine/h2shared/pr_cmds.c:492` has a legacy inference:
```c
if (spawnflags & PIMP_SF_GLOW)
    pimp->ex_flags |= EF_GLOW;
else if (glow_color != 0) and !(spawnflags & (PIMP_SF_GLOW | PIMP_SF_ILLUMINATE))
    pimp->ex_flags |= EF_GLOW;  // legacy inference
```

The proxy (r15 mask bits 16|32|64) splits entities into:
- r15 entities (reach flags) → strict contract (no inference)
- older entities → legacy inference

**Problem**: New maps that set glow_color but no PIMP_SF_GLOW are caught by legacy inference.

## Decision needed

Options (from issue):
1. Explicit opt-out spawnflag (bit 128 PIMP_SF_NOAUTOGLOW) - cheap, no risk to shipped maps
2. Contract at map level (worldspawn key or "strict" style) - one key per new map
3. Key off gamecode ident (pr_gamecode_ident distinguishes SoT/WoK)
4. Cvar (sv_pimp_strictglow) - least good

Recommend: Option 2 (map-level contract) is best for "new content is strict, old content is legacy".

## Conclusion

**DECISION REQUIRED**. The bug exists and needs a fix at the map/content level (option 2) or code level (option 1). Either preserves backwards compatibility while fixing new content.

## Next steps

Need maintainer decision:
- Option 2: implement map-level strict contract (adds one key to worldspawn or misc_modelpimp)
- Option 1: add PIMP_SF_NOAUTOGLOW spawnflag

Either must also update issue #140 with a runtime test case.
