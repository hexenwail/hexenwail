# gamecode/vendor — reference trees, not build inputs

## What is here

`jsHexen2-progs/` is a submodule of
[KoMiKoZa/jsHexen2-progs](https://github.com/KoMiKoZa/jsHexen2-progs), a sibling
fork of Raven's HexenC that fixes the same class of 1997 gameplay bugs we do.
It is here so that "what have they found that we have not looked at?" is a
`git log` away instead of a manual browse.

## It does not build anything

`progs.dat` is compiled from `gamecode/hc/{h2,portals,hw,siege}` and from
nowhere else. `flake.nix`'s `gamecodeSrc` filter admits only `gamecode/hc/**`
and `gamecode/fieldsets/**`, so this directory is not even visible to the
build — the exclusion is structural, not a convention someone has to remember.

That is deliberate. It is also not our upstream: **our base is uHexen2 1.29c**,
from [sezero/uhexen2-hcode_archive](https://github.com/sezero/uhexen2-hcode_archive),
as `gamecode/README` records. jsHexen2 is a peer that started from the same
Raven releases, not an ancestor. Pointing the build at it would replace our
tree wholesale, discarding every divergence in `gamecode/README`.

## The policy is unchanged: our .hc wins

`gamecode/README` states it and this does not reverse it. Importing means
reading their change, deciding whether we agree, and writing our own hunk with
the credit recorded in `gamecode/README` — never a merge.

We already disagree with this tree in one recorded place. `22b5c8a` is where
the `RandomMonsterGoodies` self-vs-item mechanism was worked out and we took
that diagnosis (`uhexen2-9gdx`), but its handling of the `CLASS_HENCHMAN` tail
keeps Raven's dead zone and only stops miscounting it, where `uhexen2-h77u`
closes the zone outright. Two of that commit's other hunks (`uhexen2-9r3n`,
`uhexen2-hwky`) we had already fixed independently. An auto-sync would
re-litigate all three on every pull, which is the reason this is a reference
tree rather than a merge source.

## Keeping it up to date

`REVIEWED` records the last upstream commit that has been triaged. To see what
is outstanding:

```sh
git -C gamecode/vendor/jsHexen2-progs fetch
git -C gamecode/vendor/jsHexen2-progs log --oneline $(cat gamecode/vendor/REVIEWED)..origin/main
```

For each commit: adopt it (write our own hunk, add a `gamecode/README` entry
with the upstream ref), or decide against it (add a `gamecode/README` note
saying why — a rejection nobody wrote down gets re-proposed). Then bump the
submodule pointer and `REVIEWED` together, so the pointer always means
"reviewed to here" rather than "fetched to here".
