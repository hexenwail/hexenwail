# Hexen II demo data — provenance

Supporting material for the demo-distributable epic (`uhexen2-menr`): bundling
the Hexenwail engine with playable game data so a new user can download one
file and run it, instead of having to supply `data1/pak0.pak` themselves.

## The tarball

| | |
|---|---|
| File | `hexen2demo_nov1997-linux-i586.tgz` |
| Size | 13,422,252 bytes |
| sha256 | `2df15cde0128b7a036e71995e068ca853f13be8e2b591caac140025d66643fc0` |
| SRI | `sha256-LfFc3gEot6A25xmV4GjKhT8Tvo4rWRyqwUACXWZkP8A=` |
| Source | <https://sourceforge.net/projects/uhexen2/files/Hexen2Demo-Nov.1997/hexen2demo_nov1997-linux-i586.tgz/download> |

Upstream is the Hammer of Thyrion (uHexen2) project's repackaging of the
three-level v1.11 Hexen II demo that Raven/Activision released in November
1997. The tarball carries a full i586 Linux build of the HoT engine alongside
the data; only `hexen2demo_nov1997/data1/` is of interest here.

The upstream release notes name the bundled engine as v1.5.9, and so does
`ABOUT`, but the `DEMO.TXT` inside this particular tarball still says v1.5.8 —
upstream did not refresh that file. Either way the engine binaries are unused
by us.

`data1/` contents: `pak0.pak`, `default.cfg`, `hexen.rc`, `autoexec.cfg`,
`progs.dat`, and `maps/demo2.{txt,ent}`.

### The tarball is not in git

A fresh clone will not have it — fetch it from the URL above and verify the
sha256. It is listed in `.gitignore` so that a checkout which *does* have it
cannot pick it up by accident; before that entry existed the exclusion lived
only in this machine's `.git/info/exclude`, which clones never see.

Two consequences worth knowing before building on this directory:

- `nix build` cannot reference it as a path literal (`${./assets/demo/…tgz}`).
  A flake only sees git-tracked files, so such a derivation would work on a
  machine that happens to have the file and fail everywhere else, including CI.
  A `pkgs.fetchurl` pinned to the SRI hash above is the portable form.
- Do not "fix" this by committing the file. 13 MB of binary in git history is
  permanent, and the redistribution question below is unresolved anyway.

## Files extracted from the tarball

Kept here so the licensing terms are readable without unpacking 13 MB.

| File | Origin inside the tarball |
|---|---|
| `HEXEN2_DEMO_LICENSE.txt` | `SUBLICENSE.doc`, converted with `antiword 0.37` |
| `DEMO.TXT` | `DEMO.TXT`, byte-identical |
| `ABOUT` | `docs/ABOUT`, byte-identical |

`SUBLICENSE.doc` is an OLE2 Word 97 document; the conversion is text-only and
drops no substantive content. To reproduce:

```sh
tar xzf hexen2demo_nov1997-linux-i586.tgz -O hexen2demo_nov1997/SUBLICENSE.doc \
  | antiword - > HEXEN2_DEMO_LICENSE.txt
```

## Licensing — unresolved, read before shipping anything

`HEXEN2_DEMO_LICENSE.txt` **does not grant a redistribution right**, which is
what the epic needs.

The document is Activision's retail sublicense agreement, not a shareware or
demo license. It never uses the words "shareware", "demo", or "redistribute";
it refers throughout to "the original consumer purchaser", warranty
replacement of CD-ROM media, and running the program from "the included
CD-ROM". Under `YOU SHALL NOT` it explicitly forbids:

> Sell, rent, lease, license, distribute or otherwise transfer this Program,
> or any copies of this Program, without the express prior written consent of
> Activision.

The engine side is unaffected: Hexenwail is GPLv2+ and the HoT binaries in the
tarball are too. The question is only about the Raven game content in
`data1/pak0.pak`.

### What was searched, and what turned up

Looking for a grant that covers the demo's free circulation. Nothing found:

- **The demo's own data carries no license.** `data1/pak0.pak` was unpacked and
  all 797 entries listed: game assets, `strings.txt`, `puzzles.txt`,
  `maplist.txt`, menu graphics. No license, readme, order form, or legal file.
- **Neither do the surrounding docs.** Every file under `docs/` in the tarball
  is Hammer of Thyrion's own engine documentation, `COPYING` included — that is
  the engine's GPL, not the data's terms. `SUBLICENSE.doc` is the only document
  in the package that speaks to the game content at all.
- **Upstream's release notes say nothing about terms.** The `README` files in
  both the `Hexen2Demo-Nov.1997` and `Hexen2Demo-Aug.1997` SourceForge
  directories are four lines each, describing which engine build was used.
- **A second port reached the same conclusion independently.** The Hexen 2
  Dreamcast port states: *"Hexen II Demo version Data files have another
  Licence. In my view, Hexen II Demo version can't re-distribute without
  Activision's permission."*
- **The Hexen line was not shareware.** By the mid-90s the shareware model had
  fallen out of favour; Hexen shipped a small demo rather than a shareware
  episode, and was explicitly *not* a shareware product. So the
  freely-redistributable grant that came with, say, Quake shareware is not a
  thing to go looking for here.

The one point on the other side is practice: uHexen2 has hosted these packages
on SourceForge for roughly two decades with no visible objection. That is not a
permission, and it is not one we inherit.

**Conclusion: no redistribution right can be established.** Bundling
`data1/pak0.pak` into a Hexenwail release artifact should not proceed on the
strength of what is documented here.

Tracked as `uhexen2-3vmk`, which gates deliverables 2–5 of `uhexen2-menr` (the
flake outputs, the CI release artifacts, the Pages deploy). It stays open
because the decision it feeds — ship engine-only with a user-fetched demo, or
ask Activision — has not been made. This directory is unaffected either way:
it documents what the tarball contains, it does not ship it.
