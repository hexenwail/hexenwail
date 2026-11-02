// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the map spawn of engine/hexen2/sv_main.c:
// SV_SpawnServer, and the map_checks readout it prints, SV_PrintMapChecklist
// with SV_MapCheckThresh and SV_MapCheckTitle.  E2 of #469 slice E, of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// SV_SpawnServer is the start of every level: it settles the cvars, clears
// the hunk and the server (Host_ClearMemory), loads the progs and sniffs them
// for the protocol they need, allocates the per-map server memory, loads the
// world model and its submodels, spawns the entities (ED_LoadFromFile), runs
// two settle frames, builds the baselines and sends serverinfo to every
// connected client.  With map_checks (or developer) on it then prints the
// checklist.  The C originals moved verbatim into
// engine/rust/sv_main/tests/sv_main_spawn_h2.c.
//
// Hexen II only: hwsv's SV_SpawnServer is HexenWorld's own sv_init.c.  The
// layouts, constants and globals come through engine/rust/sv_main_target.c,
// which is compiled per target.
//
// EVERYTHING ELSE IS THE ENGINE'S C, CALLED AS THE C CALLS IT
//
//   Host_ClearMemory, PR_LoadProgs, PR_HasField, Host_LoadStrings,
//   Hunk_AllocName, SZ_Init, EDICT_NUM, ED_ClearEdict, Mod_ForName,
//   SV_ParseWorldspawnSky, SV_ClearWorld, ED_LoadFromFile, SV_Physics,
//   SaveGamestate, Cvar_Set, Cvar_SetValue, PR_SetEngineString, va,
//   q_strlcpy, q_snprintf, q_strncasecmp, COM_SkipPath, FS_FileExists,
//   SV_GetLevelname, SV_ClientInventoryPage, CON_Printf; and slice D's
//   SV_SendReconnect, SV_CreateBaseline, SV_SendServerinfo.  Several of
//   those are Rust themselves (sizebuf, cvar, world_link, sv_phys_move,
//   quakefs, sv_main_connect, ...): they are declared extern here, as
//   sv_main_connect.rs declares Cmd_ExecuteString, so that this feature does
//   not drag their modules into a build of it and its differential harness
//   can record each call, in order, with its arguments.  SV_PrintMapCheck is
//   C-variadic and stays C (engine/rust/sv_main_variadic.c).
//
// C SEMANTICS KEPT
//
//   * The `#if !defined(SERVERONLY)` arms (scr_centertime_off, the loading
//     plaque counters and D_ShowLoadingSize, SCR_EndLoadingPlaque on the
//     failure path) and SV_PrintMapChecklist's `#ifndef SERVERONLY` sky
//     texture block run when the target's facts say client_build; the
//     `#ifdef GLQUAKE` flush_textures line when they say glquake_build.  These
//     are build-time facts, not isDedicated: glhexen2 -dedicated compiled
//     the client arms and ran them, so it still does.
//   * `(int) max_edicts.value` and `(int) sv.edicts->v.message` are the
//     target's own float-to-int conversion (world_hull.rs's cvt_c).
//   * current_skill is clamped in the global, then re-read for
//     Cvar_SetValue's float, as the C does.
//   * `coop.integer` picks the branch and `coop.value` is what is stored;
//     `deathmatch.value` likewise.  `*sv_globals.serverflags` is the int
//     converted to float.
//   * The protocol sniff negates first_statement with wrapping arithmetic:
//     -INT_MIN is the one input the C leaves undefined, and the wrapped
//     value (INT_MIN) is outside 107..112, which is also what the C's
//     compiled code concludes.
//   * The hunk sizes: `svs.maxclients * sizeof(client_state2_t)` and
//     `MAX_ITEMS_EX * sizeof(ex_item_t)` are size_t products narrowed to
//     Hunk_AllocName's int; `sv_max_edicts*pr_edict_size` is an int product;
//     `(svs.maxclients * MAX_INVENTORY_EX_PAGES) * sizeof(...)` is an int
//     product widened to size_t.  Each is computed in that type.
//   * The server name and startspot are passed through as the caller's
//     pointers, never copied ahead: Host_ClearMemory zeroes sv, so a caller
//     whose `server` points into sv.name sees "" from then on in both.
//   * Every global the C reads in a loop condition (svs.maxclients,
//     max_temp_edicts.integer, sv.worldmodel->numsubmodels, ...) is re-read
//     on every iteration, and host_client is the global the serverinfo loop
//     walks, left one past the last client as the C leaves it.
//   * The two "auto-upgraded" lines carry the C's UTF-8 em dash, E2 80 94.
//   * `static char dummy[8]` is C storage (SvMainSpawn_PrecacheDummy in
//     sv_main_target.c): one zeroed, writable object for the life of the
//     process, the same address every map, as the C's function-local static.
//   * SV_MapCheckTitle works on bytes: below 32 and 127 become one space, no
//     leading, doubled or trailing space; 128..255 pass through.
//
// DEPARTURES FROM THE C, UNDEFINED BEHAVIOUR ONLY
//
//   * A world model with MAX_MODELS (2048) or more submodels: the C reads
//     localmodels[] past its end and writes sv.model_precache[] and
//     sv.models[] past theirs.  The port's index check aborts instead.  No
//     loader caps numsubmodels; the model cache (MAX_MOD_KNOWN, also 2048)
//     Host_Errors first unless the world and its "*N" entries are all it
//     holds, which a first map load on h2ded can be.  Filed separately.
//   * A target whose MAX_OSPATH exceeds 4096 bytes would abort at the
//     checklist's pointfile buffer; no target's does (256, or PATH_MAX).

use core::ffi::{c_char, c_double, c_float, c_int, c_uint, c_void};

use crate::world_hull::cvt_c;

/// sv_main_target.h's `sv_main_spawn_facts_t`.
#[repr(C)]
pub struct SvMainSpawnFacts {
    pub client_build: c_int,
    pub glquake_build: c_int,
    pub print_normal: c_int,
    pub print_devel: c_int,
    pub print_safe: c_int,
    pub mapcheck_failed: c_int,
    pub mapcheck_partial: c_int,
    pub mapcheck_ok: c_int,
    pub mapcheck_na: c_int,
    pub protocol_uh2_114: c_int,
    pub protocol_hexenwail_1: c_int,
    pub min_edicts: c_int,
    pub max_edicts: c_int,
    pub max_models: c_int,
    pub max_qpath: c_int,
    pub max_ospath: c_int,
    pub max_signon_size: c_int,
    pub max_signon_buffers: c_int,
    pub max_items_ex: c_int,
    pub max_inventory_ex_pages: c_int,
    pub solid_bsp: c_int,
    pub movetype_push: c_int,
    pub ss_loading: c_int,
    pub ss_active: c_int,
    pub sv_active: usize,
    pub sv_paused: usize,
    pub sv_time: usize,
    pub sv_name: usize,
    pub sv_name_size: usize,
    pub sv_midi_name: usize,
    pub sv_mod_name: usize,
    pub sv_mod_name_size: usize,
    pub sv_cd_track: usize,
    pub sv_startspot: usize,
    pub sv_startspot_size: usize,
    pub sv_modelname: usize,
    pub sv_modelname_size: usize,
    pub sv_worldmodel: usize,
    pub sv_model_precache: usize,
    pub sv_models: usize,
    pub sv_sound_precache: usize,
    pub sv_states: usize,
    pub sv_num_edicts: usize,
    pub sv_edicts: usize,
    pub sv_state: usize,
    pub sv_datagram: usize,
    pub sv_datagram_buf: usize,
    pub sv_datagram_buf_size: usize,
    pub sv_reliable_datagram: usize,
    pub sv_reliable_datagram_buf: usize,
    pub sv_reliable_datagram_buf_size: usize,
    pub sv_signon: usize,
    pub sv_num_signon_buffers: usize,
    pub sv_signon_size: usize,
    pub sv_signon_bufs: usize,
    pub sv_ex_items: usize,
    pub sv_ex_inventory_pages: usize,
    pub sv_num_ex_items: usize,
    pub mc_active: usize,
    pub mc_numwarnings: usize,
    pub mc_changelevel: usize,
    pub mc_changespot: usize,
    pub mc_trigger_changelevel: usize,
    pub mc_valid_changelevel: usize,
    pub mc_intermission: usize,
    pub mc_skill_triggers: usize,
    pub mc_coop_spawns: usize,
    pub mc_dm_spawns: usize,
    pub mc_sp_spawns: usize,
    pub mc_skill_ents: usize,
    pub svs_maxclients: usize,
    pub svs_clients: usize,
    pub svs_serverflags: usize,
    pub svs_changelevel_issued: usize,
    pub client_size: usize,
    pub client_active: usize,
    pub client_edict: usize,
    pub client_send_all_v: usize,
    pub client_ex_inventory: usize,
    pub edict_free: usize,
    pub edict_freetime: usize,
    pub edict_v: usize,
    pub v_model: usize,
    pub v_modelindex: usize,
    pub v_solid: usize,
    pub v_movetype: usize,
    pub v_message: usize,
    pub sizebuf_name: usize,
    pub sizebuf_cursize: usize,
    pub client_state2_size: usize,
    pub ex_item_size: usize,
    pub ex_item_id: usize,
    pub ex_item_icon: usize,
    pub ex_inventory_page_size: usize,
    pub progs_numfunctions: usize,
    pub progs_entityfields: usize,
    pub function_size: usize,
    pub function_first_statement: usize,
    pub g_coop: usize,
    pub g_deathmatch: usize,
    pub g_randomclass: usize,
    pub g_mapname: usize,
    pub g_startspot: usize,
    pub g_serverflags: usize,
    pub qmodel_name: usize,
    pub qmodel_numsubmodels: usize,
    pub qmodel_lightdata: usize,
    pub qmodel_visdata: usize,
    pub qmodel_entities: usize,
    pub qmodel_numtextures: usize,
    pub qmodel_textures: usize,
    pub texture_name: usize,
    pub texture_width: usize,
    pub texture_height: usize,
    pub localmodel_size: usize,
}

// Layout accessors, for engine/rust/tests/abi_layout.c (the facts table)
// and the differential harness.
macro_rules! accessor {
    ($name:ident, $e:expr) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            $e
        }
    };
}
accessor!(
    SvMainSpawnFacts_sizeof,
    core::mem::size_of::<SvMainSpawnFacts>()
);
accessor!(
    SvMainSpawnFacts_offsetof_ss_active,
    core::mem::offset_of!(SvMainSpawnFacts, ss_active)
);
accessor!(
    SvMainSpawnFacts_offsetof_sv_active,
    core::mem::offset_of!(SvMainSpawnFacts, sv_active)
);
accessor!(
    SvMainSpawnFacts_offsetof_mc_skill_ents,
    core::mem::offset_of!(SvMainSpawnFacts, mc_skill_ents)
);
accessor!(
    SvMainSpawnFacts_offsetof_g_serverflags,
    core::mem::offset_of!(SvMainSpawnFacts, g_serverflags)
);
accessor!(
    SvMainSpawnFacts_offsetof_localmodel_size,
    core::mem::offset_of!(SvMainSpawnFacts, localmodel_size)
);

extern "C" {
    static SvMainSpawn_TargetFacts: SvMainSpawnFacts;
    fn SvMainSpawn_Server() -> *mut u8;
    fn SvMainSpawn_Static() -> *mut u8;
    fn SvMainSpawn_Globals() -> *mut u8;
    fn SvMainSpawn_Progs() -> *mut u8;
    fn SvMainSpawn_Functions() -> *mut u8;
    fn SvMainSpawn_PhysicsWalk() -> *mut c_int;
    fn SvMainSpawn_CurrentSkill() -> *mut c_int;
    fn SvMainSpawn_HostFrametime() -> *mut c_double;
    fn SvMainSpawn_Protocol() -> *mut c_int;
    fn SvMainSpawn_ProtocolUserset() -> c_int;
    fn SvMainSpawn_MaxEdicts() -> *mut c_int;
    fn SvMainSpawn_LocalModels() -> *mut c_char;
    fn SvMainSpawn_GamedirNoPath() -> *const c_char;
    fn SvMainSpawn_HostClient() -> *mut *mut u8;
    fn SvMainSpawn_EdictSize() -> c_int;
    fn SvMainSpawn_PrecacheDummy() -> *mut c_char;
    fn SvMainSpawn_HostnameString() -> *const c_char;
    fn SvMainSpawn_CoopInteger() -> c_int;
    fn SvMainSpawn_CoopValue() -> c_float;
    fn SvMainSpawn_DeathmatchValue() -> c_float;
    fn SvMainSpawn_SkillInteger() -> c_int;
    fn SvMainSpawn_MapChecksInteger() -> c_int;
    fn SvMainSpawn_DeveloperInteger() -> c_int;
    fn SvMainSpawn_MaxEdictsValue() -> c_float;
    fn SvMainSpawn_MaxTempEdictsInteger() -> c_int;
    fn SvMainSpawn_RandomclassValue() -> c_float;
    fn SvMainSpawn_CenterTimeOff() -> *mut c_float;
    fn SvMainSpawn_TotalLoadingSize() -> *mut c_int;
    fn SvMainSpawn_CurrentLoadingSize() -> *mut c_int;
    fn SvMainSpawn_LoadingStage() -> *mut c_int;
    fn SvMainSpawn_ShowLoadingSize();
    fn SvMainSpawn_EndLoadingPlaque();
    fn SvMainSpawn_FlushTextures() -> *mut c_int;

    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...);
    fn SV_PrintMapCheck(status: c_int, format: *const c_char, ...);
    fn Cvar_Set(var_name: *const c_char, value: *const c_char);
    fn Cvar_SetValue(var_name: *const c_char, value: c_float);
    fn SaveGamestate(clients_only: c_int) -> c_int;
    fn SV_SendReconnect();
    fn q_strncasecmp(s1: *const c_char, s2: *const c_char, n: usize) -> c_int;
    fn Host_ClearMemory();
    fn q_strlcpy(dst: *mut c_char, src: *const c_char, size: usize) -> usize;
    fn q_snprintf(str: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn PR_LoadProgs();
    fn PR_HasField(name: *const c_char) -> c_int;
    fn Host_LoadStrings();
    fn Hunk_AllocName(size: c_int, name: *const c_char) -> *mut c_void;
    // cmd.rs declares it with sizebuf.rs's SizeBufC, a type this feature
    // does not compile; the ABI is the same pointer.
    #[allow(clashing_extern_declarations)]
    fn SZ_Init(buf: *mut u8, data: *mut u8, length: c_int);
    fn EDICT_NUM(n: c_int) -> *mut u8;
    fn ED_ClearEdict(e: *mut u8);
    fn Mod_ForName(name: *const c_char, crash: c_int) -> *mut u8;
    fn SV_ParseWorldspawnSky();
    fn SV_ClearWorld();
    fn va(format: *const c_char, ...) -> *mut c_char;
    fn SV_ClientInventoryPage(clientnum: c_int) -> *mut u8;
    fn PR_SetEngineString(s: *const c_char) -> c_int;
    fn ED_LoadFromFile(data: *const c_char);
    fn SV_Physics();
    fn SV_CreateBaseline();
    fn SV_SendServerinfo(client: *mut u8);
    fn COM_SkipPath(pathname: *const c_char) -> *const c_char;
    fn FS_FileExists(filename: *const c_char, path_id: *mut c_uint) -> c_int;
    fn SV_GetLevelname() -> *const c_char;
    fn strncmp(s1: *const c_char, s2: *const c_char, n: usize) -> c_int;
}

#[inline(always)]
unsafe fn facts() -> &'static SvMainSpawnFacts {
    &*(&raw const SvMainSpawn_TargetFacts)
}

/// An int (or qboolean, or enum) member at `off` from `base`.
#[inline(always)]
unsafe fn int_at(base: *mut u8, off: usize) -> *mut c_int {
    base.add(off).cast::<c_int>()
}

/// A float member at `off` from `base`.
#[inline(always)]
unsafe fn float_at(base: *mut u8, off: usize) -> *mut c_float {
    base.add(off).cast::<c_float>()
}

/// A pointer member at `off` from `base`.
#[inline(always)]
unsafe fn ptr_at<T>(base: *mut u8, off: usize) -> *mut *mut T {
    base.add(off).cast::<*mut T>()
}

/// A char array member at `off` from `base`.
#[inline(always)]
unsafe fn chars_at(base: *mut u8, off: usize) -> *mut c_char {
    base.add(off).cast::<c_char>()
}

/// `sv.mapchecks.<member>`.
#[inline(always)]
unsafe fn mc(off: usize) -> c_int {
    *int_at(SvMainSpawn_Server(), off)
}

/// `svs.maxclients`, read where the C reads it.
#[inline(always)]
unsafe fn maxclients() -> c_int {
    *int_at(SvMainSpawn_Static(), facts().svs_maxclients)
}

/// `&svs.clients[i]`.
#[inline(always)]
unsafe fn client(i: c_int) -> *mut u8 {
    let f = facts();
    let clients = *ptr_at::<u8>(SvMainSpawn_Static(), f.svs_clients);
    clients.offset(i as isize * f.client_size as isize)
}

/// `sv.worldmodel`, read where the C reads it.
#[inline(always)]
unsafe fn worldmodel() -> *mut u8 {
    *ptr_at::<u8>(SvMainSpawn_Server(), facts().sv_worldmodel)
}

/// `current_loading_size += n; D_ShowLoadingSize ();`, client builds only.
#[inline(always)]
unsafe fn loading_step(n: c_int) {
    if facts().client_build != 0 {
        let p = SvMainSpawn_CurrentLoadingSize();
        *p = (*p).wrapping_add(n);
        SvMainSpawn_ShowLoadingSize();
    }
}

/// `total_loading_size = 0; loading_stage = 0;`, client builds only.
#[inline(always)]
unsafe fn loading_done() {
    *SvMainSpawn_TotalLoadingSize() = 0;
    *SvMainSpawn_LoadingStage() = 0;
}

// --- the map checklist -------------------------------------------------------

/// SV_MapCheckThresh: FAILED at or below zero, OK at or past the target,
/// PARTIAL between.
fn map_check_thresh(f: &SvMainSpawnFacts, current: c_int, target: c_int) -> c_int {
    if current <= 0 {
        return f.mapcheck_failed;
    }
    if current >= target {
        return f.mapcheck_ok;
    }
    f.mapcheck_partial
}

/// SV_MapCheckTitle: `in` squeezed onto one line into `out`, which it
/// always terminates.  `out` must hold at least one byte, as in the C (its
/// only caller passes 1024).
unsafe fn map_check_title(out: &mut [u8], mut src: *const c_char) {
    let outsize = out.len();
    let mut n = 0usize;

    if src.is_null() {
        src = c"".as_ptr();
    }
    while *src != 0 && n + 1 < outsize {
        let mut c = *src as u8;
        src = src.add(1);
        if c < 32 || c == 127 {
            if n == 0 || out[n - 1] == b' ' {
                continue; // no leading or doubled space
            }
            c = b' ';
        }
        out[n] = c;
        n += 1;
    }
    while n > 0 && out[n - 1] == b' ' {
        n -= 1;
    }
    out[n] = 0;
}

/// SV_PrintMapChecklist.  Each check's rationale is at its C original, in
/// engine/rust/sv_main/tests/sv_main_spawn_h2.c.
unsafe fn print_map_checklist() {
    const MIN_DM_SPAWN_POINTS: c_int = 5;
    const MIN_COOP_SPAWN_POINTS: c_int = 3;

    let f = facts();
    let sv = SvMainSpawn_Server();
    let safe = f.print_safe as c_uint;
    let mut buf = [0u8; 1024];

    // header
    CON_Printf(safe, c"\n".as_ptr());
    CON_Printf(safe, c"=====================================\n".as_ptr());
    CON_Printf(safe, c"\n".as_ptr());
    CON_Printf(
        safe,
        c"Map checklist (%s):\n\n".as_ptr(),
        COM_SkipPath(chars_at(sv, f.sv_modelname)),
    );

    // light data
    SV_PrintMapCheck(
        if !(*ptr_at::<u8>(worldmodel(), f.qmodel_lightdata)).is_null() {
            f.mapcheck_ok
        } else {
            f.mapcheck_failed
        },
        c"lightmap data".as_ptr(),
    );

    // vis data
    if (*ptr_at::<u8>(worldmodel(), f.qmodel_visdata)).is_null() {
        let mut storage = [0 as c_char; 4096];
        let pointfile = &mut storage[..f.max_ospath as usize];
        q_snprintf(
            pointfile.as_mut_ptr(),
            pointfile.len(),
            c"maps/%s.pts".as_ptr(),
            chars_at(sv, f.sv_name),
        );
        if FS_FileExists(pointfile.as_ptr(), core::ptr::null_mut()) != 0 {
            SV_PrintMapCheck(
                f.mapcheck_failed,
                c"vis data (unsealed map? %s exists)".as_ptr(),
                pointfile.as_ptr(),
            );
        } else {
            SV_PrintMapCheck(f.mapcheck_failed, c"vis data".as_ptr());
        }
    } else {
        SV_PrintMapCheck(f.mapcheck_ok, c"vis data".as_ptr());
    }

    // changelevel trigger
    if mc(f.mc_trigger_changelevel) == 0 {
        SV_PrintMapCheck(f.mapcheck_failed, c"trigger_changelevel".as_ptr());
    } else if mc(f.mc_trigger_changelevel) == 1 {
        if mc(f.mc_valid_changelevel) == mc(f.mc_trigger_changelevel) {
            if *chars_at(sv, f.mc_changespot) != 0 {
                SV_PrintMapCheck(
                    f.mapcheck_ok,
                    c"trigger_changelevel (%s, startspot \"%s\")".as_ptr(),
                    chars_at(sv, f.mc_changelevel),
                    chars_at(sv, f.mc_changespot),
                );
            } else {
                SV_PrintMapCheck(
                    f.mapcheck_ok,
                    c"trigger_changelevel (%s)".as_ptr(),
                    chars_at(sv, f.mc_changelevel),
                );
            }
        } else {
            SV_PrintMapCheck(
                f.mapcheck_partial,
                c"trigger_changelevel (missing \"map\" key)".as_ptr(),
            );
        }
    } else if mc(f.mc_valid_changelevel) == mc(f.mc_trigger_changelevel) {
        SV_PrintMapCheck(
            f.mapcheck_ok,
            c"trigger_changelevel (%d)".as_ptr(),
            mc(f.mc_trigger_changelevel),
        );
    } else {
        SV_PrintMapCheck(
            f.mapcheck_partial,
            c"trigger_changelevel (%d/%d missing \"map\" key)".as_ptr(),
            mc(f.mc_trigger_changelevel).wrapping_sub(mc(f.mc_valid_changelevel)),
            mc(f.mc_trigger_changelevel),
        );
    }

    // intermission camera: never a warning (see the C)
    if mc(f.mc_intermission) != 0 {
        SV_PrintMapCheck(
            f.mapcheck_ok,
            c"info_intermission (%d)".as_ptr(),
            mc(f.mc_intermission),
        );
    } else if mc(f.mc_dm_spawns) > 0 {
        SV_PrintMapCheck(
            f.mapcheck_na,
            c"info_intermission (none: deathmatch views the round end from info_player_start)"
                .as_ptr(),
        );
    } else {
        SV_PrintMapCheck(
            f.mapcheck_na,
            c"info_intermission (n/a: Hexen II shows the intermission in deathmatch only)"
                .as_ptr(),
        );
    }

    // skill levels
    let ents = f.mc_skill_ents;
    let int = core::mem::size_of::<c_int>();
    let skill_levels = mc(f.mc_skill_triggers) > 0
        || (mc(ents) != mc(ents + int) || mc(ents + int) != mc(ents + 2 * int));
    SV_PrintMapCheck(
        if skill_levels {
            f.mapcheck_ok
        } else {
            f.mapcheck_failed
        },
        c"skill spawnflags/triggers".as_ptr(),
    );

    // single player spawn point
    SV_PrintMapCheck(
        if mc(f.mc_sp_spawns) > 0 {
            f.mapcheck_ok
        } else {
            f.mapcheck_failed
        },
        c"info_player_start (%d)".as_ptr(),
        mc(f.mc_sp_spawns),
    );

    // coop spawn points
    SV_PrintMapCheck(
        map_check_thresh(f, mc(f.mc_coop_spawns), MIN_COOP_SPAWN_POINTS),
        c"info_player_coop (%d/%d+)".as_ptr(),
        mc(f.mc_coop_spawns),
        MIN_COOP_SPAWN_POINTS,
    );

    // deathmatch spawn points
    SV_PrintMapCheck(
        map_check_thresh(f, mc(f.mc_dm_spawns), MIN_DM_SPAWN_POINTS),
        c"info_player_deathmatch (%d/%d+)%s".as_ptr(),
        mc(f.mc_dm_spawns),
        MIN_DM_SPAWN_POINTS,
        if mc(f.mc_dm_spawns) != 0 {
            c"".as_ptr()
        } else {
            c" -- deathmatch on this map would hang the spawn search".as_ptr()
        },
    );

    // music track: worldspawn's MIDI and CD keys (see the C)
    let midi = chars_at(sv, f.sv_midi_name);
    let cd = *sv.add(f.sv_cd_track);
    if *midi != 0 && cd != 0 {
        SV_PrintMapCheck(
            f.mapcheck_ok,
            c"music (MIDI \"%s\", CD track %d)".as_ptr(),
            midi,
            cd as c_int,
        );
    } else if *midi != 0 {
        SV_PrintMapCheck(f.mapcheck_ok, c"music (MIDI \"%s\")".as_ptr(), midi);
    } else if cd != 0 {
        SV_PrintMapCheck(f.mapcheck_ok, c"music (CD track %d)".as_ptr(), cd as c_int);
    } else {
        SV_PrintMapCheck(
            f.mapcheck_failed,
            c"music (worldspawn \"MIDI\" or \"CD\" key)".as_ptr(),
        );
    }

    // map title: strings.txt's entry, or netname (see the C)
    map_check_title(&mut buf, SV_GetLevelname());
    if buf[0] != 0 {
        SV_PrintMapCheck(
            f.mapcheck_ok,
            c"map title (%s)".as_ptr(),
            buf.as_ptr().cast::<c_char>(),
        );
    } else {
        let edicts = *ptr_at::<u8>(sv, f.sv_edicts);
        let message = *float_at(edicts, f.edict_v + f.v_message);
        if cvt_c(message) > 0 {
            SV_PrintMapCheck(
                f.mapcheck_partial,
                c"map title (worldspawn \"message\" is %d, past the end of strings.txt)".as_ptr(),
                cvt_c(*float_at(edicts, f.edict_v + f.v_message)),
            );
        } else {
            SV_PrintMapCheck(
                f.mapcheck_failed,
                c"map title (worldspawn \"message\" index or \"netname\" string)".as_ptr(),
            );
        }
    }

    // Sky textures: the C's `#ifndef SERVERONLY` block (h2ded's sv_model.h
    // has no texture list).
    if f.client_build != 0 {
        let mut numskies: c_int = 0;
        let mut oddsized: c_int = 0;

        let mut i: c_int = 0;
        while i < *int_at(worldmodel(), f.qmodel_numtextures) {
            let tex = sky_texture(f, i);
            i += 1;
            if tex.is_null() {
                continue;
            }
            numskies += 1;
            if !sky_256x128(f, tex) {
                oddsized += 1;
            }
        }

        if numskies > 1 || oddsized > 0 {
            SV_PrintMapCheck(
                f.mapcheck_failed,
                c"compat: single %ssky texture (%d found)".as_ptr(),
                if oddsized > 0 {
                    c"256 x 128 ".as_ptr()
                } else {
                    c"".as_ptr()
                },
                numskies,
            );
            let mut i: c_int = 0;
            while i < *int_at(worldmodel(), f.qmodel_numtextures) {
                let tex = sky_texture(f, i);
                i += 1;
                if tex.is_null() {
                    continue;
                }
                let name = chars_at(tex, f.texture_name);
                if !sky_256x128(f, tex) {
                    CON_Printf(
                        safe,
                        c"      %s (%u x %u)\n".as_ptr(),
                        name,
                        *tex.add(f.texture_width).cast::<c_uint>(),
                        *tex.add(f.texture_height).cast::<c_uint>(),
                    );
                } else {
                    CON_Printf(safe, c"      %s\n".as_ptr(), name);
                }
            }
        }
    }

    // footer
    let warnings = mc(f.mc_numwarnings);
    CON_Printf(safe, c"\n".as_ptr());
    if warnings != 0 {
        CON_Printf(
            safe,
            c"%d item%s to look at.\n".as_ptr(),
            warnings,
            if warnings == 1 {
                c"".as_ptr()
            } else {
                c"s".as_ptr()
            },
        );
    } else {
        CON_Printf(safe, c"Nothing to look at.\n".as_ptr());
    }
    CON_Printf(safe, c"=====================================\n".as_ptr());
    CON_Printf(safe, c"\n".as_ptr());
}

/// `sv.worldmodel->textures[i]` when it is a texture whose name starts
/// "sky", else NULL: the C's `if (!tex || strncmp (tex->name, "sky", 3) != 0)
/// continue;`.
unsafe fn sky_texture(f: &SvMainSpawnFacts, i: c_int) -> *mut u8 {
    let textures = *ptr_at::<*mut u8>(worldmodel(), f.qmodel_textures);
    let tex = *textures.offset(i as isize);
    if tex.is_null() || strncmp(chars_at(tex, f.texture_name), c"sky".as_ptr(), 3) != 0 {
        return core::ptr::null_mut();
    }
    tex
}

/// `tex->width == 256 && tex->height == 128`.
unsafe fn sky_256x128(f: &SvMainSpawnFacts, tex: *mut u8) -> bool {
    *tex.add(f.texture_width).cast::<c_uint>() == 256
        && *tex.add(f.texture_height).cast::<c_uint>() == 128
}

// --- the spawn ---------------------------------------------------------------

/// SV_SpawnServer: the start of every level.
#[no_mangle]
pub unsafe extern "C" fn SV_SpawnServer(server: *const c_char, startspot: *const c_char) {
    let f = facts();
    let sv = SvMainSpawn_Server();
    let svs = SvMainSpawn_Static();
    let normal = f.print_normal as c_uint;
    let devel = f.print_devel as c_uint;

    // let's not have any servers with no name
    if *SvMainSpawn_HostnameString() == 0 {
        Cvar_Set(c"hostname".as_ptr(), c"UNNAMED".as_ptr());
    }
    if f.client_build != 0 {
        *SvMainSpawn_CenterTimeOff() = 0.0;
    }

    CON_Printf(devel, c"%s: %s\n".as_ptr(), c"SV_SpawnServer".as_ptr(), server);

    // A Host_Error out of SV_Physics skips its reset; a map loaded after it
    // would stamp every entity with that walk's value instead of 0 (#138).
    *SvMainSpawn_PhysicsWalk() = 0;

    if *int_at(svs, f.svs_changelevel_issued) != 0 {
        SaveGamestate(1);
    }

    // tell all connected clients that we are going to a new level
    if *int_at(sv, f.sv_active) != 0 {
        SV_SendReconnect();
    }

    // GL builds: tell D_FlushCaches whether to flush the textures
    if f.glquake_build != 0 {
        *SvMainSpawn_FlushTextures() =
            (q_strncasecmp(server, chars_at(sv, f.sv_name), 64) != 0) as c_int;
    }

    // make cvars consistant
    if SvMainSpawn_CoopInteger() != 0 {
        Cvar_Set(c"deathmatch".as_ptr(), c"0".as_ptr());
    }

    let current_skill = SvMainSpawn_CurrentSkill();
    *current_skill = SvMainSpawn_SkillInteger();
    // (spawn_mutants.sh EQUIVALENT: `< 1` here only adds 0, which the
    // branch would store as 0 again.)
    if *current_skill < 0 {
        *current_skill = 0;
    }
    if *current_skill > 4 {
        *current_skill = 4;
    }

    Cvar_SetValue(c"skill".as_ptr(), *current_skill as c_float);

    // set up the new server
    Host_ClearMemory();

    q_strlcpy(chars_at(sv, f.sv_name), server, f.sv_name_size);
    if !startspot.is_null() {
        q_strlcpy(chars_at(sv, f.sv_startspot), startspot, f.sv_startspot_size);
    }

    // Set before ED_LoadFromFile runs, which is where the census happens.
    if SvMainSpawn_MapChecksInteger() != 0 || SvMainSpawn_DeveloperInteger() != 0 {
        *int_at(sv, f.mc_active) = 1;
    }

    // load progs to get entity field count
    if f.client_build != 0 {
        *SvMainSpawn_TotalLoadingSize() = 100;
        *SvMainSpawn_CurrentLoadingSize() = 0;
        *SvMainSpawn_LoadingStage() = 1;
        SvMainSpawn_ShowLoadingSize();
    }
    PR_LoadProgs();

    // Auto-detect protocol: extended builtins (107-112) need protocol 21
    // (UH2_114), and a .alpha field needs PROTOCOL_HEXENWAIL_1.
    if SvMainSpawn_ProtocolUserset() == 0 {
        let mut needs_114 = false;
        let mut fi: c_int = 0;
        while fi < *int_at(SvMainSpawn_Progs(), f.progs_numfunctions) {
            let first_statement = *int_at(
                SvMainSpawn_Functions(),
                fi as usize * f.function_size + f.function_first_statement,
            );
            let bi = first_statement.wrapping_neg();
            // (spawn_mutants.sh EQUIVALENT, both kept because the C has
            // them: `first_statement < 0` is implied by 107 <= bi <= 112,
            // INT_MIN wrapping to itself; and the break only stops the scan
            // early, since nothing after the loop reads fi and needs_114
            // never goes back to false.)
            if first_statement < 0 && bi >= 107 && bi <= 112 {
                needs_114 = true;
                break;
            }
            fi += 1;
        }
        if needs_114 {
            *SvMainSpawn_Protocol() = f.protocol_uh2_114;
            CON_Printf(
                normal,
                c"Progs uses extended builtins \xe2\x80\x94 auto-upgraded to protocol %d\n".as_ptr(),
                *SvMainSpawn_Protocol(),
            );
        }

        if PR_HasField(c"alpha".as_ptr()) != 0 {
            *SvMainSpawn_Protocol() = f.protocol_hexenwail_1;
            CON_Printf(
                normal,
                c"Progs uses .alpha \xe2\x80\x94 auto-upgraded to protocol %d\n".as_ptr(),
                *SvMainSpawn_Protocol(),
            );
        }
    }

    loading_step(10);
    Host_LoadStrings();
    loading_step(5);

    // allocate server memory (Host_ClearMemory cleared the whole of sv)
    *ptr_at::<c_void>(sv, f.sv_states) = Hunk_AllocName(
        (maxclients() as usize).wrapping_mul(f.client_state2_size) as c_int,
        c"states".as_ptr(),
    );
    // Latch the ceiling for this map before anything can allocate against it.
    let sv_max_edicts = SvMainSpawn_MaxEdicts();
    *sv_max_edicts = cvt_c(SvMainSpawn_MaxEdictsValue());
    if *sv_max_edicts < f.min_edicts {
        *sv_max_edicts = f.min_edicts;
    }
    if *sv_max_edicts > f.max_edicts {
        *sv_max_edicts = f.max_edicts;
    }

    *ptr_at::<c_void>(sv, f.sv_edicts) = Hunk_AllocName(
        (*sv_max_edicts).wrapping_mul(SvMainSpawn_EdictSize()),
        c"edicts".as_ptr(),
    );

    SZ_Init(
        sv.add(f.sv_datagram),
        sv.add(f.sv_datagram_buf),
        f.sv_datagram_buf_size as c_int,
    );
    *ptr_at::<c_char>(sv.add(f.sv_datagram), f.sizebuf_name) =
        c"sv.datagram".as_ptr().cast_mut();
    SZ_Init(
        sv.add(f.sv_reliable_datagram),
        sv.add(f.sv_reliable_datagram_buf),
        f.sv_reliable_datagram_buf_size as c_int,
    );
    *ptr_at::<c_char>(sv.add(f.sv_reliable_datagram), f.sizebuf_name) =
        c"sv.reliable_datagram".as_ptr().cast_mut();
    // Start the signon list over: buffer 0 is the one being filled.
    *int_at(sv, f.sv_num_signon_buffers) = 0;
    SZ_Init(sv.add(f.sv_signon), sv.add(f.sv_signon_bufs), f.max_signon_size);
    *ptr_at::<c_char>(sv.add(f.sv_signon), f.sizebuf_name) = c"sv.signon".as_ptr().cast_mut();

    // leave slots at start for clients only
    *int_at(sv, f.sv_num_edicts) = maxclients()
        .wrapping_add(1)
        .wrapping_add(SvMainSpawn_MaxTempEdictsInteger());
    let mut i: c_int = 0;
    while i < maxclients() {
        let ent = EDICT_NUM(i + 1);
        *ptr_at::<u8>(client(i), f.client_edict) = ent;
        *int_at(client(i), f.client_send_all_v) = 1;
        i += 1;
    }

    let mut i: c_int = 0;
    while i < SvMainSpawn_MaxTempEdictsInteger() {
        let ent = EDICT_NUM(i.wrapping_add(maxclients()).wrapping_add(1));
        ED_ClearEdict(ent);

        *int_at(ent, f.edict_free) = 1;
        *float_at(ent, f.edict_freetime) = -999.0;
        i += 1;
    }

    *int_at(sv, f.sv_state) = f.ss_loading;
    *int_at(sv, f.sv_paused) = 0;

    *sv.add(f.sv_time).cast::<c_double>() = 1.0;

    q_strlcpy(chars_at(sv, f.sv_name), server, f.sv_name_size);
    q_snprintf(
        chars_at(sv, f.sv_modelname),
        f.sv_modelname_size,
        c"maps/%s.bsp".as_ptr(),
        server,
    );

    *ptr_at::<u8>(sv, f.sv_worldmodel) = Mod_ForName(chars_at(sv, f.sv_modelname), 0);
    if worldmodel().is_null() {
        CON_Printf(
            normal,
            c"Couldn't spawn server %s\n".as_ptr(),
            chars_at(sv, f.sv_modelname),
        );
        *int_at(sv, f.sv_active) = 0;
        if f.client_build != 0 {
            loading_done();
            // Every caller reaches here through SCR_BeginLoadingPlaque;
            // returning without ending it left the window painting nothing
            // (uhexen2-h9zy, the C's note).
            SvMainSpawn_EndLoadingPlaque();
        }
        return;
    }
    let models = ptr_at::<u8>(sv, f.sv_models);
    *models.add(1) = worldmodel();

    SV_ParseWorldspawnSky();
    q_strlcpy(
        chars_at(sv, f.sv_mod_name),
        SvMainSpawn_GamedirNoPath(),
        f.sv_mod_name_size,
    );

    // clear world interaction links
    SV_ClearWorld();

    let dummy = SvMainSpawn_PrecacheDummy();
    let model_precache = ptr_at::<c_char>(sv, f.sv_model_precache);
    *ptr_at::<c_char>(sv, f.sv_sound_precache) = dummy;
    *model_precache = dummy;
    *model_precache.add(1) = chars_at(sv, f.sv_modelname);
    let max_models = f.max_models as usize;
    let mut i: c_int = 1;
    while i < *int_at(worldmodel(), f.qmodel_numsubmodels) {
        // The C indexes past localmodels[] and both lists here (above).
        let slot = 1 + i as usize;
        assert!(slot < max_models);
        let name = SvMainSpawn_LocalModels().add(i as usize * f.localmodel_size);
        *model_precache.add(slot) = name;
        *models.add(slot) = Mod_ForName(name, 0);
        i += 1;
    }

    *int_at(sv, f.sv_num_ex_items) = 0;
    if (*ptr_at::<u8>(sv, f.sv_ex_items)).is_null() {
        *ptr_at::<c_void>(sv, f.sv_ex_items) = Hunk_AllocName(
            (f.max_items_ex as usize).wrapping_mul(f.ex_item_size) as c_int,
            c"ex_items".as_ptr(),
        );
        let mut i: c_int = 0;
        while i < 15 {
            *int_at(sv, f.sv_num_ex_items) += 1;
            let item = (*ptr_at::<u8>(sv, f.sv_ex_items)).add(i as usize * f.ex_item_size);
            *int_at(item, f.ex_item_id) = i + 1;
            q_strlcpy(
                chars_at(item, f.ex_item_icon),
                va(c"gfx/arti%02d.lmp".as_ptr(), i),
                f.max_qpath as usize,
            );
            i += 1;
        }
    }

    if (*ptr_at::<u8>(sv, f.sv_ex_inventory_pages)).is_null() {
        *ptr_at::<c_void>(sv, f.sv_ex_inventory_pages) = Hunk_AllocName(
            (maxclients().wrapping_mul(f.max_inventory_ex_pages) as usize)
                .wrapping_mul(f.ex_inventory_page_size) as c_int,
            c"ex_pages".as_ptr(),
        );
    }

    // Re-point every client that is still connected: Host_ClearMemory freed
    // the previous array and clients survive a changelevel (uhexen2-rutp).
    let mut i: c_int = 0;
    while i < maxclients() {
        if *int_at(client(i), f.client_active) != 0 {
            *ptr_at::<u8>(client(i), f.client_ex_inventory) = SV_ClientInventoryPage(i);
        }
        i += 1;
    }

    // load the rest of the entities
    let ent = EDICT_NUM(0);
    core::ptr::write_bytes(
        ent.add(f.edict_v),
        0,
        (*int_at(SvMainSpawn_Progs(), f.progs_entityfields)).wrapping_mul(4) as usize,
    );
    *int_at(ent, f.edict_free) = 0;
    *int_at(ent, f.edict_v + f.v_model) =
        PR_SetEngineString(chars_at(worldmodel(), f.qmodel_name));
    *float_at(ent, f.edict_v + f.v_modelindex) = 1.0; // world model
    *float_at(ent, f.edict_v + f.v_solid) = f.solid_bsp as c_float;
    *float_at(ent, f.edict_v + f.v_movetype) = f.movetype_push as c_float;

    let globals = SvMainSpawn_Globals();
    if SvMainSpawn_CoopInteger() != 0 {
        **ptr_at::<c_float>(globals, f.g_coop) = SvMainSpawn_CoopValue();
    } else {
        **ptr_at::<c_float>(globals, f.g_deathmatch) = SvMainSpawn_DeathmatchValue();
    }
    if !(*ptr_at::<c_float>(globals, f.g_randomclass)).is_null() {
        **ptr_at::<c_float>(globals, f.g_randomclass) = SvMainSpawn_RandomclassValue();
    }
    **ptr_at::<c_int>(globals, f.g_mapname) = PR_SetEngineString(chars_at(sv, f.sv_name));
    **ptr_at::<c_int>(globals, f.g_startspot) = PR_SetEngineString(chars_at(sv, f.sv_startspot));
    // serverflags are for cross level information (sigils)
    **ptr_at::<c_float>(globals, f.g_serverflags) = *int_at(svs, f.svs_serverflags) as c_float;

    loading_step(5);
    ED_LoadFromFile(*ptr_at::<c_char>(worldmodel(), f.qmodel_entities));

    *int_at(sv, f.sv_active) = 1;

    // all setup is completed, any further precache statements are errors
    *int_at(sv, f.sv_state) = f.ss_active;

    // run two frames to allow everything to settle
    *SvMainSpawn_HostFrametime() = 0.1;
    SV_Physics();
    SV_Physics();

    // create a baseline for more efficient communications
    SV_CreateBaseline();

    // How much of the signon budget this map actually used (uhexen2-z5wt).
    let signon_size = sv.add(f.sv_signon_size).cast::<c_int>();
    let mut total: c_int = 0;
    let mut i: c_int = 0;
    while i < *int_at(sv, f.sv_num_signon_buffers) {
        // A raw read, as the C's: a count past MAX_SIGNON_BUFFERS (which
        // SV_ReserveSignonSpace never leaves) reads on into signon_bufs,
        // inside sv either way.
        total = total.wrapping_add(*signon_size.offset(i as isize));
        i += 1;
    }
    total = total.wrapping_add(*int_at(sv.add(f.sv_signon), f.sizebuf_cursize));
    CON_Printf(
        devel,
        c"Signon: %d bytes in %d/%d buffers\n".as_ptr(),
        total,
        (*int_at(sv, f.sv_num_signon_buffers)).wrapping_add(1),
        f.max_signon_buffers,
    );

    // send serverinfo to all connected clients
    let host_client = SvMainSpawn_HostClient();
    let mut i: c_int = 0;
    *host_client = *ptr_at::<u8>(svs, f.svs_clients);
    while i < maxclients() {
        if *int_at(*host_client, f.client_active) != 0 {
            SV_SendServerinfo(*host_client);
        }
        i += 1;
        *host_client = (*host_client).add(f.client_size);
    }

    *int_at(svs, f.svs_changelevel_issued) = 0; // now safe to issue another

    CON_Printf(devel, c"Server spawned.\n".as_ptr());

    if *int_at(sv, f.mc_active) != 0 {
        print_map_checklist();
    }

    if f.client_build != 0 {
        loading_done();
    }
}

// --- test hooks for the differential harness ---------------------------------

/// SV_MapCheckThresh, for the harness.
#[no_mangle]
pub unsafe extern "C" fn SvMainSpawn_TestMapCheckThresh(current: c_int, target: c_int) -> c_int {
    map_check_thresh(facts(), current, target)
}

/// SV_MapCheckTitle, for the harness; `outsize` must be at least 1.
#[no_mangle]
pub unsafe extern "C" fn SvMainSpawn_TestMapCheckTitle(
    out: *mut c_char,
    outsize: usize,
    src: *const c_char,
) {
    map_check_title(core::slice::from_raw_parts_mut(out.cast::<u8>(), outsize), src);
}

/// SV_PrintMapChecklist, for the harness.
#[no_mangle]
pub unsafe extern "C" fn SvMainSpawn_TestPrintMapChecklist() {
    print_map_checklist();
}
