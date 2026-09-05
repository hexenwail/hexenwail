---
name: version-bump
description: Cut a Hexenwail release revision — bump HW_BASE_VERSION, write the release notes, commit, annotate a tag from those notes, and push branch + tag. Use when asked to "bump the version", "cut r28", "version bump, tag, push", "tag a release", or to write release notes for the commits since the last tag. Covers the traps this repo has: the shared worktree, the fetch that fails on tag clobber, and the tag range that is not the previous bump commit.
---

# Cutting a Hexenwail revision

Five files' worth of process, four of which are one line each. The work is the
release notes; everything around them is a checklist.

**Do the steps in order.** The tag annotation is the notes file, so the notes
must exist and be right before the tag is made, and the tag must be made after
the bump commit or it points at the wrong tree.

## 0. Read the current version. Do not infer it.

```bash
grep -n 'HW_BASE_VERSION' engine/hexen2/quakedef.h     # line 29
```

The tag name and `HW_BASE_VERSION` must match **exactly**, character for
character. Scheme is `X.Y.Z-<phase>.rN` — `0.8.0-beta.r27` → `0.8.0-beta.r28`.
Only `rN` moves for an ordinary revision.

## 1. Find what is in the release

Range from the **previous tag**, not the previous `version(bump)` commit:

```bash
git log --format='%s' 0.8.0-beta.r27..HEAD | grep -vE '^(chore|docs)'
git rev-list --count 0.8.0-beta.r27..HEAD          # the rollup number for the notes
```

> **The two are not the same range.** A branch merged after the tag carries
> commits whose *dates* precede it, so they belong to this release even though
> they look older than the last one. r28 picked up the maps-menu PR this way.
> Using the bump commit as the floor silently drops them.

`.claude/agents/changelog-writer.md` exists to draft the body from this range in
the house voice. Use it if you want a first pass — but only when agents are in
play for the session, and read what it returns before shipping it.

## 2. Write `docs/release-notes/<version>.md`

**Start from the previous file.** Most of it is boilerplate that must survive
verbatim; retyping it is how a Downloads section drifts.

```bash
cp docs/release-notes/0.8.0-beta.r27.md docs/release-notes/0.8.0-beta.r28.md
```

Then change exactly these, and check each one:

| What | Where |
|---|---|
| Version in all **three** download badge URLs | top of file |
| `### Changes from [<prev>](...tag/<prev>)` | heading + its link |
| The bullets | the whole middle |
| `This rolls up N commits since <prev>.` | after the bullets |
| `**Full Changelog**: ...compare/<prev>...<new>` | last line |

Leave **Downloads**, **Compiled gamecode**, **Reporting a crash** and
**Requirements** alone unless something in them actually changed.

### Voice

Imitates Ironwail's release notes. Flat list, past tense, verb first:

- `Added \`r_litwater\` (default: 1) — liquid surfaces keep their lightmap`
- `Fixed the demo playback bar re-appearing on every keystroke while the menu was open`

Sub-points hang off a bullet with `+`, indented two spaces. Name the cvar and
its default in backticks. Say what changed for the player, then why if the why
is not obvious — a note that explains a bug's mechanism is worth more than one
that names the file it was in.

For a large release, group under bold headings (`**Movement and physics**`,
`**Lighting and the renderer**`, `**Models**`, …) and lead with whatever the
release is actually about. A small one is a flat list with no headings — see r27.

Close with the rollup sentence, and say plainly that bookkeeping and doc commits
are omitted. The count is every commit in the range, not just the listed ones.

## 3. Bump, build, verify

Edit line 29 of `engine/hexen2/quakedef.h`, then **prove the binary agrees**:

```bash
nix build .#default
./result/bin/glhexen2 -version        # must print the new version
```

Do not skip this. `HW_GIT_VERSION` can override `HW_BASE_VERSION` at build time,
so the file saying r28 is not the same as the engine saying r28.

## 4. Commit, then tag from the notes file

```bash
git add engine/hexen2/quakedef.h docs/release-notes/0.8.0-beta.r28.md
git commit -m "version(bump) 0.8.0-beta.r28"    # body: what the release is about
git tag -a 0.8.0-beta.r28 -F docs/release-notes/0.8.0-beta.r28.md
```

`-F <notes>` is the point: **the notes file is the tag annotation and the GitHub
release body.** One source, three places.

Check it took:

```bash
git tag -l --format='%(refname:short)  %(objecttype)' 0.8.0-beta.r28   # want: tag
```

`objecttype tag` means annotated. `commit` means you made a lightweight tag and
the annotation is gone — delete and redo.

## 5. Push

**The remote is `hexenwail`, not `origin`.**

```bash
git fetch hexenwail master
git log --oneline hexenwail/master -3        # see step 5a before assuming
git push hexenwail master
git push hexenwail 0.8.0-beta.r28
```

Confirm both landed:

```bash
git rev-list --left-right --count hexenwail/master...HEAD    # want: 0  0
git ls-remote --tags hexenwail | grep r28                    # want: 2 lines
```

Two lines is correct for an annotated tag — the tag object and its `^{}`
dereference.

### 5a. This worktree is shared. Check before you assume.

Other Claude sessions commit and push here. A branch push from any of them
carries **your** unpushed commits along with theirs, so `git log hexenwail/master`
can already contain work you never pushed. Read the remote log before saying what
you are about to push, or you will report pushing three commits when only one
was yours.

The same goes the other way: `git status` may show files modified by another
session. **Stage by explicit path, never `git add -A` or `git commit -a`.**

### 5b. `git fetch --all --tags` fails in this repo

```
! [rejected] shanjaq-r6303-archive-2026-05-31 -> ... (would clobber existing tag)
error: could not fetch hexenwail
```

Local and remote disagree about the two `shanjaq-r6303-archive-*` tags. This is
pre-existing and unrelated to any release. Fetch the branch by name — `git fetch
hexenwail master` — and move on. Do not `--force` the tags to get past it; those
archive markers are the r6303 reference point and which side is right has not
been decided.

## What this does NOT do

Bump, tag and push only. It does **not** cut a GitHub release, and the download
badges in the notes will 404 until someone does.

That needs built artifacts — Windows zip, Linux zip, Flatpak, `SHA256SUMS` —
and then:

```bash
gh release create 0.8.0-beta.r28 --notes-file docs/release-notes/0.8.0-beta.r28.md <artifacts>
```

> `flake.nix`'s devShell banner advertises `./build-release.sh [nix|cmake]`.
> **That script is not in the tree.** The banner is stale; don't go looking.

## The whole thing, on one screen

```bash
PREV=0.8.0-beta.r27 NEW=0.8.0-beta.r28

grep -n HW_BASE_VERSION engine/hexen2/quakedef.h
git log --format='%s' $PREV..HEAD | grep -vE '^(chore|docs)'
git rev-list --count $PREV..HEAD
cp docs/release-notes/$PREV.md docs/release-notes/$NEW.md
#   ... edit notes, then edit quakedef.h line 29 ...
nix build .#default && ./result/bin/glhexen2 -version
git add engine/hexen2/quakedef.h docs/release-notes/$NEW.md
git commit -m "version(bump) $NEW"
git tag -a $NEW -F docs/release-notes/$NEW.md
git fetch hexenwail master && git log --oneline hexenwail/master -3
git push hexenwail master && git push hexenwail $NEW
git rev-list --left-right --count hexenwail/master...HEAD
```
