# Release notes style

The canonical template for Hexenwail GitHub tag bodies. It follows
[Ironwail](https://github.com/andrei-drexler/ironwail)'s release-notes voice,
for the same reason the renderer follows Ironwail's approach: consistency with
the upstream this fork tracks parity against, and a terse, scannable,
credit-dense presentation instead of a changelog dump.

Study Ironwail's v0.8.2 and v0.8.1 release bodies as worked examples.
`docs/release-notes/0.8.0-beta.r1.md` is the first Hexenwail release written to
this template; keep a copy of every tag body under `docs/release-notes/` so the
next one has a local reference.

The versioning scheme (`X.Y.Z-<phase>.rN`) is unaffected — this is presentation
only.

## Structure

In order, top to bottom:

1. **Download badges.** One `shields.io` badge per platform, each wrapped in an
   `<a>` linking the release asset directly. Always `style=for-the-badge`.
2. **Platform caveat note.** A plain text line for glibc version, build flags
   (e.g. `DO_USERDIRS=0`), bundled gamecode, or anything else a downloader needs
   to know before unzipping.
3. **Changes header.** `### Changes from [<prev-version>](<link to previous tag>):`
   — always link the previous release. Use "Changes since" when the comparison
   point is not the immediately preceding tag, and say why in a sentence.
4. **Flat bullet list.** One bullet per user-visible change, ordered big features
   → small cvars → default changes and removals. No per-category subheadings.
5. **Long-form sections** (install instructions, crash reporting, checksums) as
   `<details open><summary>...</summary>` blocks.
6. **Closing.** `**Full Changelog**: <github compare link>`.

## Voice

- Past-tense, verb-first, one line each: "Added…", "Fixed…", "Changed… to…",
  "Improved…", "Reduced…", "Removed…", "Made…".
- No marketing adjectives. No "we".
- Cvars in backticks with the default inline: "Added `scr_infoscale` cvar
  (default: 2.0), controlling debug-text canvas scale".
- Inline credits: "by @user", "(thanks to @user)". Link external mods, tools and
  examples referenced.
- Group related items as `+` / `-` sub-bullets under one parent rather than
  repeating a prefix across several top-level bullets.
- Headings in a release body are `###` only — never `#` or `##`. Keeps it compact.
- Emoji callouts: `❗ **Note**:` for general notes, `⚠️` for warnings. In headings
  use the `:warning:` shortcode; inline use the literal character.

## Never list raw commits or SHAs

This is the single biggest way a Hexenwail release body drifts from the
template. Old notes (through `0.7.9-beta.r11`) carried a `### Commits` section
listing commit subjects with short hashes. Every commit must be translated into
a user-facing line, or dropped if it is not user-visible — a bookkeeping,
documentation or pure-refactor commit does not earn a bullet. State the omitted
count in a closing sentence instead:

> This rolls up 265 commits since June 13. About 150 of those were issue-tracker
> bookkeeping, documentation, or pure internal refactors with no user-visible
> effect and are omitted above.

## Badges

Release body — one per shipped asset, `<TAG>` substituted throughout:

```html
<a href="https://github.com/hexenwail/hexenwail/releases/download/<TAG>/hexenwail-<TAG>-windows-x86_64.zip">![Download Windows build](https://img.shields.io/github/downloads/hexenwail/hexenwail/<TAG>/hexenwail-<TAG>-windows-x86_64.zip?color=blue&label=Windows%20x86_64&logo=windows&style=for-the-badge)</a>
<a href="https://github.com/hexenwail/hexenwail/releases/download/<TAG>/hexenwail-<TAG>-linux-x86_64.zip">![Download Linux build](https://img.shields.io/github/downloads/hexenwail/hexenwail/<TAG>/hexenwail-<TAG>-linux-x86_64.zip?color=8A9A5B&label=Linux%20x86_64&style=for-the-badge)</a>
<a href="https://github.com/hexenwail/hexenwail/releases/download/<TAG>/hexenwail-<TAG>.flatpak">![Download Flatpak](https://img.shields.io/github/downloads/hexenwail/hexenwail/<TAG>/hexenwail-<TAG>.flatpak?color=4A90D9&label=Flatpak&logo=flatpak&style=for-the-badge)</a>
```

Colors are not free choices:

| Platform | Color | Logo | Origin |
|---|---|---|---|
| Windows | `blue` | `windows` | Ironwail standard |
| Linux | `8A9A5B` (muted olive) | none | Ironwail standard |
| Flatpak | `4A90D9` | `flatpak` | Hexenwail-specific; Ironwail ships no flatpak |

README — a single static badge above the top-level heading, pointing at
`/releases/latest` via the version endpoint relabeled "Download". Renders as
`[DOWNLOAD | 0.8.0-beta.r1]`:

```markdown
[![Download](https://img.shields.io/github/v/release/hexenwail/hexenwail?display_name=release&style=for-the-badge&label=Download)](https://github.com/hexenwail/hexenwail/releases/latest)
```

## Keep what Hexenwail does better than Ironwail

Adopting the voice does not mean dropping content Ironwail has no equivalent
for. Carry these forward into every restyled body:

- **Crash-reporting instructions** — how to attach `glh2.RPT` and `qconsole.log`
  to a bug report.
- **SHA256SUMS verification** — the checksums, and how to verify a download
  against them.
- **Per-platform install and run instructions** — Windows, Linux, Flatpak.
- **Requirements line** — e.g. "**Requirements:** OpenGL 4.3 capable GPU (2012+)".
- **Warnings in release _titles_.** Hexenwail warns at the title level (the
  broken-audio `r5`–`r10` releases); Ironwail only banners the body. The title is
  visible from the releases list without opening anything, so keep it — this is
  an improvement on the template, not a deviation from it.

## Superseded releases

When a release is replaced, retroactively prepend to its body:

```markdown
### :warning: Note: this release is outdated, please download the [latest one](https://github.com/hexenwail/hexenwail/releases/latest) instead.
```

Tag-level bookkeeping (which old tags are deliberately kept, which schemes are
retired) lives in `docs/RETIRED_TAGS.md`, not here.
