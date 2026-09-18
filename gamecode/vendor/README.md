# gamecode/vendor — reference trees, not build inputs

## What is here

`jsHexen2-progs/` is a submodule of
[KoMiKoZa/jsHexen2-progs](https://github.com/KoMiKoZa/jsHexen2-progs), a sibling
fork of Raven's HexenC that fixes the same class of 1997 gameplay bugs we do.

## It does not build anything

`progs.dat` is compiled only from `gamecode/hc/{h2,portals,hw,siege}`. The
`gamecodeSrc` filter in `flake.nix` does not include this directory.

Our base is uHexen2 1.29c, from
[sezero/uhexen2-hcode_archive](https://github.com/sezero/uhexen2-hcode_archive),
as `gamecode/README` records. jsHexen2 is a peer, not an upstream.

## The policy is unchanged: our .hc wins

Importing means reading their change, deciding whether we agree, and writing
our own hunk with the credit recorded in `gamecode/README`. Never merge.
`gamecode/README` already records one place where we took their diagnosis
(`22b5c8a`) but not their fix.

## Keeping it up to date

`REVIEWED` records the last upstream commit that has been triaged. To see what
is outstanding:

```sh
git -C gamecode/vendor/jsHexen2-progs fetch
git -C gamecode/vendor/jsHexen2-progs log --oneline $(cat gamecode/vendor/REVIEWED)..origin/main
```

For each commit, either adopt it (write our own hunk, add a `gamecode/README`
entry with the upstream ref) or decide against it (add a `gamecode/README` note
saying why). Then bump the submodule pointer and `REVIEWED` together, so the
pointer means "reviewed to here".
