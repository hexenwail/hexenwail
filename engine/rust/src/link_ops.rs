// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rust replacement for engine/h2shared/link_ops.c.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 2026 Hexenwail contributors.
//
// Four operations on an intrusive doubly-linked list.  The list is *entirely*
// caller-owned: the link_t lives inside an edict or an area node, the head is
// a sentinel whose own link_t is the list, and the C side keeps raw pointers
// into the middle of the chain (engine/hexen2/world.c's sv_link_next /
// sv_link_prev during SV_TouchLinks).  Nothing here allocates, and nothing
// here validates: every function is exactly the pointer assignment the C
// original performs, in the same order, so a caller that leaves the list in a
// deliberately odd state (an unlinked edict, a node inserted twice) observes
// the same odd state either way.
//
// Two properties are load-bearing and must not be "improved":
//
//   * RemoveLink does not touch `l` itself.  SV_UnlinkEdict reads
//     ent->area.next / ent->area.prev *after* removing the link, and the
//     stale values are what it wants; clearing them would silently break the
//     touch loop's saved cursor.
//   * The statements keep C's evaluation order.  Where the arguments alias
//     (`l == before`, or a link that is already in the list), the result
//     depends on which assignment happens first, and the C order is the
//     contract.

/// `link_t` from engine/h2shared/link_ops.h: two self-referential pointers,
/// `prev` first.  C stores them as `struct link_s *`, so this is a plain
/// `#[repr(C)]` pair of raw pointers -- no Rust reference is ever formed,
/// which is what lets the C side alias them freely.
#[repr(C)]
pub struct LinkC {
    pub prev: *mut LinkC,
    pub next: *mut LinkC,
}

const PTR_SIZE: usize = core::mem::size_of::<*mut LinkC>();
const PTR_ALIGN: usize = core::mem::align_of::<*mut LinkC>();

const _: () = {
    assert!(core::mem::offset_of!(LinkC, prev) == 0);
    assert!(core::mem::offset_of!(LinkC, next) == PTR_SIZE);
    assert!(core::mem::size_of::<LinkC>() == 2 * PTR_SIZE);
};

// These accessors let the C differential harness check the layout without
// restating Rust's assumptions in C, as the sizebuf port does.
#[no_mangle]
pub extern "C" fn LinkC_sizeof() -> usize {
    core::mem::size_of::<LinkC>()
}

#[no_mangle]
pub extern "C" fn LinkC_alignof() -> usize {
    PTR_ALIGN
}

#[no_mangle]
pub extern "C" fn LinkC_offsetof_prev() -> usize {
    core::mem::offset_of!(LinkC, prev)
}

#[no_mangle]
pub extern "C" fn LinkC_offsetof_next() -> usize {
    core::mem::offset_of!(LinkC, next)
}

/// `ClearLink` -- C: `l->prev = l->next = l;`
#[no_mangle]
pub unsafe extern "C" fn ClearLink(l: *mut LinkC) {
    (*l).next = l;
    (*l).prev = l;
}

/// `RemoveLink` -- C: `l->next->prev = l->prev; l->prev->next = l->next;`
///
/// `l` is left alone, and the second statement re-reads `l->prev` after the
/// first one may have written it, exactly as the C does.
#[no_mangle]
pub unsafe extern "C" fn RemoveLink(l: *mut LinkC) {
    (*(*l).next).prev = (*l).prev;
    (*(*l).prev).next = (*l).next;
}

/// `InsertLinkBefore` -- C: `l->next = before; l->prev = before->prev;
/// l->prev->next = l; l->next->prev = l;`
#[no_mangle]
pub unsafe extern "C" fn InsertLinkBefore(l: *mut LinkC, before: *mut LinkC) {
    (*l).next = before;
    (*l).prev = (*before).prev;
    (*(*l).prev).next = l;
    (*(*l).next).prev = l;
}

/// `InsertLinkAfter` -- C: `l->next = after->next; l->prev = after;
/// l->prev->next = l; l->next->prev = l;`
///
/// No engine caller uses this one today (world.c links everything through
/// InsertLinkBefore); it is part of the ABI and is kept switchable with the
/// rest.
#[no_mangle]
pub unsafe extern "C" fn InsertLinkAfter(l: *mut LinkC, after: *mut LinkC) {
    (*l).next = (*after).next;
    (*l).prev = after;
    (*(*l).prev).next = l;
    (*(*l).next).prev = l;
}
