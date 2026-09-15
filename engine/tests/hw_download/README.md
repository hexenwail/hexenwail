# HexenWorld download percentage regression

`percentcheck.c` runs the server's loose-file block loop against a sparse
22 MiB + 1 byte file. That size crosses the point where `count * 100` overflows
a signed 32-bit integer. It checks monotonic protocol percentages, rejects an
early 100, and requires the final block to produce exactly 100 as the client
completion check requires.

Run it through the pinned development environment:

```bash
nix develop --command sh -c \
  'cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
    -fno-sanitize-recover=undefined \
    -Iengine/hexenworld/server engine/tests/hw_download/percentcheck.c \
    -o /tmp/hw-download-percentcheck && /tmp/hw-download-percentcheck'
```
