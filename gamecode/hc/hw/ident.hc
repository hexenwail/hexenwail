/*
===============
ident.hc

The gamecode's own statement of what it is, so the engine can tell our
progs.dat apart from Raven's retail one and from a mod's.

Why a function and not a global.  hcc.c :: WriteData() replaces a global's
name with "LCL+" under -on unless the global carries DEF_SAVEGLOBAL, and
hcc.c only sets that flag for UNinitialized globals -- so a
`string HEXENWAIL_GAMECODE = "..."` would lose its name in exactly the three
trees built with -on (portals, hw, siege).  The strip test is
`def->type->type < ev_field', and ev_function sorts above ev_field
(common/pr_comp.h), so function names always survive it.  That is also why
`qcdis.py --list BadBackpackDump' works against the portals build.

Why it is safe to add.  It is not an entity field, so gamecode/fieldsets stays
byte-identical and check_progs_fields.py still passes; and PR_WriteProgdefs()
CRCs only the globals up to end_sys_globals plus the field table, so the
progdefs CRC the engine reports as "H2/v1.11" is untouched.  The whole-file
CRC does move, which is expected -- docs/BUNDLED_GAMECODE.md says so.

Deliberately never called.  hcc emits every defined function into the function
table whether or not anything references it, and a call would be a behaviour
change for a marker that exists only to be looked up.

Engine side: PR_ClassifyGamecode() in engine/h2shared/pr_edict.c.  Keep the
name in sync with GAMECODE_SENTINEL there; nothing checks it automatically,
which is the one weakness of this scheme.  (uhexen2-8r3e)
===============
*/
void HexenwailGamecode(void)
{
}
