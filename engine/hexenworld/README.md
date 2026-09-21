# HexenWorld (hwsv)

Raven's QuakeWorld-derived multiplayer fork of Hexen II: a separate engine with
its own protocol, gamecode (`hwprogs.dat`), client prediction and master
server, sharing `h2shared` with Hexen II.

`engine/hexenworld/` holds the dedicated server (`hwsv`) and the shared
HexenWorld protocol/transport. There is no standalone `hwcl` or `glhwcl`;
HexenWorld client networking is a protocol mode of Hexenwail itself. The master
server and helper tools (`hwmaster`, `hwmquery`, `hwrcon`, `hwterm`) are built
from `hw_utils/` by `nix build .#utils`.

## Building

`hwsv` is behind a CMake option, default OFF, which must be set at configure
time:

    cmake -DBUILD_HEXENWORLD=ON ...

or via the flake:

    nix build .#hwsv

The HexenWorld client transport is built in by default
(`-DUSE_HEXENWORLD_CLIENT=OFF` disables it). Connect with:

    connect hw://server.example:26950

or from the menu: Multiplayer → Join a Game → HexenWorld. That screen lists
servers this client has connected to (archived in `hw_server1`..`hw_server8`),
opens a connect dialog for a new address, and reaches player setup, where the
Hostname row becomes a Spectator toggle (`hw_spectator`, read at connect, not
archived because it doubles as the spectator password). There is no
master-server browser yet. While connected, `name`, `color` and `playerclass`
reach the server as `setinfo`. Other server commands (`say`, `say_team`,
`kill`, `cmd <anything>`) go out as HexenWorld string commands.
`scripts/hw-cmd-check.sh` checks that on a live server (#212).

The client completes the connection handshake through spawn and begin for
protocols 24/25/26/100. Gameplay state, assets and presentation messages are
not yet routed into the Hexenwail client.

## Runtime data

`hwsv` needs three things, none of which are in this repository.

### 1. Hexen II retail data — `data1/pak0.pak`, `data1/pak1.pak`

From your own Hexen II copy, patched to v1.11 (`h2patch`). Not redistributable. Without it:

    FATAL ERROR: Unable to find a proper Hexen II installation.

Expected MD5s:

    c9675191e75dd25a3b9ed81ee7e05eff  data1/pak0.pak
    c2ac5b0640773eed9ebe1cda2eca2ad0  data1/pak1.pak

### 2. HexenWorld data — `hw/pak4.pak`

Freely redistributable (Raven's free beta add-on) and not on any CD. Hammer of
Thyrion distributes it:

    https://sourceforge.net/projects/uhexen2/files/Hexen2%20GameData/hexenworld-pakfiles/

    curl -LO .../hexenworld-pakfiles-0.15.tgz
    tar xzf hexenworld-pakfiles-0.15.tgz    # yields hw/pak4.pak
    md5sum hw/pak4.pak                      # 88109ee385d9723ac5f1015e034a44dd

Put it in your game installation's `hw/` directory; do not commit it. Without
it the server stops with "You must have the HexenWorld data installed". The
engine also accepts the older 0.11 and 0.09 betas; prefer 0.15.

### 3. HexenWorld gamecode — `hw/hwprogs.dat`

Built from this tree. The bundled package installs `hwsv` together with
`hw/hwprogs.dat`:

    nix build .#hwsv-bundled

Or install the gamecode by hand:

    nix build .#gamecode
    install -Dm644 result/share/hexenwail/hw/hwprogs.dat <gamedir>/hw/hwprogs.dat

Without it:

    SV_Error: PR_LoadProgs: couldn't load hwprogs.dat

### 4. String table: `hw/strings.txt` (server and client)

Obituaries, pickups and "joined the game" are indexed prints: line numbers
into HexenWorld's `strings.txt`. That table is not Hexen II's. `STR_SUICIDES`
is line 468, and the obituaries run to 592, while data1's table has 409
lines and portals' 562. Siege has its own table. No pak ships one.

**Server:** install it beside `hwprogs.dat`. `hwsv-bundled` ships it for
exactly that:

    nix build .#hwsv-bundled
    install -Dm644 result/share/hexenwail/hw/strings.txt <gamedir>/hw/strings.txt

hwsv loads an installed HW-family copy (the gamedir's own, or hw's under a
mod), never data1's or portals'. Without one it warns, falls back to whatever
`strings.txt` it finds (portals' on a mission-pack install), and the first
obituary past that table's end stops the gamecode.

**Client:** it tries these in order and needs no install step:
1. The server gamedir's own installed copy.
2. The copy shipped beside the engine for that gamedir
   (`share/hexenwail/{hw,siege}/strings.txt`, from `gamecode/res/`).
3. For a mod stacked on hw, an installed copy in hw.
4. The shipped hw copy.

Without any of them it says so once per map, and those messages stay blank
(#214).

## Verified bringup

With all three in place, from the game installation directory:

    $ hwsv +map demo1
    HexenWorld server 0.29 (Linux)
    ...
    IP address 0.0.0.0:26950
    UDP Initialized
    ======== HexenWorld Initialized ========
    Gamecode: hwprogs.dat from .../hw/hwprogs.dat (HW/v0.15, file crc 9155)

The server then serves on UDP 26950.

A fatal error goes to unbuffered stderr while the banner is buffered stdout, so
the error can appear *above* the banner. That does not mean it happened first.

## Headless checks

Both need the retail data above and skip (exit 77) without it, so CI cannot
run them. Run them locally after touching the HexenWorld client:

- `scripts/hw-smoke.sh`: signon, broadcast, map change, Siege, mod download.
- `scripts/hw-hud-check.sh`: the HUD status icons on screen (#211). The net
  icon must be absent on a live session, drawn while the server is
  SIGSTOPped, and clear again after SIGCONT. The spawn-protection rook must
  appear at spawn and be gone once spawn protection ends. Icons are matched against the pics
  in your own paks (`scripts/wadpic2ppm.py`, `scripts/hudicon-score.py`).

      nix build .#default .#hwsv-bundled -o result
      nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick \
        nixpkgs#bubblewrap nixpkgs#python3 nixpkgs#util-linux --command \
        scripts/hw-hud-check.sh --basedir ~/hexen2 \
          --client result/bin/glhexen2 --server result-1/bin/hwsv

## Not yet done

See GitHub issue #35.

- A harness that starts `hwmaster` plus two `hwsv` instances and asserts the
  heartbeat.
- Full parsing of HexenWorld server messages in the Hexenwail client.
- Whether this engine's modern wire extensions ride over the HexenWorld
  protocol. Undecided; `hwsv` must not silently drop clients that advertise
  them.
