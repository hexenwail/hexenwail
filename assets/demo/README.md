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

Whatever permission actually covers the 1997 demo's free circulation, it is not
in this file — upstream appears to have copied the retail EULA into the demo
package. The engine side is unaffected: Hexenwail is GPLv2+ and the HoT
binaries in the tarball are too. The question is only about the Raven game
content in `data1/pak0.pak`.

Tracked separately as `uhexen2-3vmk`. That question gates deliverables 2–5 of
`uhexen2-menr` (the flake outputs, the CI release artifacts, and the release
notes) — not this directory, which only documents what the tarball contains.
