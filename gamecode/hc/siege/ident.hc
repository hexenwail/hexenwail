/*
===============
ident.hc

The gamecode's own statement of what it is and when it last changed, so the
engine can tell our progs.dat apart from Raven's retail one and from a mod's,
and name the revision in a form a bug reporter can read back.

The date rides IN THE FUNCTION NAME, which is the only place it can.  A
`string HEXENWAIL_DATE = "..."' would not survive: hcc.c :: WriteData()
replaces a global's name with "LCL+" under -on unless the global carries
DEF_SAVEGLOBAL, and hcc only sets that for UNinitialised globals -- so the one
form that can hold a value is exactly the form that loses its name, in the
three trees built with -on (portals, hw, siege).  The strip test is
`def->type->type < ev_field', and ev_function sorts above ev_field
(common/pr_comp.h), so function names always survive it.  That is also why
`qcdis.py --list BadBackpackDump' works against the portals build.

It is a source constant and not a build timestamp because `nix build
.#gamecode --rebuild' reports no differences today (docs/GAMECODE.md), and a
clock reading would end that.  The cost is that it can go stale, which would be
worse than printing nothing -- a confident wrong date sends a bug report to the
wrong revision.  Two gates in flake.nix hold it: all five built images must
carry the SAME stamp, and it must equal the newest dated entry in
gamecode/README.  The second is the load-bearing one, because this fork's
policy is already that every divergence gets a README entry, so the build now
fails at exactly the moment someone records a change without restamping.

Why it is otherwise free.  Not an entity field, so gamecode/fieldsets stays
byte-identical and check_progs_fields.py still passes; and PR_WriteProgdefs()
CRCs only the globals up to end_sys_globals plus the field table, so the
progdefs CRC the engine reports as "H2/v1.11" is untouched.  The whole-file CRC
does move, which is expected -- docs/BUNDLED_GAMECODE.md says so.

Deliberately never called.  hcc emits every defined function into the function
table whether or not anything references it, and a call would be a behaviour
change for a marker that exists only to be looked up.

Engine side: PR_ClassifyGamecode() in engine/h2shared/pr_edict.c, which prefix-
matches GAMECODE_SENTINEL and parses the _YYYYMMDD tail.  A build carrying the
bare name with no tail still identifies, just without a date -- that is what
our own builds between uhexen2-8r3e landing and this restamping look like.
(uhexen2-8r3e)
===============
*/
void HexenwailGamecode_20260815(void)
{
}
