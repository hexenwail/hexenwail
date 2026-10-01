/* Client-only facts read by net_loop.c, exposed without copying client/server
 * structs into Rust.  The packet, hostcache and connection logic live in Rust.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Hexenwail contributors.
 */
#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "net_defs.h"

int Loop_TargetDedicated(void) { return cls.state == ca_dedicated; }
int Loop_TargetServerActive(void) { return sv.active; }
const char *Loop_TargetMapName(void) { return sv.name; }
int Loop_TargetMaxClients(void) { return svs.maxclients; }
const char *Loop_TargetHostname(void) { return hostname.string; }
