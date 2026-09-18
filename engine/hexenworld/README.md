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
reach the server as `setinfo`.

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

## Not yet done

See GitHub issue #35.

- A harness that starts `hwmaster` plus two `hwsv` instances and asserts the
  heartbeat.
- Full parsing of HexenWorld server messages in the Hexenwail client.
- Whether this engine's modern wire extensions ride over the HexenWorld
  protocol. Undecided; `hwsv` must not silently drop clients that advertise
  them.
