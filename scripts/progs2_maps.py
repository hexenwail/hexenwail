#!/usr/bin/env python3
"""Report which retail Hexen II maps run progs2.dat, and how a player reaches them.

data1/pak0.pak carries a 193-byte maplist.txt naming ten maps that load
progs2.dat instead of progs.dat.  Five of those ten (rick3-rick7) ship in no
retail pak, so the number a player actually meets is smaller than the maplist
suggests -- see docs/GAMECODE.md.  This script re-derives both numbers from an
installed copy of the game rather than taking the maplist at its word.

Usage:  scripts/progs2_maps.py [hexen2-install-dir]     (default: ~/.hexen2)
"""

import os
import re
import struct
import sys

PAKS = ("data1/pak0.pak", "data1/pak1.pak", "portals/pak3.pak")


def pak_entries(path):
	"""Yield (name, offset, length, blob) for every file in a Quake .pak."""
	with open(path, "rb") as f:
		blob = f.read()
	if blob[:4] != b"PACK":
		raise ValueError("%s is not a .pak file" % path)
	diroff, dirlen = struct.unpack("<ii", blob[4:12])
	for i in range(dirlen // 64):
		rec = blob[diroff + i * 64 : diroff + (i + 1) * 64]
		name = rec[:56].split(b"\0")[0].decode("latin-1")
		off, length = struct.unpack("<ii", rec[56:64])
		yield name, off, length, blob


def bsp_entities(blob):
	"""Return the text of a BSP's entity lump (lump 0, right after the version)."""
	off, length = struct.unpack("<ii", blob[4:12])
	return blob[off : off + length].decode("latin-1", "replace")


def main(base):
	paks = [os.path.join(base, p) for p in PAKS]
	paks = [p for p in paks if os.path.exists(p)]
	if not paks:
		sys.exit("no retail .pak files under %s -- pass the install dir as argv[1]" % base)

	maps = {}		# "meso9" -> (pak, size)
	changelevel = {}	# target map -> [source maps]
	maplist = None

	for pak in paks:
		for name, off, length, blob in pak_entries(pak):
			lname = name.lower()
			if lname.endswith("maplist.txt") and maplist is None:
				maplist = (pak, blob[off : off + length].decode("latin-1"))
			elif lname.endswith(".bsp"):
				short = os.path.basename(lname)[:-4]
				maps.setdefault(short, (pak, length))
				for m in re.finditer(r'"map"\s*"([^"]+)"', bsp_entities(blob[off : off + length])):
					changelevel.setdefault(m.group(1).strip().lower(), []).append(short)

	if maplist is None:
		sys.exit("no maplist.txt in %s -- progs2.dat is unreachable in this install" % base)

	pak, text = maplist
	fields = text.split()
	count = int(fields[0])
	listed = [(fields[1 + 2 * i], fields[2 + 2 * i]) for i in range(count)]

	print("maplist.txt: %s, %d entries" % (pak, count))
	print("scanned %d maps across %d pak(s)\n" % (len(maps), len(paks)))
	print("%-10s %-12s %-9s %s" % ("map", "progs", "in pak", "reached from"))
	print("%-10s %-12s %-9s %s" % ("-" * 10, "-" * 12, "-" * 9, "-" * 20))

	reachable = []
	for name, progs in listed:
		where = "yes" if name in maps else "MISSING"
		sources = sorted(set(changelevel.get(name, [])))
		if name in maps and sources:
			reachable.append(name)
		print("%-10s %-12s %-9s %s" % (name, progs, where, ", ".join(sources) or "-- nothing links here --"))

	print("\nmaps a player can reach that use progs2.dat: %d" % len(reachable))
	print("  %s" % ", ".join(reachable))
	return 0


if __name__ == "__main__":
	base = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/.hexen2")
	sys.exit(main(base))
