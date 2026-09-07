# HexenWorld (hwsv)

Raven's QuakeWorld-derived multiplayer fork of Hexen II.  It is a *separate
engine* that shares `h2shared` with Hexen II — its own protocol, its own
gamecode (`hwprogs.dat`), its own client prediction and master server — not a
mode of the Hexen II server.

`engine/hexenworld/` holds only the dedicated server (`hwsv`); the HexenWorld
client (`hwcl` / `glhwcl`) is not restored.  The master server and its helper
tools *are* built, but from `hw_utils/` rather than here — `hwmaster`,
`hwmquery`, `hwrcon` and `hwterm` all come out of `nix build .#utils`, which
CI already runs.

## Building

`hwsv` is behind a CMake option, default OFF:

    cmake -DBUILD_HEXENWORLD=ON ...

or via the flake:

    nix build .#hwsv

The flag is required at *configure* time — it gates whether `add_executable(hwsv)`
is reached at all, so `make hwsv` without it asks for a target the generated
build system does not contain.

## Runtime data

`hwsv` needs three things, none of which are in this repository.

### 1. Hexen II retail data — `data1/pak0.pak`, `data1/pak1.pak`

From your own Hexen II CD, patched to v1.11 (`h2patch`).  Licensed commercial
data; not redistributable.  `FS_Init` refuses to start without it, before it
ever looks at HexenWorld:

    FATAL ERROR: Unable to find a proper Hexen II installation.

Expected MD5s:

    c9675191e75dd25a3b9ed81ee7e05eff  data1/pak0.pak
    c2ac5b0640773eed9ebe1cda2eca2ad0  data1/pak1.pak

### 2. HexenWorld data — `hw/pak4.pak`

**This one is freely redistributable** and is *not* on any CD.  HexenWorld was
Raven's free beta add-on; the pak's own bundled readme reads "this is the
hexenworld pak file from Raven's latest beta release".  Hammer of Thyrion
distributes it directly:

    https://sourceforge.net/projects/uhexen2/files/Hexen2%20GameData/hexenworld-pakfiles/

    curl -LO .../hexenworld-pakfiles-0.15.tgz
    tar xzf hexenworld-pakfiles-0.15.tgz    # yields hw/pak4.pak
    md5sum hw/pak4.pak                      # 88109ee385d9723ac5f1015e034a44dd

Store it outside version control, in your game installation's `hw/` directory.
Do not commit it.  `FS_Init`'s `GAME_HEXENWORLD` gate refuses to start without
it (`engine/h2shared/quakefs.c`, "You must have the HexenWorld data installed").

The engine recognises three vintages — 0.14/0.15 (10780245 bytes, the one
above), and the older 0.11 and 0.09 betas.  Prefer 0.15.

### 3. HexenWorld gamecode — `hw/hwprogs.dat`

Built from source in this tree, so no acquisition problem. Install via the bundled package:

    nix build .#hwsv-bundled

This creates a complete installation with both the `hwsv` binary and `hw/hwprogs.dat` in the correct location.

Alternatively, you can install the gamecode manually from the gamecode package:

    nix build .#gamecode
    install -Dm644 result/share/hexenwail/hw/hwprogs.dat <gamedir>/hw/hwprogs.dat

Without it `hwsv` gets all the way through `Host_Init` and then dies:

    SV_Error: PR_LoadProgs: couldn't load hwprogs.dat

## Verified bringup

With all three in place, from the game installation directory:

    $ hwsv +map demo1
    HexenWorld server 0.29 (Linux)
    Added packfile .../data1/pak0.pak (696 files)
    Added packfile .../data1/pak1.pak (523 files)
    Playing the registered version.
    Added packfile .../hw/pak4.pak (102 files)
    IP address 0.0.0.0:26950
    UDP Initialized
    ======== HexenWorld Initialized ========
    Gamecode: hwprogs.dat from .../hw/hwprogs.dat (HW/v0.15, file crc 9155)
    Building PHS...
    Average leafs visible / hearable / total: 74 / 179 / 792

The server then stays up serving on UDP 26950.

Note the log ordering when something fails: `Sys_Error` writes to unbuffered
stderr while the banner is buffered stdout flushed at exit, so a fatal error
appears *above* the startup banner.  It does not mean the failure happened
before `Host_Init`.

## Not yet done

See GitHub issue #35.

- Two `hwsv` instances discovering each other via `hwmaster`.  Not blocked on
  building anything — `hwmaster` already ships in `.#utils`.  What is missing is
  a harness that starts a master plus two servers and asserts the heartbeat.
- A real HexenWorld client completing the connectionless handshake — `hwcl` is
  not built in this tree; needs an upstream or community client.
- Whether this engine's modern wire extensions ride over the HexenWorld protocol
  at all, or are Hexen II only.  Undecided; `hwsv` must not silently drop
  clients that advertise them.
