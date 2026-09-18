# Hexen II demo data

Hexenwail ships engine-only. We do not host, mirror or bundle Raven's demo
data: its only license, `HEXEN2_DEMO_LICENSE.txt` (Activision's sublicense),
forbids distribution without Activision's written consent. Users fetch the
demo themselves with `scripts/get_demo.sh`, `scripts/get_demo.cmd` or
`nix run .#get-demo`.

| | |
|---|---|
| File | `hexen2demo_nov1997-linux-i586.tgz` |
| Source | <https://sourceforge.net/projects/uhexen2/files/Hexen2Demo-Nov.1997/hexen2demo_nov1997-linux-i586.tgz/download> |
| sha256 | `2df15cde0128b7a036e71995e068ca853f13be8e2b591caac140025d66643fc0` |
| SRI | `sha256-LfFc3gEot6A25xmV4GjKhT8Tvo4rWRyqwUACXWZkP8A=` |

`HEXEN2_DEMO_LICENSE.txt` (converted from `SUBLICENSE.doc`), `DEMO.TXT` and
`ABOUT` are extracted from that tarball. The tarball itself is gitignored;
do not commit it.
