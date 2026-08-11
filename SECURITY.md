# Security Policy

Hexenwail is a game engine descended from the 1997 Hexen II source release. Large
parts of it predate the practice of treating file and network input as hostile, so
it is best assumed that parsing untrusted content can reach exploitable code.
Reports that improve that situation are genuinely welcome.

## Reporting a vulnerability

**Do not open a public issue for a security problem.**

Use GitHub's private vulnerability reporting:
[**Report a vulnerability**](https://github.com/hexenwail/hexenwail/security/advisories/new).
That opens a private advisory visible only to the maintainers, where a proof of
concept can be attached safely.

Useful things to include, roughly in order of value:

- The crafted file or packet capture that triggers it, or a script that generates one.
- Which loader or code path it reaches, if you know.
- Whether it is a plain crash, an out-of-bounds read, or a write you can influence.
- Engine version, OS, and how you built or installed it.
- A `qconsole.log` from the crashing run (see the
  [bug report form](https://github.com/hexenwail/hexenwail/issues/new/choose)
  for where to find it).

This is a hobby project maintained in spare time. Expect a first response within a
couple of weeks rather than a couple of hours. There is no bounty. Credit is given
in the advisory and the release notes unless you would rather stay anonymous.

## Supported versions

Only the [most recent release](https://github.com/hexenwail/hexenwail/releases/latest)
is supported. The project is pre-1.0 and moves quickly; fixes land on `master` and
go out in the next tag rather than being backported.

## What is in scope

The engine parses a lot of attacker-supplied data. These are the paths worth
looking at:

| Surface | Where |
|---|---|
| Map, model and sprite loading | `engine/h2shared/model.c`, `gl_model.c`, `sv_model.c` |
| PAK archives and WAD lumps | `engine/h2shared/common.c`, `wad.c` |
| Demo playback (`.dem`) | `engine/hexen2/cl_demo.c` |
| Save game loading | `engine/hexen2/host_cmd.c` |
| Network protocol, client and server | `engine/hexen2/net_*.c`, `sv_main.c`, `cl_parse.c` |
| Dedicated server (`h2ded`) | `engine/hexen2/server/` |

The realistic threats are a malicious mod, map pack or demo file distributed to
players, and a hostile server or client on the network path — the dedicated server
in particular accepts input from anyone who can reach the port.

Memory-safety bugs reachable from any of the above are in scope even if the only
demonstrated impact is a crash, because a crash in a C parser is usually the
visible end of something worse.

Third-party libraries are in scope only where we bundle or pin them. The Nix flake
closure is scanned weekly for known CVEs by
[`.github/workflows/security-scan.yml`](.github/workflows/security-scan.yml), with
unreachable findings recorded in
[`.github/vulnix-whitelist.toml`](.github/vulnix-whitelist.toml) so the scan stays
signal. If you think something whitelisted there is actually reachable, that is a
valid report.

## What is not in scope

- **Mods executing QuakeC.** `progs.dat` is a program, and running a mod means
  running its code. That is the design, inherited from Quake. "A mod can change
  game behaviour" is not a vulnerability.
- **Console commands and cvars doing what they say**, including ones that read or
  write files within the game directory. If you can already type into the console,
  you already control the process.
- **Crashes on files you authored and loaded yourself** while developing a map or
  mod. Those are ordinary bugs — please
  [file them normally](https://github.com/hexenwail/hexenwail/issues/new/choose),
  they are still worth fixing.
- Anything requiring an already-compromised machine, or physical access.
- Findings in the original game data, in Raven's or id's servers, or in unrelated
  Hexen II ports.

## What has and has not been looked at

So you do not waste time assuming more rigour than exists:

- **There is no systematic parser audit, and no fuzzing.** Malformed-input fixes
  have landed reactively, one at a time, when something crashed in the field —
  undefined behaviour on malformed MP3 tags, a shared static buffer in the Windows
  UTF-8/UTF-16 save path, and similar. Nobody has swept the model, map or demo
  loaders end to end.
- **The dependency closure is scanned, the engine's own code is not.** The weekly
  `vulnix` run covers what the Nix flake pulls in. There is no static analyser,
  sanitizer build, or fuzz target in CI.
- CI does build the dedicated server and the map/model toolchain on both Linux and
  Windows, so code paths that never touch a renderer do not rot unnoticed.

None of this should be read as a claim that the parsers are safe. They are old, and
they were not written adversarially. That is precisely why reports are useful.
