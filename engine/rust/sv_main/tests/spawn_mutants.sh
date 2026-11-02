#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The mutation run for engine/rust/src/sv_main_spawn.rs (E2 / slice E of
# #469): applies each mutant below to the file in turn, rebuilds the archive
# with sv_main_spawn, runs the differential harness (run_spawn_harness.sh,
# all three configs, a shorter case count, no coverage pass), and requires
# the harness to FAIL.  The file is restored on exit.
#
#   bash engine/rust/sv_main/tests/spawn_mutants.sh            # every mutant
#   bash engine/rust/sv_main/tests/spawn_mutants.sh 'name re'  # matching ones
#
# A mutant that does not compile, or whose substitution changes nothing, is
# an error (exit 2), not a kill.  The mutants listed as EQUIVALENT are
# expected to survive; each has its proof at the site it mutates, and the run
# fails if one of them is killed (the proof is then wrong) or if anything
# else survives.  Not part of check-rust-sv-main.sh (it rewrites a tracked
# source); run it when sv_main_spawn.rs or the harness changes.  Needs perl,
# cargo, cc -- run inside `nix develop`.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(cd "$here/../.." && pwd)"
src="$crate/src/sv_main_spawn.rs"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
orig="$work/sv_main_spawn.rs.orig"
cp "$src" "$orig"
trap 'cp "$orig" "$src"' EXIT
filter="${1:-}"

kill_cmd() {
	local out
	if ! out=$(cargo build --release --offline --manifest-path "$crate/Cargo.toml" \
		--target-dir "$work/target" --features sv_main_spawn 2>&1); then
		echo "BROKEN MUTANT (does not compile)" >&2
		echo "$out" | grep -E '^(error|warning: unused)' -A5 >&2 || true
		return 2
	fi
	printf '%s\n' "$out" >"$work/cargo.log"
	# a build that left the port out would be "killed" by a link error
	# (captured, not piped into grep -q: pipefail and SIGPIPE)
	local syms
	syms=$(nm "$work/target/release/libengine_rs.a" 2>/dev/null || true)
	if ! grep -q ' T SV_SpawnServer$' <<<"$syms"; then
		echo "BROKEN BUILD: the archive has no SV_SpawnServer" >&2
		cat "$work/cargo.log" >&2
		return 2
	fi
	VARIANT=h2 RUST_LIB="$work/target/release/libengine_rs.a" OUT="$work/h" \
		HARNESS_COVERAGE=0 HARNESS_SPAWN="${MUTANT_SPAWN:-1500}" \
		bash "$here/run_spawn_harness.sh" >"$work/last.log" 2>&1
}

# name|perl substitution, applied once to sv_main_spawn.rs.  \Q..\E quotes
# the text being replaced.
# shellcheck disable=SC2016  # perl code, not shell
mutants=(
	# SV_SpawnServer: the prologue
	'hostname test inverted|s~\Q*SvMainSpawn_HostnameString() == 0\E~*SvMainSpawn_HostnameString() != 0~'
	'hostname default text|s~\Qc"UNNAMED"\E~c"UNNAMEd"~'
	'centertime arm on the wrong build|s~\Qif f.client_build != 0 {\E\n(\s*)\Q*SvMainSpawn_CenterTimeOff() = 0.0;\E~if f.client_build == 0 {\n${1}*SvMainSpawn_CenterTimeOff() = 0.0;~'
	'centertime reset to 1|s~\Q*SvMainSpawn_CenterTimeOff() = 0.0;\E~*SvMainSpawn_CenterTimeOff() = 1.0;~'
	'devel line names another function|s~\Qc"SV_SpawnServer".as_ptr()\E~c"SV_SpawnServe".as_ptr()~'
	'physics walk reset to 1|s~\Q*SvMainSpawn_PhysicsWalk() = 0;\E~*SvMainSpawn_PhysicsWalk() = 1;~'
	'changelevel_issued test inverted|s~\Q*int_at(svs, f.svs_changelevel_issued) != 0 {\E~*int_at(svs, f.svs_changelevel_issued) == 0 {~'
	'SaveGamestate(false)|s~\QSaveGamestate(1);\E~SaveGamestate(0);~'
	'reconnect test inverted|s~\Q*int_at(sv, f.sv_active) != 0 {\E\n(\s*)\QSV_SendReconnect\E~*int_at(sv, f.sv_active) == 0 {\n${1}SV_SendReconnect~'
	'flush_textures on the wrong build|s~\Qif f.glquake_build != 0 {\E~if f.glquake_build == 0 {~'
	'flush_textures compares 63 chars|s~\Qchars_at(sv, f.sv_name), 64)\E~chars_at(sv, f.sv_name), 63)~'
	'flush_textures inverted|s~\Q, 64) != 0) as c_int\E~, 64) == 0) as c_int~'
	'coop test inverted (deathmatch)|s~\Qif SvMainSpawn_CoopInteger() != 0 {\E\n(\s*)\QCvar_Set(c"deathmatch"\E~if SvMainSpawn_CoopInteger() == 0 {\n${1}Cvar_Set(c"deathmatch"~'
	'deathmatch forced to 1|s~\Qc"deathmatch".as_ptr(), c"0".as_ptr()\E~c"deathmatch".as_ptr(), c"1".as_ptr()~'
	'EQUIVALENT skill floor test < 1|s~\Qif *current_skill < 0 {\E~if *current_skill < 1 {~'
	'skill floor test < -1|s~\Qif *current_skill < 0 {\E~if *current_skill < -1 {~'
	'skill floor stores 1|s~\Q*current_skill = 0;\E~*current_skill = 1;~'
	'skill ceiling 5|s~\Qif *current_skill > 4 {\E~if *current_skill > 5 {~'
	'skill set unclamped|s~\QCvar_SetValue(c"skill".as_ptr(), *current_skill as c_float);\E~Cvar_SetValue(c"skill".as_ptr(), SvMainSpawn_SkillInteger() as c_float);~'
	'Host_ClearMemory dropped|s~\Q    Host_ClearMemory();\E~~'
	'startspot copied when NULL|s~\Qif !startspot.is_null() {\E~if true {~'
	'startspot never copied|s~\Qif !startspot.is_null() {\E~if false {~'
	'map_checks or developer -> and|s~\QSvMainSpawn_MapChecksInteger() != 0 || SvMainSpawn_DeveloperInteger() != 0\E~SvMainSpawn_MapChecksInteger() != 0 && SvMainSpawn_DeveloperInteger() != 0~'
	'mapchecks.active not set|s~\Q*int_at(sv, f.mc_active) = 1;\E~*int_at(sv, f.mc_active) = 0;~'
	'loading total 99|s~\Q*SvMainSpawn_TotalLoadingSize() = 100;\E~*SvMainSpawn_TotalLoadingSize() = 99;~'
	'loading start 1|s~\Q*SvMainSpawn_CurrentLoadingSize() = 0;\E~*SvMainSpawn_CurrentLoadingSize() = 1;~'
	'loading stage 2|s~\Q*SvMainSpawn_LoadingStage() = 1;\E~*SvMainSpawn_LoadingStage() = 2;~'
	'first ShowLoadingSize dropped|s~\Q*SvMainSpawn_LoadingStage() = 1;\E\n(\s*)\QSvMainSpawn_ShowLoadingSize();\E~*SvMainSpawn_LoadingStage() = 1;~'
	'loading arm on the wrong build|s~\Q    if f.client_build != 0 {\E\n(\s*)\Q*SvMainSpawn_TotalLoadingSize() = 100;\E~    if f.client_build == 0 {\n${1}*SvMainSpawn_TotalLoadingSize() = 100;~'
	'PR_LoadProgs dropped|s~\Q    PR_LoadProgs();\E~~'
	# the protocol sniff
	'userset test inverted|s~\Qif SvMainSpawn_ProtocolUserset() == 0 {\E~if SvMainSpawn_ProtocolUserset() != 0 {~'
	'builtin range from 106|s~\Q&& bi >= 107 &&\E~\&\& bi >= 106 \&\&~'
	'builtin range from 108|s~\Q&& bi >= 107 &&\E~\&\& bi >= 108 \&\&~'
	'builtin range to 113|s~\Q&& bi <= 112 {\E~\&\& bi <= 113 {~'
	'builtin range to 111|s~\Q&& bi <= 112 {\E~\&\& bi <= 111 {~'
	'EQUIVALENT builtin sign test dropped|s~\Qfirst_statement < 0 && bi\E~true \&\& bi~'
	'EQUIVALENT sniff loop does not break|s~\Qneeds_114 = true;\E\n(\s*)\Qbreak;\E~needs_114 = true;~'
	'sniff skips function 0|s~\Qlet mut fi: c_int = 0;\E~let mut fi: c_int = 1;~'
	'sniff reads first_statement of the next function|s~\Qfi as usize * f.function_size + f.function_first_statement\E~(fi as usize + 1) * f.function_size + f.function_first_statement~'
	'114 upgrade picks protocol 100|s~\Q*SvMainSpawn_Protocol() = f.protocol_uh2_114;\E~*SvMainSpawn_Protocol() = f.protocol_hexenwail_1;~'
	'114 line loses its em dash|s~\Qbuiltins \xe2\x80\x94 auto\E~builtins - auto~'
	'alpha field name|s~\QPR_HasField(c"alpha"\E~PR_HasField(c"Alpha"~'
	'alpha test inverted|s~\QPR_HasField(c"alpha".as_ptr()) != 0\E~PR_HasField(c"alpha".as_ptr()) == 0~'
	'alpha upgrade picks protocol 21|s~\Q*SvMainSpawn_Protocol() = f.protocol_hexenwail_1;\E~*SvMainSpawn_Protocol() = f.protocol_uh2_114;~'
	'alpha line loses its em dash|s~\Q.alpha \xe2\x80\x94 auto\E~.alpha -- auto~'
	'loading +10 is 11|s~\Qloading_step(10);\E~loading_step(11);~'
	'loading +5 after strings is 6|s~\QHost_LoadStrings();\E\n(\s*)\Qloading_step(5);\E~Host_LoadStrings();\n${1}loading_step(6);~'
	'loading step on the wrong build|s~\Qif facts().client_build != 0 {\E~if facts().client_build == 0 {~'
	'Host_LoadStrings dropped|s~\Q    Host_LoadStrings();\E~~'
	# the allocations
	'states for one client too many|s~\Q(maxclients() as usize).wrapping_mul(f.client_state2_size)\E~(maxclients() as usize + 1).wrapping_mul(f.client_state2_size)~'
	'max_edicts truncation saturates|s~\Q*sv_max_edicts = cvt_c(SvMainSpawn_MaxEdictsValue());\E~*sv_max_edicts = SvMainSpawn_MaxEdictsValue() as c_int;~'
	'max_edicts floor +1|s~\Q*sv_max_edicts = f.min_edicts;\E~*sv_max_edicts = f.min_edicts + 1;~'
	'max_edicts floor test <=|s~\Qif *sv_max_edicts < f.min_edicts {\E~if *sv_max_edicts < f.min_edicts - 1 {~'
	'max_edicts ceiling -1|s~\Q*sv_max_edicts = f.max_edicts;\E~*sv_max_edicts = f.max_edicts - 1;~'
	'max_edicts ceiling test|s~\Qif *sv_max_edicts > f.max_edicts {\E~if *sv_max_edicts > f.max_edicts + 1 {~'
	'edicts allocation one edict short|s~\Q(*sv_max_edicts).wrapping_mul(SvMainSpawn_EdictSize())\E~(*sv_max_edicts - 1).wrapping_mul(SvMainSpawn_EdictSize())~'
	'datagram one byte short|s~\Qf.sv_datagram_buf_size as c_int,\E~f.sv_datagram_buf_size as c_int - 1,~'
	'datagram name|s~\Qc"sv.datagram".as_ptr()\E~c"sv.datagrm".as_ptr()~'
	'reliable datagram on the datagram buffer|s~\Qsv.add(f.sv_reliable_datagram_buf),\E~sv.add(f.sv_datagram_buf),~'
	'reliable datagram name|s~\Qc"sv.reliable_datagram".as_ptr()\E~c"sv.reliable".as_ptr()~'
	'signon count left at 1|s~\Q*int_at(sv, f.sv_num_signon_buffers) = 0;\E~*int_at(sv, f.sv_num_signon_buffers) = 1;~'
	'signon buffer size -1|s~\Qsv.add(f.sv_signon_bufs), f.max_signon_size)\E~sv.add(f.sv_signon_bufs), f.max_signon_size - 1)~'
	'signon name|s~\Qc"sv.signon".as_ptr()\E~c"sv.signo".as_ptr()~'
	# the client and temp edicts
	'num_edicts one more|s~\Q.wrapping_add(1)\E\n(\s*)\Q.wrapping_add(SvMainSpawn_MaxTempEdictsInteger());\E~.wrapping_add(2)\n${1}.wrapping_add(SvMainSpawn_MaxTempEdictsInteger());~'
	'client edicts start at 2|s~\Qlet ent = EDICT_NUM(i + 1);\E~let ent = EDICT_NUM(i + 2);~'
	'send_all_v not set|s~\Q*int_at(client(i), f.client_send_all_v) = 1;\E~*int_at(client(i), f.client_send_all_v) = 0;~'
	'client loop one short|s~\Qlet ent = EDICT_NUM(i + 1);\E~if i + 1 == maxclients() { break; }\n        let ent = EDICT_NUM(i + 1);~'
	'temp edicts start one early|s~\QEDICT_NUM(i.wrapping_add(maxclients()).wrapping_add(1))\E~EDICT_NUM(i.wrapping_add(maxclients()))~'
	'ED_ClearEdict dropped|s~\QED_ClearEdict(ent);\E~~'
	'temp edict not freed|s~\Q*int_at(ent, f.edict_free) = 1;\E~*int_at(ent, f.edict_free) = 0;~'
	'freetime -998|s~\Q= -999.0;\E~= -998.0;~'
	'temp loop runs one long|s~\Qwhile i < SvMainSpawn_MaxTempEdictsInteger() {\E~while i <= SvMainSpawn_MaxTempEdictsInteger() {~'
	'state set active while loading|s~\Q*int_at(sv, f.sv_state) = f.ss_loading;\E~*int_at(sv, f.sv_state) = f.ss_active;~'
	'paused left set|s~\Q*int_at(sv, f.sv_paused) = 0;\E~*int_at(sv, f.sv_paused) = 1;~'
	'time 2.0|s~\Q.cast::<c_double>() = 1.0;\E~.cast::<c_double>() = 2.0;~'
	'second name copy dropped|s~\Q    q_strlcpy(chars_at(sv, f.sv_name), server, f.sv_name_size);\E\n(\s*)\Qq_snprintf(\E~    q_snprintf(~'
	'model name format|s~\Qc"maps/%s.bsp".as_ptr()\E~c"maps/%s.bs".as_ptr()~'
	'world loaded with crash true|s~\QMod_ForName(chars_at(sv, f.sv_modelname), 0)\E~Mod_ForName(chars_at(sv, f.sv_modelname), 1)~'
	# the failure path
	'failure leaves sv active|s~\Q*int_at(sv, f.sv_active) = 0;\E~*int_at(sv, f.sv_active) = 1;~'
	'failure does not reset the loading counters|s~\Q            loading_done();\E\n~\n~'
	'failure does not end the plaque|s~\Q    SvMainSpawn_EndLoadingPlaque();\E~~'
	'failure arm on the wrong build|s~\Q        if f.client_build != 0 {\E\n(\s*)\Qloading_done();\E~        if f.client_build == 0 {\n${1}loading_done();~'
	'failure carries on|s~\Q        return;\E\n(\s*)\Q}\E\n(\s*)\Qlet models\E~    }\n${2}let models~'
	'failure line text|s~\QCouldn'"'"'t spawn server %s\E~Could not spawn server %s~'
	# the world and the precache lists
	'world into models[2]|s~\Q*models.add(1) = worldmodel();\E~*models.add(2) = worldmodel();~'
	'sky parse dropped|s~\Q    SV_ParseWorldspawnSky();\E~~'
	'mod_name not copied|s~\QSvMainSpawn_GamedirNoPath(),\E~c"".as_ptr(),~'
	'SV_ClearWorld dropped|s~\Q    SV_ClearWorld();\E~~'
	'sound precache 0 is the model name|s~\Q*ptr_at::<c_char>(sv, f.sv_sound_precache) = dummy;\E~*ptr_at::<c_char>(sv, f.sv_sound_precache) = chars_at(sv, f.sv_modelname);~'
	'model precache 0 is a fresh zero buffer|s~\Q*model_precache = dummy;\E~*model_precache = SvMainSpawn_PrecacheDummy().add(1);~'
	'model precache 1 is the map name|s~\Q*model_precache.add(1) = chars_at(sv, f.sv_modelname);\E~*model_precache.add(1) = chars_at(sv, f.sv_name);~'
	'submodels start at 2|s~\Qlet mut i: c_int = 1;\E~let mut i: c_int = 2;~'
	'submodels run one long|s~\Qwhile i < *int_at(worldmodel(), f.qmodel_numsubmodels) {\E~while i <= *int_at(worldmodel(), f.qmodel_numsubmodels) {~'
	'submodel slot index check dropped|s~\Qassert!(slot < max_models);\E~~'
	'submodel precache name is the next one|s~\Qadd(i as usize * f.localmodel_size)\E~add((i as usize + 1) * f.localmodel_size)~'
	'submodel loaded with crash true|s~\Q*models.add(slot) = Mod_ForName(name, 0);\E~*models.add(slot) = Mod_ForName(name, 1);~'
	# the extended inventory
	'num_ex_items starts at 1|s~\Q*int_at(sv, f.sv_num_ex_items) = 0;\E~*int_at(sv, f.sv_num_ex_items) = 1;~'
	'ex_items always reallocated|s~\Qif (*ptr_at::<u8>(sv, f.sv_ex_items)).is_null() {\E~if true {~'
	'ex_items allocation one item short|s~\Q(f.max_items_ex as usize).wrapping_mul(f.ex_item_size)\E~(f.max_items_ex as usize - 1).wrapping_mul(f.ex_item_size)~'
	'14 artifact icons|s~\Qwhile i < 15 {\E~while i < 14 {~'
	'artifact ids from 0|s~\Q*int_at(item, f.ex_item_id) = i + 1;\E~*int_at(item, f.ex_item_id) = i;~'
	'artifact icon format|s~\Qc"gfx/arti%02d.lmp".as_ptr(), i)\E~c"gfx/arti%d.lmp".as_ptr(), i)~'
	'artifact icon from the next index|s~\Qc"gfx/arti%02d.lmp".as_ptr(), i)\E~c"gfx/arti%02d.lmp".as_ptr(), i + 1)~'
	'artifact icon truncated at 8|s~\Qf.max_qpath as usize,\E~8,~'
	'num_ex_items not counted|s~\Q*int_at(sv, f.sv_num_ex_items) += 1;\E~~'
	'pages always reallocated|s~\Qif (*ptr_at::<u8>(sv, f.sv_ex_inventory_pages)).is_null() {\E~if true {~'
	'pages one per client short|s~\Qmaxclients().wrapping_mul(f.max_inventory_ex_pages)\E~maxclients().wrapping_mul(f.max_inventory_ex_pages - 1)~'
	'inactive clients re-pointed|s~\Qif *int_at(client(i), f.client_active) != 0 {\E\n(\s*)\Q*ptr_at::<u8>(client(i), f.client_ex_inventory)\E~if *int_at(client(i), f.client_active) == 0 {\n${1}*ptr_at::<u8>(client(i), f.client_ex_inventory)~'
	'every client gets page 0|s~\QSV_ClientInventoryPage(i);\E~SV_ClientInventoryPage(0);~'
	# the world entity and the progs globals
	'world entity cleared three bytes per field|s~\Q.wrapping_mul(4) as usize,\E~.wrapping_mul(3) as usize,~'
	'world entity left free|s~\Q*int_at(ent, f.edict_free) = 0;\E~*int_at(ent, f.edict_free) = 1;~'
	'world model string is the map name|s~\QPR_SetEngineString(chars_at(worldmodel(), f.qmodel_name));\E~PR_SetEngineString(chars_at(sv, f.sv_name));~'
	'world modelindex 2|s~\Qf.v_modelindex) = 1.0;\E~f.v_modelindex) = 2.0;~'
	'solid and movetype swapped|s~\Qf.v_solid) = f.solid_bsp as c_float;\E~f.v_solid) = f.movetype_push as c_float;~'
	'movetype is solid_bsp|s~\Qf.v_movetype) = f.movetype_push as c_float;\E~f.v_movetype) = f.solid_bsp as c_float;~'
	'coop global gets deathmatch|s~\Q**ptr_at::<c_float>(globals, f.g_coop) = SvMainSpawn_CoopValue();\E~**ptr_at::<c_float>(globals, f.g_coop) = SvMainSpawn_DeathmatchValue();~'
	'globals branch inverted|s~\Q    if SvMainSpawn_CoopInteger() != 0 {\E\n(\s*)\Q**ptr_at\E~    if SvMainSpawn_CoopInteger() == 0 {\n${1}**ptr_at~'
	'deathmatch global gets coop|s~\Q= SvMainSpawn_DeathmatchValue();\E~= SvMainSpawn_CoopValue();~'
	'randomclass written when absent|s~\Qif !(*ptr_at::<c_float>(globals, f.g_randomclass)).is_null() {\E~if true {~'
	'randomclass never written|s~\Qif !(*ptr_at::<c_float>(globals, f.g_randomclass)).is_null() {\E~if false {~'
	'mapname is the startspot|s~\QPR_SetEngineString(chars_at(sv, f.sv_name));\E~PR_SetEngineString(chars_at(sv, f.sv_startspot));~'
	'startspot is the mapname|s~\QPR_SetEngineString(chars_at(sv, f.sv_startspot));\E~PR_SetEngineString(chars_at(sv, f.sv_name));~'
	'serverflags off by one|s~\Q= *int_at(svs, f.svs_serverflags) as c_float;\E~= (*int_at(svs, f.svs_serverflags) + 1) as c_float;~'
	'loading +5 before the entities is 4|s~\Q    loading_step(5);\E\n(\s*)\QED_LoadFromFile\E~    loading_step(4);\n${1}ED_LoadFromFile~'
	'entities from the map name|s~\QED_LoadFromFile(*ptr_at::<c_char>(worldmodel(), f.qmodel_entities));\E~ED_LoadFromFile(chars_at(sv, f.sv_name));~'
	# activation, settle frames, baselines, serverinfo
	'not marked active|s~\Q    *int_at(sv, f.sv_active) = 1;\E~    *int_at(sv, f.sv_active) = 0;~'
	'left loading|s~\Q*int_at(sv, f.sv_state) = f.ss_active;\E~*int_at(sv, f.sv_state) = f.ss_loading;~'
	'settle frametime 0.2|s~\Q*SvMainSpawn_HostFrametime() = 0.1;\E~*SvMainSpawn_HostFrametime() = 0.2;~'
	'one settle frame|s~\QSV_Physics();\E\n(\s*)\QSV_Physics();\E~SV_Physics();~'
	'SV_CreateBaseline dropped|s~\Q    SV_CreateBaseline();\E~~'
	'signon total counts one buffer too many|s~\Qwhile i < *int_at(sv, f.sv_num_signon_buffers) {\E~while i <= *int_at(sv, f.sv_num_signon_buffers) {~'
	'signon total without the open buffer|s~\Qtotal = total.wrapping_add(*int_at(sv.add(f.sv_signon), f.sizebuf_cursize));\E~~'
	'signon buffers in use not +1|s~\Q(*int_at(sv, f.sv_num_signon_buffers)).wrapping_add(1),\E~*int_at(sv, f.sv_num_signon_buffers),~'
	'signon line names the buffer size|s~\Q        f.max_signon_buffers,\E~        f.max_signon_size,~'
	'serverinfo to inactive clients|s~\Qif *int_at(*host_client, f.client_active) != 0 {\E~if *int_at(*host_client, f.client_active) == 0 {~'
	'host_client not advanced|s~\Q*host_client = (*host_client).add(f.client_size);\E~~'
	'host_client starts at client 1|s~\Q*host_client = *ptr_at::<u8>(svs, f.svs_clients);\E~*host_client = (*ptr_at::<u8>(svs, f.svs_clients)).add(f.client_size);~'
	'changelevel_issued left set|s~\Q*int_at(svs, f.svs_changelevel_issued) = 0;\E~*int_at(svs, f.svs_changelevel_issued) = 1;~'
	'spawned line|s~\Qc"Server spawned.\n"\E~c"Server spawned\n"~'
	'checklist test inverted|s~\Qif *int_at(sv, f.mc_active) != 0 {\E~if *int_at(sv, f.mc_active) == 0 {~'
	'final loading reset dropped|s~\Q    if f.client_build != 0 {\E\n(\s*)\Qloading_done();\E\n(\s*)\Q}\E\n\Q}\E~}\n~'
	# the checklist
	'header uses the full model path|s~\QCOM_SkipPath(chars_at(sv, f.sv_modelname))\E~chars_at(sv, f.sv_modelname)~'
	'header flag normal|s~\Qlet safe = f.print_safe as c_uint;\E~let safe = f.print_normal as c_uint;~'
	'lightdata test inverted|s~\Q(*ptr_at::<u8>(worldmodel(), f.qmodel_lightdata)).is_null()\E~(*ptr_at::<u8>(worldmodel(), f.qmodel_lightdata)).is_null() == false~'
	'visdata test inverted|s~\Qif (*ptr_at::<u8>(worldmodel(), f.qmodel_visdata)).is_null() {\E~if !(*ptr_at::<u8>(worldmodel(), f.qmodel_visdata)).is_null() {~'
	'pointfile name|s~\Qc"maps/%s.pts".as_ptr()\E~c"maps/%s.pt".as_ptr()~'
	'pointfile from the model name|s~\Qc"maps/%s.pts".as_ptr(),\E\n(\s*)\Qchars_at(sv, f.sv_name),\E~c"maps/%s.pts".as_ptr(),\n${1}chars_at(sv, f.sv_modelname),~'
	'pts test inverted|s~\QFS_FileExists(pointfile.as_ptr(), core::ptr::null_mut()) != 0\E~FS_FileExists(pointfile.as_ptr(), core::ptr::null_mut()) == 0~'
	'pointfile buffer one byte short|s~\Q[..f.max_ospath as usize]\E~[..f.max_ospath as usize - 1]~'
	'no changelevel test 1|s~\Qif mc(f.mc_trigger_changelevel) == 0 {\E~if mc(f.mc_trigger_changelevel) == 1 {~'
	'single changelevel test 2|s~\Q} else if mc(f.mc_trigger_changelevel) == 1 {\E~} else if mc(f.mc_trigger_changelevel) == 2 {~'
	'single changelevel valid test inverted|s~\Q        if mc(f.mc_valid_changelevel) == mc(f.mc_trigger_changelevel) {\E~        if mc(f.mc_valid_changelevel) != mc(f.mc_trigger_changelevel) {~'
	'changespot test inverted|s~\Qif *chars_at(sv, f.mc_changespot) != 0 {\E~if *chars_at(sv, f.mc_changespot) == 0 {~'
	'changespot line shows the level twice|s~\Qchars_at(sv, f.mc_changelevel),\E\n(\s*)\Qchars_at(sv, f.mc_changespot),\E~chars_at(sv, f.mc_changelevel),\n${1}chars_at(sv, f.mc_changelevel),~'
	'many changelevels valid test inverted|s~\Q} else if mc(f.mc_valid_changelevel) == mc(f.mc_trigger_changelevel) {\E~} else if mc(f.mc_valid_changelevel) != mc(f.mc_trigger_changelevel) {~'
	'missing count reversed|s~\Qmc(f.mc_trigger_changelevel).wrapping_sub(mc(f.mc_valid_changelevel))\E~mc(f.mc_valid_changelevel).wrapping_sub(mc(f.mc_trigger_changelevel))~'
	'intermission test inverted|s~\Qif mc(f.mc_intermission) != 0 {\E~if mc(f.mc_intermission) == 0 {~'
	'intermission dm test >= 0|s~\Q} else if mc(f.mc_dm_spawns) > 0 {\E~} else if mc(f.mc_dm_spawns) >= 0 {~'
	'intermission counts as a warning|s~\Qf.mapcheck_na,\E\n(\s*)\Qc"info_intermission (none\E~f.mapcheck_partial,\n${1}c"info_intermission (none~'
	'skill triggers > 1|s~\Qmc(f.mc_skill_triggers) > 0\E~mc(f.mc_skill_triggers) > 1~'
	'skill or -> and|s~\Qmc(f.mc_skill_triggers) > 0\E\n(\s*)\Q||\E~mc(f.mc_skill_triggers) > 0\n${1}\&\&~'
	'skill ents compares 0 with 2|s~\Qmc(ents + int) != mc(ents + 2 * int)\E~mc(ents) != mc(ents + 2 * int)~'
	'skill ents first comparison dropped|s~\Q(mc(ents) != mc(ents + int) ||\E~(false ||~'
	'skill result inverted|s~\Qif skill_levels {\E~if !skill_levels {~'
	'sp spawns > 1|s~\Qif mc(f.mc_sp_spawns) > 0 {\E~if mc(f.mc_sp_spawns) > 1 {~'
	'coop threshold 2|s~\Qconst MIN_COOP_SPAWN_POINTS: c_int = 3;\E~const MIN_COOP_SPAWN_POINTS: c_int = 2;~'
	'dm threshold 4|s~\Qconst MIN_DM_SPAWN_POINTS: c_int = 5;\E~const MIN_DM_SPAWN_POINTS: c_int = 4;~'
	'thresh floor < 0|s~\Qif current <= 0 {\E~if current < 0 {~'
	'thresh ok > target|s~\Qif current >= target {\E~if current > target {~'
	'thresh partial is failed|s~\Q    f.mapcheck_partial\E\n\Q}\E~    f.mapcheck_failed\n\}~'
	'dm hang note inverted|s~\Qif mc(f.mc_dm_spawns) != 0 {\E~if mc(f.mc_dm_spawns) == 0 {~'
	'music midi and cd -> or|s~\Qif *midi != 0 && cd != 0 {\E~if *midi != 0 || cd != 0 {~'
	'music midi test inverted|s~\Q} else if *midi != 0 {\E~} else if *midi == 0 {~'
	'music cd test inverted|s~\Q} else if cd != 0 {\E~} else if cd == 0 {~'
	'title test inverted|s~\Qif buf[0] != 0 {\E~if buf[0] == 0 {~'
	'title message test >= 0|s~\Qif cvt_c(message) > 0 {\E~if cvt_c(message) >= 0 {~'
	'title message saturates|s~\Qif cvt_c(message) > 0 {\E~if (message as c_int) > 0 {~'
	'title control below 31|s~\Qif c < 32 || c == 127 {\E~if c < 31 || c == 127 {~'
	'title DEL kept|s~\Qif c < 32 || c == 127 {\E~if c < 32 || c == 126 {~'
	'title leading space kept|s~\Qif n == 0 || out[n - 1] == b'"'"' '"'"' {\E~if out.get(n.wrapping_sub(1)) == Some(\&b'"'"' '"'"') {~'
	'title doubled space kept|s~\Qif n == 0 || out[n - 1] == b'"'"' '"'"' {\E~if n == 0 {~'
	'title trailing space kept|s~\Qwhile n > 0 && out[n - 1] == b'"'"' '"'"' {\E~while false {~'
	'title writes the last byte|s~\Q*src != 0 && n + 1 < outsize\E~*src != 0 \&\& n < outsize~'
	'title not terminated|s~\Q    out[n] = 0;\E~~'
	'title NULL is a crash|s~\Qif src.is_null() {\E~if false {~'
	'sky block on the wrong build|s~\Q    if f.client_build != 0 {\E\n(\s*)\Qlet mut numskies\E~    if f.client_build == 0 {\n${1}let mut numskies~'
	'sky prefix 2 chars|s~\Qc"sky".as_ptr(), 3)\E~c"sky".as_ptr(), 2)~'
	'sky null slot read|s~\Qtex.is_null() || strncmp\E~false || strncmp~'
	'sky count > 0|s~\Qif numskies > 1 || oddsized > 0 {\E~if numskies > 0 || oddsized > 0 {~'
	'sky oddsized > 1|s~\Qif numskies > 1 || oddsized > 0 {\E~if numskies > 1 || oddsized > 1 {~'
	'sky width 255|s~\Qcast::<c_uint>() == 256\E~cast::<c_uint>() == 255~'
	'sky height 127|s~\Qcast::<c_uint>() == 128\E~cast::<c_uint>() == 127~'
	'sky size and -> or|s~\Q== 256\E\n(\s*)\Q&&\E~== 256\n${1}||~'
	'sky odd text|s~\Qc"256 x 128 ".as_ptr()\E~c"256x128 ".as_ptr()~'
	'sky odd text on the wrong test|s~\Qif oddsized > 0 {\E~if oddsized > 1 {~'
	'sky list odd test inverted|s~\Q                if !sky_256x128(f, tex) {\E~                if sky_256x128(f, tex) {~'
	'sky list sizes swapped|s~\Q*tex.add(f.texture_width).cast::<c_uint>(),\E\n(\s*)\Q*tex.add(f.texture_height).cast::<c_uint>(),\E~*tex.add(f.texture_height).cast::<c_uint>(),\n${1}*tex.add(f.texture_width).cast::<c_uint>(),~'
	'sky list starts at 1|s~\Q            let mut i: c_int = 0;\E\n(\s*)\Qwhile i < *int_at(worldmodel(), f.qmodel_numtextures) {\E\n(\s*)\Qlet tex = sky_texture(f, i);\E\n(\s*)\Qi += 1;\E\n(\s*)\Qif tex.is_null() {\E\n(\s*)\Qcontinue;\E\n(\s*)\Q}\E\n(\s*)\Qlet name\E~            let mut i: c_int = 1;\n${1}while i < *int_at(worldmodel(), f.qmodel_numtextures) {\n${2}let tex = sky_texture(f, i);\n${3}i += 1;\n${4}if tex.is_null() {\n${5}continue;\n${6}}\n${7}let name~'
	'footer zero test inverted|s~\Qif warnings != 0 {\E~if warnings == 0 {~'
	'footer plural at 2|s~\Qif warnings == 1 {\E~if warnings == 2 {~'
	'footer counts before the checks|s~\Qlet warnings = mc(f.mc_numwarnings);\E~let warnings = mc(f.mc_numwarnings) - 1;~'
	'control: a comment changes nothing the harness sees|s~\Q// footer\E~// footer (mutated)~'
)

survived=0 killed=0 equivalent=0 total=0
for m in "${mutants[@]}"; do
	name="${m%%|*}"; sub="${m#*|}"
	[ -n "$filter" ] && ! grep -qE -- "$filter" <<<"$name" && continue
	total=$((total + 1))
	perl -0pe "$sub" "$orig" >"$src"
	if cmp -s "$src" "$orig"; then echo "BROKEN MUTANT (no change): $name" >&2; exit 2; fi
	# MUTANT_DRYRUN=1: only check that every substitution applies
	# and show the first line it changes, so a pattern that lands in a
	# comment rather than the code it names is visible
	if [ "${MUTANT_DRYRUN:-0}" = 1 ]; then
		echo "applies:  $name :: $(diff "$orig" "$src" | grep -m1 '^[<>]' | sed 's/^[<>] *//')"
		continue
	fi
	rc=0
	kill_cmd || rc=$?
	if [ "$rc" -eq 2 ]; then echo "(mutant: $name)" >&2; exit 2; fi
	case "$name" in
		EQUIVALENT*|control:*)
			if [ "$rc" -eq 0 ]; then
				echo "equivalent (survived, as proved): $name"
				equivalent=$((equivalent + 1))
			else
				echo "KILLED AN EQUIVALENT: $name -- its proof is wrong" >&2
				grep -E 'MISMATCH|FAIL' "$work/last.log" | head -3 >&2
				survived=$((survived + 1))
			fi ;;
		*)
			if [ "$rc" -eq 0 ]; then
				echo "SURVIVED: $name"
				survived=$((survived + 1))
			else
				why=$(grep -E -m1 'MISMATCH|FAIL' "$work/last.log" || true)
				echo "killed:   $name  [${why:0:110}]"
				killed=$((killed + 1))
			fi ;;
	esac
done
cp "$orig" "$src"
echo "mutants: $total run, $killed killed, $equivalent equivalent (proved), $survived wrong"
[ "$survived" -eq 0 ] || { echo "FAIL: $survived mutant(s) survived or a proof failed" >&2; exit 1; }
echo "PASS"
