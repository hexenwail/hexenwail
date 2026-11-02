// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for the extended inventory, worldspawn sky, level name and
// savegame helpers of engine/hexen2/sv_main.c: SV_ParseWorldspawnSky,
// SV_ClientInventoryPage, INV_UpdateExItem, SV_GetLevelname,
// SV_SaveSpawnparms, INV_WritePage, SV_LoadInventory and INV_SavePages.
// #469 slice E (unit E1), of #455.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// sv.ex_inventory_pages is a hunk array of maxclients * MAX_INVENTORY_EX_PAGES
// pages; a client's chain starts at the page SV_ClientInventoryPage finds or
// claims for it, and INV_UpdateExItem walks and extends that chain.  The
// savegame writes each client's start page as one "Page:" text line
// (INV_SavePages, INV_WritePage) and reads them back (SV_LoadInventory).
// SV_ParseWorldspawnSky fills sv.skybox from the world's entity lump, and
// SV_GetLevelname resolves the world's message/netname.  The C originals
// moved verbatim into engine/rust/sv_main/tests/sv_main_inv_h2.c.
//
// Hexen II only, like the other sv_main_* slices.  The layouts, the constants
// and the globals come through engine/rust/sv_main_target.c.  HexenWorld's
// server has its own C SV_GetLevelname and SV_SaveSpawnparms (sv_init.c) and
// none of the rest.
//
// THE STDIO FUNCTIONS CALL THE C LIBRARY
//
//   INV_WritePage, INV_SavePages and SV_LoadInventory read and write the
//   engine's `FILE *` with the C library's own fprintf, fscanf and fgetc,
//   called through FFI (calling a C variadic is stable Rust; only defining
//   one is not).  The stream is the C library's object -- its buffer, its
//   position, its EOF and error flags, and the C's other reads and writes of
//   the same savegame around these calls -- so the only way to keep its state
//   exactly as the C left it is to let the same library touch it.  A Rust
//   formatter or scanner over fread/fwrite would have to reproduce scanf's
//   whitespace directives, its partial assignment and its one-character
//   pushback on a mismatch, and could still not share the stream's pushback
//   with the C callers.  cvar.rs and cmd.rs already write the engine's
//   config files with the C library's fprintf the same way.
//
//   Two library entry points differ by name from what the C compiles to,
//   neither in any way these formats reach: glibc's headers redirect C99
//   fscanf to __isoc99_fscanf, which differs from the plain fscanf this
//   links only in %a; _FORTIFY_SOURCE turns the C's fprintf into
//   __fprintf_chk, which differs only in its %n checks.  The formats here are
//   %d/%i, literals and whitespace directives.
//
// C SEMANTICS KEPT
//
//   * `(int)` of a float -- sv.edicts->v.message and *sv_globals.serverflags
//     -- goes through world_hull.rs's cvt_c, the target's own conversion.
//   * COM_Parse, q_strlcpy, Host_GetString, PR_GetString, Host_Error and
//     the stdio are the engine's/C library's, called as the C calls them,
//     on the same arguments in the same order.
//   * SV_ParseWorldspawnSky's key is a 128-byte local, cut by q_strlcpy and
//     trimmed of trailing spaces; a key that is exactly "sky", "skyname" or
//     "qlsky" after that copies the value into sv.skybox (sizeof).
//   * SV_ClientInventoryPage returns the first page that is free or the
//     client's own, and claims a free one with `++sv.next_page_id`.
//   * INV_UpdateExItem's first walk updates the matching slot on *every*
//     page of the chain (its break leaves the slot loop, not the page loop),
//     and its result is the last one written.
//   * SV_SaveSpawnparms leaves host_client one past the last client
//     (svs.clients when maxclients <= 0), as the C's for loop does; INV_SavePages
//     walks a local copy and leaves the global alone.  The C loop's
//     `host_client->active` test guards nothing (the body is all comments),
//     so the port does not make the read; an optimising compiler drops it
//     from the C too.
//   * SV_LoadInventory's page search is bounded by svs.maxclients, not by
//     the array's maxclients * MAX_INVENTORY_EX_PAGES; its ints keep the value
//     of the last fscanf that assigned them; nextId is read and ignored; and a
//     line ends at the first '\n' or '\r' fgetc returns.
//
// UNDEFINED BEHAVIOUR IN THE C, AND WHAT THE PORT DOES
//
//   The port does what the C does for every input the C handles defined.
//   Where the C's behaviour is undefined:
//
//   * `1 << i` for i == 31 (slot 31's changed/new bit) shifts into the sign
//     bit.  The port gives INT_MIN, which is what GCC and Clang produce.
//   * `item_cnt += inv_cnt`, `++sv.next_page_id` and
//     `svs.maxclients * MAX_INVENTORY_EX_PAGES` can overflow int.  The port
//     wraps (two's complement), which is what the compiled C does.
//   * SV_LoadInventory reads an int no fscanf ever assigned (the first
//     "Page:" line or the first item pair fails to scan).  The C reads stack
//     garbage; the port reads 0.  The harness compiles the C with
//     -ftrivial-auto-var-init=zero so those inputs are compared too.
//   * SV_LoadInventory's item_idx has no bound: a line with more than
//     MAX_INVENTORY_EX pairs writes past item_id[] and item_cnt[] (into the
//     rest of the page, then the next page), and a line cut short by EOF
//     loops forever -- fgetc keeps returning EOF, which is neither '\n' nor
//     '\r' -- writing further each time until it faults.  The port aborts
//     (bounds check, panic = abort) at the top of the item loop once
//     item_idx reaches MAX_INVENTORY_EX, before the fscanf that iteration
//     would make: the stream is where the C's was, and nothing has been
//     written past the page.
//   * SV_LoadInventory with sv.ex_inventory_pages NULL dereferences NULL.
//     SV_SpawnServer always allocates the array first, so this is
//     unreachable; the port aborts where the C would first read through it.
//
// Every one of these is a defect of the C, reported separately; none is fixed
// here.

use core::ffi::{c_char, c_int, c_void};

use crate::world_hull::cvt_c;

/// sv_main_target.h's `sv_main_inv_facts_t`.
#[repr(C)]
pub struct SvMainInvFacts {
    pub max_inventory_ex: c_int,
    pub max_inventory_ex_pages: c_int,
    pub max_clients: c_int,
    pub ex_size: usize,
    pub ex_id: usize,
    pub ex_client_id: usize,
    pub ex_changed_items: usize,
    pub ex_new_items: usize,
    pub ex_item_id: usize,
    pub ex_item_cnt: usize,
    pub ex_next: usize,
    pub client_size: usize,
    pub client_active: usize,
    pub client_ex_inventory: usize,
    pub edict_v: usize,
    pub v_message: usize,
    pub v_netname: usize,
    pub qmodel_entities: usize,
    pub skybox_size: usize,
}

// Layout accessors, for engine/rust/tests/abi_layout.c and the harness.
macro_rules! accessor {
    ($name:ident, $e:expr) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            $e
        }
    };
}
accessor!(SvMainInvFacts_sizeof, core::mem::size_of::<SvMainInvFacts>());
accessor!(
    SvMainInvFacts_offsetof_max_clients,
    core::mem::offset_of!(SvMainInvFacts, max_clients)
);
accessor!(
    SvMainInvFacts_offsetof_ex_size,
    core::mem::offset_of!(SvMainInvFacts, ex_size)
);
accessor!(
    SvMainInvFacts_offsetof_ex_next,
    core::mem::offset_of!(SvMainInvFacts, ex_next)
);
accessor!(
    SvMainInvFacts_offsetof_client_ex_inventory,
    core::mem::offset_of!(SvMainInvFacts, client_ex_inventory)
);
accessor!(
    SvMainInvFacts_offsetof_skybox_size,
    core::mem::offset_of!(SvMainInvFacts, skybox_size)
);

/// The C library's `FILE`, opaque: only ever handed back to the library.
/// `c_void`, as cvar.rs's fprintf and quakefs.rs's LibcFile spell it, so the
/// crate's declarations of the same libc functions agree.
pub type CFile = c_void;

/// stdio.h's EOF is never '\n' or '\r', so the item loop needs no name for it.
const NEWLINE: c_int = b'\n' as c_int;
const RETURN: c_int = b'\r' as c_int;

/// SV_ParseWorldspawnSky's `char key[128]`.
const KEY_SIZE: usize = 128;

extern "C" {
    static SvMainInv_TargetFacts: SvMainInvFacts;
    fn SvMain_InventoryPages() -> *mut u8;
    fn SvMain_NextPageId() -> *mut c_int;
    fn SvMain_SkyboxBuf() -> *mut c_char;
    fn SvMain_ServerFlags() -> *mut c_int;
    fn SvMain_GlobalServerFlags() -> *mut f32;
    fn SvMain_HostStringCount() -> c_int;
    fn SvMain_ComToken() -> *mut c_char;
    fn SvMain_WorldModel() -> *mut u8;
    fn SvMain_Edicts() -> *mut u8;
    fn SvMain_Clients() -> *mut u8;
    fn SvMain_MaxClients() -> c_int;
    fn SvMain_HostClient() -> *mut *mut u8;

    fn COM_Parse(data: *const c_char) -> *const c_char;
    fn q_strlcpy(dst: *mut c_char, src: *const c_char, size: usize) -> usize;
    fn Host_GetString(idx: c_int) -> *const c_char;
    fn PR_GetString(num: c_int) -> *const c_char;
    fn Host_Error(error: *const c_char, ...) -> !;

    fn fprintf(stream: *mut CFile, format: *const c_char, ...) -> c_int;
    fn fscanf(stream: *mut CFile, format: *const c_char, ...) -> c_int;
    fn fgetc(stream: *mut CFile) -> c_int;

    fn abort() -> !;
}

#[inline(always)]
unsafe fn facts() -> &'static SvMainInvFacts {
    &*(&raw const SvMainInv_TargetFacts)
}

// ---- ex_inventory_page_t, by the target's offsets ----------------------------

#[inline(always)]
unsafe fn pint(page: *mut u8, member: usize) -> *mut c_int {
    page.add(member) as *mut c_int
}

/// `&page->item_id[i]` (`which` is ex_item_id) or `&page->item_cnt[i]`.
/// Only ever called with 0 <= i < MAX_INVENTORY_EX.
#[inline(always)]
unsafe fn slot(page: *mut u8, which: usize, i: c_int) -> *mut c_int {
    (page.add(which) as *mut c_int).add(i as usize)
}

#[inline(always)]
unsafe fn next(page: *mut u8) -> *mut *mut u8 {
    page.add(facts().ex_next) as *mut *mut u8
}

/// `&sv.ex_inventory_pages[i]`, i >= 0.
#[inline(always)]
unsafe fn page_at(pages: *mut u8, i: c_int) -> *mut u8 {
    pages.add(i as usize * facts().ex_size)
}

/// `1 << i`, i in 0..32: slot 31 gives INT_MIN, as the compiled C does.
#[inline(always)]
fn bit(i: c_int) -> c_int {
    1i32.wrapping_shl(i as u32)
}

// ---- the worldspawn sky -------------------------------------------------------

/// The NUL-terminated `key` equals the NUL-terminated `want`.
fn key_is(key: &[c_char; KEY_SIZE], want: &[u8]) -> bool {
    let mut i = 0;
    loop {
        let k = key[i] as u8;
        if k != want[i] {
            return false;
        }
        if k == 0 {
            return true;
        }
        i += 1;
    }
}

/// `SV_ParseWorldspawnSky`: sv.skybox from the worldspawn's sky, skyname or
/// qlsky key (a leading underscore and trailing spaces ignored), else "".
#[no_mangle]
pub unsafe extern "C" fn SV_ParseWorldspawnSky() {
    let f = facts();
    let skybox = SvMain_SkyboxBuf();
    let token = SvMain_ComToken();
    let mut key: [c_char; KEY_SIZE] = [0; KEY_SIZE];

    *skybox = 0;

    let world = SvMain_WorldModel();
    if world.is_null() {
        return;
    }
    let entities = *(world.add(f.qmodel_entities) as *const *const c_char);
    if entities.is_null() {
        return;
    }

    let mut data = COM_Parse(entities);
    if data.is_null() || *token != b'{' as c_char {
        return;
    }

    loop {
        data = COM_Parse(data);
        if data.is_null() {
            return;
        }
        if *token == b'}' as c_char {
            return; // end of worldspawn
        }

        let src = if *token == b'_' as c_char {
            token.add(1)
        } else {
            token
        };
        q_strlcpy(key.as_mut_ptr(), src, KEY_SIZE);
        let mut len = key.iter().position(|&c| c == 0).unwrap_or(KEY_SIZE);
        while key[0] != 0 && key[len - 1] == b' ' as c_char {
            key[len - 1] = 0;
            len -= 1;
        }

        data = COM_Parse(data);
        if data.is_null() {
            return;
        }

        if key_is(&key, b"sky\0") || key_is(&key, b"skyname\0") || key_is(&key, b"qlsky\0") {
            q_strlcpy(skybox, token, f.skybox_size);
        }
    }
}

// ---- the extended inventory ---------------------------------------------------

/// `SV_ClientInventoryPage`: the first page that is free or `clientnum`'s,
/// claimed with the next page id when free; NULL when there is none.
#[no_mangle]
pub unsafe extern "C" fn SV_ClientInventoryPage(clientnum: c_int) -> *mut u8 {
    let f = facts();
    let pages = SvMain_InventoryPages();
    if pages.is_null() {
        return core::ptr::null_mut();
    }

    let npages = SvMain_MaxClients().wrapping_mul(f.max_inventory_ex_pages);

    let mut i: c_int = 0;
    while i < npages {
        let page = page_at(pages, i);
        if *pint(page, f.ex_id) != 0 && *pint(page, f.ex_client_id) != clientnum {
            i += 1;
            continue; // in use by someone else
        }
        if *pint(page, f.ex_id) == 0 {
            let next_id = SvMain_NextPageId();
            *next_id = (*next_id).wrapping_add(1);
            *pint(page, f.ex_id) = *next_id;
        }
        return page;
    }

    core::ptr::null_mut()
}

/// `INV_UpdateExItem`: set (or, with `inc`, add to) the count of item
/// `inv_id` in the chain from `start_page`; a positive count of an item the
/// chain lacks takes the first empty slot, extending the chain by
/// SV_ClientInventoryPage when every page is full.  Returns the last count
/// written, or 0.
#[no_mangle]
pub unsafe extern "C" fn INV_UpdateExItem(
    start_page: *mut u8,
    inv_id: c_int,
    inv_cnt: c_int,
    inc: c_int,
) -> c_int {
    let f = facts();
    let max = f.max_inventory_ex;
    let mut found = false;
    let mut result: c_int = 0;

    let mut page = start_page;
    while !page.is_null() {
        // try to find a matching slot
        let mut i: c_int = 0;
        while i < max {
            if *slot(page, f.ex_item_id, i) == inv_id {
                let cnt = slot(page, f.ex_item_cnt, i);
                if inc != 0 {
                    *cnt = (*cnt).wrapping_add(inv_cnt);
                } else {
                    *cnt = inv_cnt;
                }
                *pint(page, f.ex_changed_items) |= bit(i);
                result = *cnt;
                found = true;
                break;
            }
            i += 1;
        }
        page = *next(page);
    }

    if inv_cnt > 0 && !found {
        // no matching slot found, create one at first empty
        page = start_page;
        while !page.is_null() {
            let mut i: c_int = 0;
            while i < max {
                if *slot(page, f.ex_item_id, i) == 0 {
                    *slot(page, f.ex_item_id, i) = inv_id;
                    let cnt = slot(page, f.ex_item_cnt, i);
                    if inc != 0 {
                        *cnt = (*cnt).wrapping_add(inv_cnt);
                    } else {
                        *cnt = inv_cnt;
                    }
                    *pint(page, f.ex_changed_items) |= bit(i);
                    *pint(page, f.ex_new_items) |= bit(i);
                    result = *cnt;
                    found = true;
                    break;
                }
                i += 1;
            }

            if found {
                break;
            }
            if (*next(page)).is_null() {
                // The owner is whoever owns the chain being extended.
                *next(page) = SV_ClientInventoryPage(*pint(start_page, f.ex_client_id));
                // Mutation-equivalent: without this break the loop goes on
                // to `page = *next(page)`, which is NULL here, and the while
                // condition ends it with nothing executed in between -- the
                // same exit.  Kept because it is the C's text.
                if (*next(page)).is_null() {
                    break; // out of pages
                }
            }
            page = *next(page);
        }
    }

    result
}

// ---- the level name and the spawn parms --------------------------------------

/// `SV_GetLevelname`: strings.txt's entry for the world's `message`, when it
/// is 1..=host_string_count, else the world's `netname`.
#[no_mangle]
pub unsafe extern "C" fn SV_GetLevelname() -> *const c_char {
    let f = facts();
    let v = SvMain_Edicts().add(f.edict_v);
    let idx = cvt_c(*(v.add(f.v_message) as *const f32));
    if idx > 0 && idx <= SvMain_HostStringCount() {
        return Host_GetString(idx - 1);
    }
    PR_GetString(*(v.add(f.v_netname) as *const c_int))
}

/// `SV_SaveSpawnparms`: svs.serverflags from the progs global; the client
/// loop's body is all commented out in the C, so what it leaves is
/// host_client one past the last client.
#[no_mangle]
pub unsafe extern "C" fn SV_SaveSpawnparms() {
    let f = facts();
    *SvMain_ServerFlags() = cvt_c(*SvMain_GlobalServerFlags());

    let maxclients = SvMain_MaxClients();
    let mut client = SvMain_Clients();
    let mut i: c_int = 0;
    while i < maxclients {
        i += 1;
        client = client.wrapping_add(f.client_size);
    }
    *SvMain_HostClient() = client;
}

// ---- the savegame -------------------------------------------------------------

/// `INV_WritePage`: one "Page: id next_id client_id {item_id item_cnt}" line,
/// listing the slots with an item and a positive count.
#[no_mangle]
pub unsafe extern "C" fn INV_WritePage(file: *mut CFile, page: *mut u8, client_id: c_int) {
    let f = facts();
    let next_page = *next(page);
    let next_id = if next_page.is_null() {
        -1
    } else {
        *pint(next_page, f.ex_id)
    };
    fprintf(
        file,
        c"Page: %i %i %i".as_ptr(),
        *pint(page, f.ex_id),
        next_id,
        client_id,
    );
    let mut i: c_int = 0;
    while i < f.max_inventory_ex {
        let id = *slot(page, f.ex_item_id, i);
        let cnt = *slot(page, f.ex_item_cnt, i);
        if id != 0 && cnt > 0 {
            fprintf(file, c" %i %i".as_ptr(), id, cnt);
        }
        i += 1;
    }
    fprintf(file, c"\n".as_ptr());
}

/// `SV_LoadInventory`: read INV_SavePages' "Pages:" count and that many
/// "Page:" lines back into sv.ex_inventory_pages.
#[no_mangle]
pub unsafe extern "C" fn SV_LoadInventory(file: *mut CFile) {
    let f = facts();
    let mut total: c_int = 0;
    // Zero where the C's are uninitialised: see the module header.
    let mut id: c_int = 0;
    let mut client_id: c_int = 0;
    let mut next_id: c_int = 0;
    let mut item_id: c_int = 0;
    let mut item_cnt: c_int = 0;

    fscanf(file, c"Pages: %d\n".as_ptr(), &mut total as *mut c_int);
    if total < 0 || total > f.max_clients {
        Host_Error(c"%s: bad numpages".as_ptr(), c"SV_LoadInventory".as_ptr());
    }

    let mut count: c_int = 0;
    while count < total {
        fscanf(
            file,
            c"Page: %d %d %d".as_ptr(),
            &mut id as *mut c_int,
            &mut next_id as *mut c_int,
            &mut client_id as *mut c_int,
        );
        let pages = SvMain_InventoryPages();
        if pages.is_null() {
            abort(); // the C dereferences NULL here: see the module header
        }
        let maxclients = SvMain_MaxClients();
        let mut i: c_int = 0;
        while i < maxclients
            && *pint(page_at(pages, i), f.ex_id) != 0
            && *pint(page_at(pages, i), f.ex_client_id) != client_id
        {
            i += 1;
        }
        let page = page_at(pages, i);
        core::ptr::write_bytes(page, 0, f.ex_size);
        *pint(page, f.ex_id) = id;
        *pint(page, f.ex_client_id) = client_id;
        let next_page_id = SvMain_NextPageId();
        // Mutation-equivalent: `>=` would also store when id equals
        // sv.next_page_id, which writes the value already there.
        if id > *next_page_id {
            *next_page_id = id;
        }

        let mut item_idx: c_int = 0;
        let mut c = fgetc(file); // read one char, see what it is
        while c != NEWLINE && c != RETURN {
            if item_idx >= f.max_inventory_ex {
                abort(); // the C writes past the page here: see the module header
            }
            fscanf(
                file,
                c"%d %d".as_ptr(),
                &mut item_id as *mut c_int,
                &mut item_cnt as *mut c_int,
            );
            *slot(page, f.ex_item_id, item_idx) = item_id;
            *slot(page, f.ex_item_cnt, item_idx) = item_cnt;
            *pint(page, f.ex_changed_items) |= bit(item_idx);
            *pint(page, f.ex_new_items) |= bit(item_idx);

            item_idx += 1;
            c = fgetc(file); // read one char, see what it is
        }
        count += 1;
    }
}

/// `INV_SavePages`: "Pages: n" for the clients with a start page, then each
/// of those pages.
#[no_mangle]
pub unsafe extern "C" fn INV_SavePages(file: *mut CFile) {
    let f = facts();
    let clients = SvMain_Clients();
    let maxclients = SvMain_MaxClients();
    let ex_inventory = |i: c_int| {
        *(clients.add(i as usize * f.client_size + f.client_ex_inventory) as *mut *mut u8)
    };

    let mut j: c_int = 0;
    let mut i: c_int = 0;
    while i < maxclients {
        if !ex_inventory(i).is_null() {
            j += 1;
        }
        i += 1;
    }

    fprintf(file, c"Pages: %i\n".as_ptr(), j);

    i = 0;
    while i < maxclients {
        let page = ex_inventory(i);
        if !page.is_null() {
            INV_WritePage(file, page, i);
        }
        i += 1;
    }
}
