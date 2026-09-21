// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for engine/h2shared/link_ops.c and the Rust link_ops
// module.  The C original is compiled with c_* names so both implementations
// operate on structurally identical, independently owned lists in one
// process.
//
// Because the two lists live at different addresses, the comparison is a
// normalised topology: every prev/next pointer is mapped to the index of the
// link it points at (sentinel, node 0..N-1, NULL, or "unknown"), and the two
// fingerprint vectors must match after every operation.  That catches a wrong
// field, a missing write, an extra write, and a write in the wrong order,
// without depending on where the allocator put anything.

#include "q_stdinc.h"
#include "link_ops.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		return 1; \
	} \
} while (0)

extern void c_ClearLink(link_t *l);
extern void c_RemoveLink(link_t *l);
extern void c_InsertLinkBefore(link_t *l, link_t *before);
extern void c_InsertLinkAfter(link_t *l, link_t *after);

extern size_t LinkC_sizeof(void);
extern size_t LinkC_alignof(void);
extern size_t LinkC_offsetof_prev(void);
extern size_t LinkC_offsetof_next(void);

// Engine callbacks other features in the consolidated archive reference.  The
// link_ops module needs none of them; they exist only so the link succeeds
// whichever codegen units the linker pulls, and must never be reached.
void Sys_Error(const char *fmt, ...) { (void)fmt; abort(); }
void CON_Printf(unsigned int flags, const char *fmt, ...) { (void)flags; (void)fmt; abort(); }
void *Hunk_AllocName(int size, const char *name) { (void)size; (void)name; abort(); }

enum { NODE_COUNT = 8 };
#define CANARY 0x5a5a5a5au

// Same shape as the engine's use: a link_t embedded in a larger caller-owned
// object (edict_t.area, areanode_t.trigger_edicts).  The canaries bracket the
// link so a write at the wrong offset is visible.
typedef struct node_s {
	int id;
	unsigned canary_before;
	link_t link;
	unsigned canary_after;
} node_t;

typedef struct head_s {
	unsigned canary_before;
	link_t link;
	unsigned canary_after;
} head_t;

static head_t c_head, r_head;
static node_t c_nodes[NODE_COUNT], r_nodes[NODE_COUNT];

static void reset_all(void)
{
	int i;

	// 0xcc fill leaves the link fields holding garbage, so the first
	// ClearLink has to overwrite both of them rather than relying on a
	// zeroed start.
	memset(&c_head, 0xcc, sizeof(c_head));
	memset(&r_head, 0xcc, sizeof(r_head));
	for (i = 0; i < NODE_COUNT; ++i) {
		memset(&c_nodes[i], 0xcc, sizeof(c_nodes[i]));
		memset(&r_nodes[i], 0xcc, sizeof(r_nodes[i]));
		c_nodes[i].id = r_nodes[i].id = i;
		c_nodes[i].canary_before = r_nodes[i].canary_before = CANARY;
		c_nodes[i].canary_after = r_nodes[i].canary_after = CANARY;
	}
	c_head.canary_before = r_head.canary_before = CANARY;
	c_head.canary_after = r_head.canary_after = CANARY;
}

// Index of the link a pointer refers to: -1 sentinel, 0..N-1 node, -2 NULL,
// -3 anything else (garbage or a stale pointer into another object).
static int link_index(const link_t *l, const head_t *head, const node_t *nodes)
{
	int i;

	if (l == NULL)
		return -2;
	if (l == &head->link)
		return -1;
	for (i = 0; i < NODE_COUNT; ++i)
		if (l == &nodes[i].link)
			return i;
	return -3;
}

typedef struct {
	int prev, next;
} link_shape_t;

static void fingerprint(link_shape_t *out, const head_t *head, const node_t *nodes)
{
	int i;

	out[0].prev = link_index(head->link.prev, head, nodes);
	out[0].next = link_index(head->link.next, head, nodes);
	for (i = 0; i < NODE_COUNT; ++i) {
		out[i + 1].prev = link_index(nodes[i].link.prev, head, nodes);
		out[i + 1].next = link_index(nodes[i].link.next, head, nodes);
	}
}

static int same_topology(const char *what)
{
	link_shape_t c[1 + NODE_COUNT], r[1 + NODE_COUNT];
	int i;

	fingerprint(c, &c_head, c_nodes);
	fingerprint(r, &r_head, r_nodes);
	for (i = 0; i <= NODE_COUNT; ++i) {
		CHECK(c[i].prev == r[i].prev && c[i].next == r[i].next,
			"%s: %s prev/next differ: C=(%d,%d) Rust=(%d,%d)",
			what, i == 0 ? "sentinel" : "node",
			c[i].prev, c[i].next, r[i].prev, r[i].next);
	}
	return 0;
}

static int canaries_intact(const char *what)
{
	int i;

	CHECK(c_head.canary_before == CANARY && c_head.canary_after == CANARY,
		"%s: C sentinel canary overwritten", what);
	CHECK(r_head.canary_before == CANARY && r_head.canary_after == CANARY,
		"%s: Rust sentinel canary overwritten", what);
	for (i = 0; i < NODE_COUNT; ++i) {
		CHECK(c_nodes[i].canary_before == CANARY && c_nodes[i].canary_after == CANARY,
			"%s: C node %d canary overwritten", what, i);
		CHECK(r_nodes[i].canary_before == CANARY && r_nodes[i].canary_after == CANARY,
			"%s: Rust node %d canary overwritten", what, i);
		CHECK(c_nodes[i].id == i && r_nodes[i].id == i,
			"%s: node %d payload changed (C=%d Rust=%d)",
			what, i, c_nodes[i].id, r_nodes[i].id);
	}
	return 0;
}

// Walk from the sentinel recovering the enclosing node through
// STRUCT_FROM_LINK -- the pattern world.c uses.  Returns the number of nodes,
// or -1 if the walk never came back to the sentinel (a malformed list, which
// both implementations must reproduce identically rather than repair).
static int walk(const head_t *head, const node_t *nodes, int backwards, int *ids, int max)
{
	const link_t *l = backwards ? head->link.prev : head->link.next;
	int n = 0;

	while (l != &head->link) {
		if (n >= max)
			return -1;
		ids[n++] = STRUCT_FROM_LINK(l, node_t, link)->id;
		l = backwards ? l->prev : l->next;
	}
	return n;
}

// Forward and backward walks agree between the two implementations; no
// expectation about the contents, for the deliberately malformed states.
static int same_walks(const char *what)
{
	int c_ids[NODE_COUNT + 1], r_ids[NODE_COUNT + 1];
	int dir;

	for (dir = 0; dir < 2; ++dir) {
		int c_n = walk(&c_head, c_nodes, dir, c_ids, NODE_COUNT + 1);
		int r_n = walk(&r_head, r_nodes, dir, r_ids, NODE_COUNT + 1);
		int i;

		CHECK(c_n == r_n, "%s: %s walk lengths differ: C=%d Rust=%d",
			what, dir ? "backward" : "forward", c_n, r_n);
		for (i = 0; i < c_n; ++i)
			CHECK(c_ids[i] == r_ids[i], "%s: %s walk[%d]: C=%d Rust=%d",
				what, dir ? "backward" : "forward", i, c_ids[i], r_ids[i]);
	}
	return 0;
}

// The walked order must be exactly `ids`, in both implementations and both
// directions.
static int expect_walks(const char *what, const int *ids, int count)
{
	int c_ids[NODE_COUNT + 1], r_ids[NODE_COUNT + 1];
	int c_n, r_n, i, j;

	for (i = 0; i < 2; ++i) {
		c_n = walk(&c_head, c_nodes, i, c_ids, NODE_COUNT + 1);
		r_n = walk(&r_head, r_nodes, i, r_ids, NODE_COUNT + 1);
		CHECK(c_n == count && r_n == count,
			"%s: %s walk reached C=%d Rust=%d nodes, expected %d",
			what, i ? "backward" : "forward", c_n, r_n, count);
		for (j = 0; j < count; ++j) {
			int expected = i ? ids[count - 1 - j] : ids[j];

			CHECK(c_ids[j] == expected, "%s: C %s walk[%d]=%d, expected %d",
				what, i ? "backward" : "forward", j, c_ids[j], expected);
			CHECK(r_ids[j] == expected, "%s: Rust %s walk[%d]=%d, expected %d",
				what, i ? "backward" : "forward", j, r_ids[j], expected);
		}
	}
	return 0;
}

// Every check in one place, so each scenario is one line per step.
static int same_state(const char *what)
{
	CHECK(same_topology(what) == 0, "%s: topology", what);
	CHECK(same_walks(what) == 0, "%s: walk", what);
	CHECK(canaries_intact(what) == 0, "%s: canaries", what);
	return 0;
}

static void clear_heads(void)
{
	c_ClearLink(&c_head.link);
	ClearLink(&r_head.link);
}

static int abi_checks(void)
{
	CHECK(sizeof(link_t) == LinkC_sizeof(),
		"link_t size differs: C=%zu Rust=%zu", sizeof(link_t), LinkC_sizeof());
	CHECK(_Alignof(link_t) == LinkC_alignof(),
		"link_t alignment differs: C=%zu Rust=%zu", _Alignof(link_t), LinkC_alignof());
	CHECK(offsetof(link_t, prev) == LinkC_offsetof_prev(),
		"prev offset differs: C=%zu Rust=%zu", offsetof(link_t, prev), LinkC_offsetof_prev());
	CHECK(offsetof(link_t, next) == LinkC_offsetof_next(),
		"next offset differs: C=%zu Rust=%zu", offsetof(link_t, next), LinkC_offsetof_next());
	CHECK(offsetof(link_t, prev) == 0, "prev is not the first member");
	CHECK(sizeof(link_t) == 2 * sizeof(void *), "link_t is not two pointers");
	return 0;
}

static int clear_checks(void)
{
	int i;

	reset_all();
	clear_heads();
	CHECK(same_state("ClearLink") == 0, "ClearLink");
	CHECK(c_head.link.prev == &c_head.link && c_head.link.next == &c_head.link,
		"C ClearLink did not leave the sentinel self-linked");
	CHECK(r_head.link.prev == &r_head.link && r_head.link.next == &r_head.link,
		"Rust ClearLink did not leave the sentinel self-linked");
	CHECK(expect_walks("empty list", NULL, 0) == 0, "empty list");

	// An empty list is a sentinel pointing at itself: removing it is a no-op
	// in both.  The second removal is only safe because the first one left
	// the sentinel's own pointers alone, so assert that in between rather
	// than segfaulting on a divergence.
	c_RemoveLink(&c_head.link);
	RemoveLink(&r_head.link);
	CHECK(same_state("RemoveLink on an empty list") == 0, "RemoveLink on an empty list");
	CHECK(c_head.link.prev == &c_head.link && r_head.link.prev == &r_head.link,
		"RemoveLink on an empty list did not leave the sentinel self-linked");
	c_RemoveLink(&c_head.link);
	RemoveLink(&r_head.link);
	CHECK(same_state("RemoveLink on an empty list, twice") == 0, "RemoveLink twice on an empty list");

	// A cleared node that is not in any list: removing it touches only
	// itself, and nothing else in memory.
	for (i = 0; i < NODE_COUNT; ++i) {
		c_ClearLink(&c_nodes[i].link);
		ClearLink(&r_nodes[i].link);
	}
	c_RemoveLink(&c_nodes[3].link);
	RemoveLink(&r_nodes[3].link);
	CHECK(same_state("RemoveLink on an unlinked node") == 0, "RemoveLink on an unlinked node");
	CHECK(c_nodes[3].link.prev == &c_nodes[3].link && c_nodes[3].link.next == &c_nodes[3].link,
		"C RemoveLink changed an unlinked node's own pointers");
	CHECK(r_nodes[3].link.prev == &r_nodes[3].link && r_nodes[3].link.next == &r_nodes[3].link,
		"Rust RemoveLink changed an unlinked node's own pointers");
	return 0;
}

static int single_checks(void)
{
	reset_all();
	clear_heads();
	c_InsertLinkBefore(&c_nodes[0].link, &c_head.link);
	InsertLinkBefore(&r_nodes[0].link, &r_head.link);
	CHECK(same_state("InsertLinkBefore into an empty list") == 0, "InsertLinkBefore");
	CHECK(c_head.link.next == &c_nodes[0].link && c_head.link.prev == &c_nodes[0].link,
		"C sentinel does not point at the only node");
	CHECK(r_head.link.next == &r_nodes[0].link && r_head.link.prev == &r_nodes[0].link,
		"Rust sentinel does not point at the only node");
	CHECK(c_nodes[0].link.next == &c_head.link && c_nodes[0].link.prev == &c_head.link,
		"C node does not point back at the sentinel");
	CHECK(r_nodes[0].link.next == &r_head.link && r_nodes[0].link.prev == &r_head.link,
		"Rust node does not point back at the sentinel");
	CHECK(expect_walks("single node", (const int[]){ 0 }, 1) == 0, "single node");

	// Removing the only node empties the list, and -- the invariant
	// SV_UnlinkEdict depends on -- leaves the node's own pointers stale,
	// still naming its former neighbours.
	c_RemoveLink(&c_nodes[0].link);
	RemoveLink(&r_nodes[0].link);
	CHECK(same_state("RemoveLink of the only node") == 0, "RemoveLink of the only node");
	CHECK(expect_walks("emptied list", NULL, 0) == 0, "emptied list");
	CHECK(c_nodes[0].link.next == &c_head.link && c_nodes[0].link.prev == &c_head.link,
		"C RemoveLink cleared the removed node's own pointers");
	CHECK(r_nodes[0].link.next == &r_head.link && r_nodes[0].link.prev == &r_head.link,
		"Rust RemoveLink cleared the removed node's own pointers");

	// Re-linking the removed node must be indistinguishable from a fresh
	// insert.
	c_InsertLinkBefore(&c_nodes[0].link, &c_head.link);
	InsertLinkBefore(&r_nodes[0].link, &r_head.link);
	CHECK(same_state("re-insert after removal") == 0, "re-insert after removal");
	CHECK(expect_walks("re-inserted single node", (const int[]){ 0 }, 1) == 0, "re-inserted");
	return 0;
}

static int multi_checks(void)
{
	int order[NODE_COUNT];
	int i;

	// Tail append: InsertLinkBefore(l, sentinel) puts l last.
	reset_all();
	clear_heads();
	for (i = 0; i < NODE_COUNT; ++i) {
		c_InsertLinkBefore(&c_nodes[i].link, &c_head.link);
		InsertLinkBefore(&r_nodes[i].link, &r_head.link);
		order[i] = i;
	}
	CHECK(same_state("InsertLinkBefore x8") == 0, "InsertLinkBefore x8");
	CHECK(expect_walks("tail-appended list", order, NODE_COUNT) == 0, "tail-appended list");
	CHECK(c_head.link.prev == &c_nodes[NODE_COUNT - 1].link
		&& r_head.link.prev == &r_nodes[NODE_COUNT - 1].link,
		"the sentinel's prev is not the last inserted node");

	// Head insert: InsertLinkAfter(l, sentinel) puts l first, so the walk is
	// the reverse of the insertion order.
	reset_all();
	clear_heads();
	for (i = 0; i < NODE_COUNT; ++i) {
		c_InsertLinkAfter(&c_nodes[i].link, &c_head.link);
		InsertLinkAfter(&r_nodes[i].link, &r_head.link);
	}
	for (i = 0; i < NODE_COUNT; ++i)
		order[i] = NODE_COUNT - 1 - i;
	CHECK(same_state("InsertLinkAfter x8") == 0, "InsertLinkAfter x8");
	CHECK(expect_walks("head-inserted list", order, NODE_COUNT) == 0, "head-inserted list");

	// Insert before and after a node in the middle of an existing list.
	reset_all();
	clear_heads();
	for (i = 0; i < 4; ++i) {
		c_InsertLinkBefore(&c_nodes[i].link, &c_head.link);
		InsertLinkBefore(&r_nodes[i].link, &r_head.link);
	}
	c_InsertLinkBefore(&c_nodes[4].link, &c_nodes[2].link);
	InsertLinkBefore(&r_nodes[4].link, &r_nodes[2].link);
	CHECK(same_state("InsertLinkBefore a middle node") == 0, "InsertLinkBefore middle");
	CHECK(expect_walks("middle insert", (const int[]){ 0, 1, 4, 2, 3 }, 5) == 0, "middle insert");

	c_RemoveLink(&c_nodes[4].link);
	RemoveLink(&r_nodes[4].link);
	c_InsertLinkAfter(&c_nodes[4].link, &c_nodes[1].link);
	InsertLinkAfter(&r_nodes[4].link, &r_nodes[1].link);
	CHECK(same_state("InsertLinkAfter a middle node") == 0, "InsertLinkAfter middle");
	CHECK(expect_walks("middle insert after", (const int[]){ 0, 1, 4, 2, 3 }, 5) == 0, "middle after");

	// Insert before the tail node: l->prev is the sentinel there.
	c_RemoveLink(&c_nodes[4].link);
	RemoveLink(&r_nodes[4].link);
	c_InsertLinkBefore(&c_nodes[4].link, &c_nodes[3].link);
	InsertLinkBefore(&r_nodes[4].link, &r_nodes[3].link);
	CHECK(same_state("InsertLinkBefore the tail node") == 0, "insert before tail");
	CHECK(expect_walks("before tail", (const int[]){ 0, 1, 2, 4, 3 }, 5) == 0, "before tail");
	return 0;
}

static int unlink_relink_checks(void)
{
	int i;

	reset_all();
	clear_heads();
	for (i = 0; i < NODE_COUNT; ++i) {
		c_InsertLinkBefore(&c_nodes[i].link, &c_head.link);
		InsertLinkBefore(&r_nodes[i].link, &r_head.link);
	}

	// Unlink from the middle.
	c_RemoveLink(&c_nodes[3].link);
	RemoveLink(&r_nodes[3].link);
	CHECK(same_state("unlink the middle node") == 0, "unlink middle");
	CHECK(expect_walks("after unlinking the middle",
		(const int[]){ 0, 1, 2, 4, 5, 6, 7 }, 7) == 0, "after unlink middle");
	CHECK(c_nodes[3].link.prev == &c_nodes[2].link && c_nodes[3].link.next == &c_nodes[4].link,
		"C removed node does not still name its former neighbours");
	CHECK(r_nodes[3].link.prev == &r_nodes[2].link && r_nodes[3].link.next == &r_nodes[4].link,
		"Rust removed node does not still name its former neighbours");

	// Relink it elsewhere.
	c_InsertLinkBefore(&c_nodes[3].link, &c_nodes[6].link);
	InsertLinkBefore(&r_nodes[3].link, &r_nodes[6].link);
	CHECK(same_state("relink the middle node") == 0, "relink middle");
	CHECK(expect_walks("after relinking the middle",
		(const int[]){ 0, 1, 2, 4, 5, 3, 6, 7 }, 8) == 0, "after relink middle");

	// Unlink the first and the last node.
	c_RemoveLink(&c_nodes[0].link);
	RemoveLink(&r_nodes[0].link);
	c_RemoveLink(&c_nodes[7].link);
	RemoveLink(&r_nodes[7].link);
	CHECK(same_state("unlink the ends") == 0, "unlink ends");
	CHECK(expect_walks("after unlinking the ends",
		(const int[]){ 1, 2, 4, 5, 3, 6 }, 6) == 0, "after unlink ends");

	// Relink the first node at the front and the last node at the tail.
	c_InsertLinkAfter(&c_nodes[0].link, &c_head.link);
	InsertLinkAfter(&r_nodes[0].link, &r_head.link);
	c_InsertLinkBefore(&c_nodes[7].link, &c_head.link);
	InsertLinkBefore(&r_nodes[7].link, &r_head.link);
	CHECK(same_state("relink at the ends") == 0, "relink ends");
	CHECK(expect_walks("after relinking the ends",
		(const int[]){ 0, 1, 2, 4, 5, 3, 6, 7 }, 8) == 0, "after relink ends");

	// Removing the same node twice: the second call runs on the stale
	// pointers and must be the same no-op in both.  same_state first, so a
	// divergence is a diagnostic rather than a fault on the second call.
	c_RemoveLink(&c_nodes[5].link);
	RemoveLink(&r_nodes[5].link);
	CHECK(same_state("unlink node 5") == 0, "unlink node 5");
	CHECK(expect_walks("unlink node 5", (const int[]){ 0, 1, 2, 4, 3, 6, 7 }, 7) == 0, "unlink 5");
	c_RemoveLink(&c_nodes[5].link);
	RemoveLink(&r_nodes[5].link);
	CHECK(same_state("RemoveLink twice") == 0, "RemoveLink twice");
	CHECK(expect_walks("after the second RemoveLink",
		(const int[]){ 0, 1, 2, 4, 3, 6, 7 }, 7) == 0, "after second remove");
	return 0;
}

static int duplicate_absent_checks(void)
{
	int i;

	// The same link inserted twice.  This is not a list the engine builds,
	// but a caller can do it, and the C original's answer is the contract:
	// the sentinel still points at the node, and the node ends up pointing
	// at itself, so the forward walk never returns.  Reproducing that
	// malformed state exactly -- rather than silently repairing it -- is the
	// property under test.
	reset_all();
	clear_heads();
	c_InsertLinkBefore(&c_nodes[0].link, &c_head.link);
	InsertLinkBefore(&r_nodes[0].link, &r_head.link);
	c_InsertLinkBefore(&c_nodes[0].link, &c_head.link);
	InsertLinkBefore(&r_nodes[0].link, &r_head.link);
	CHECK(same_state("duplicate insert") == 0, "duplicate insert");
	CHECK(c_head.link.next == &c_nodes[0].link && c_head.link.prev == &c_nodes[0].link,
		"C sentinel lost the duplicated node");
	CHECK(r_head.link.next == &r_nodes[0].link && r_head.link.prev == &r_nodes[0].link,
		"Rust sentinel lost the duplicated node");
	CHECK(c_nodes[0].link.prev == &c_nodes[0].link && c_nodes[0].link.next == &c_nodes[0].link,
		"C duplicate insert did not self-link the node");
	CHECK(r_nodes[0].link.prev == &r_nodes[0].link && r_nodes[0].link.next == &r_nodes[0].link,
		"Rust duplicate insert did not self-link the node");
	CHECK(walk(&c_head, c_nodes, 0, (int[NODE_COUNT + 1]){ 0 }, NODE_COUNT + 1) == -1,
		"C walk of a duplicated list unexpectedly terminated");
	CHECK(walk(&r_head, r_nodes, 0, (int[NODE_COUNT + 1]){ 0 }, NODE_COUNT + 1) == -1,
		"Rust walk of a duplicated list unexpectedly terminated");

	// ... and removing it once from that state is the same no-op in both.
	c_RemoveLink(&c_nodes[0].link);
	RemoveLink(&r_nodes[0].link);
	CHECK(same_state("remove a duplicated node") == 0, "remove duplicated node");

	// A well-formed list with an absent node queried and removed alongside
	// it: the list itself must be untouched by work on the absent node.
	reset_all();
	clear_heads();
	for (i = 0; i < 3; ++i) {
		c_InsertLinkBefore(&c_nodes[i].link, &c_head.link);
		InsertLinkBefore(&r_nodes[i].link, &r_head.link);
	}
	c_ClearLink(&c_nodes[5].link);
	ClearLink(&r_nodes[5].link);
	c_RemoveLink(&c_nodes[5].link);
	RemoveLink(&r_nodes[5].link);
	CHECK(same_state("absent node") == 0, "absent node");
	CHECK(expect_walks("list after an absent node was removed",
		(const int[]){ 0, 1, 2 }, 3) == 0, "list after absent remove");
	CHECK(c_nodes[5].link.prev == &c_nodes[5].link && r_nodes[5].link.prev == &r_nodes[5].link,
		"the absent node was linked into the list");

	// A node that was never linked and whose pointers are NULL is what the
	// engine guards with `if (!ent->area.prev) return;`; both
	// implementations would fault on it identically, so it is deliberately
	// not exercised here.
	return 0;
}

static int alias_checks(void)
{
	int i;

	// l == before / l == after: well defined in C, and the order of the
	// assignments decides the result, so the Rust has to keep it.
	reset_all();
	clear_heads();
	c_ClearLink(&c_nodes[0].link);
	ClearLink(&r_nodes[0].link);
	c_ClearLink(&c_nodes[1].link);
	ClearLink(&r_nodes[1].link);
	c_InsertLinkBefore(&c_nodes[0].link, &c_nodes[0].link);
	InsertLinkBefore(&r_nodes[0].link, &r_nodes[0].link);
	CHECK(same_state("InsertLinkBefore(l, l)") == 0, "InsertLinkBefore(l, l)");
	CHECK(c_nodes[0].link.prev == &c_nodes[0].link && c_nodes[0].link.next == &c_nodes[0].link,
		"C InsertLinkBefore(l, l) did not self-link");
	CHECK(r_nodes[0].link.prev == &r_nodes[0].link && r_nodes[0].link.next == &r_nodes[0].link,
		"Rust InsertLinkBefore(l, l) did not self-link");
	CHECK(c_head.link.prev == &c_head.link && r_head.link.prev == &r_head.link,
		"InsertLinkBefore(l, l) touched the sentinel");

	c_InsertLinkAfter(&c_nodes[1].link, &c_nodes[1].link);
	InsertLinkAfter(&r_nodes[1].link, &r_nodes[1].link);
	CHECK(same_state("InsertLinkAfter(l, l)") == 0, "InsertLinkAfter(l, l)");

	// Inserting a link before the node it already precedes.  The C original
	// corrupts the list here (l->prev == l, so `l->prev->next = l` rewrites
	// l->next); the point is that Rust corrupts it identically.
	reset_all();
	clear_heads();
	for (i = 0; i < 3; ++i) {
		c_InsertLinkBefore(&c_nodes[i].link, &c_head.link);
		InsertLinkBefore(&r_nodes[i].link, &r_head.link);
	}
	c_InsertLinkBefore(&c_nodes[1].link, &c_nodes[2].link);
	InsertLinkBefore(&r_nodes[1].link, &r_nodes[2].link);
	CHECK(same_state("InsertLinkBefore(l, l->next)") == 0, "InsertLinkBefore(l, l->next)");
	CHECK(c_nodes[1].link.next == &c_nodes[1].link && r_nodes[1].link.next == &r_nodes[1].link,
		"the aliased insert did not rewrite l->next in both implementations");

	// ... and the aliased InsertLinkAfter of a link that already follows its
	// anchor.
	reset_all();
	clear_heads();
	for (i = 0; i < 3; ++i) {
		c_InsertLinkBefore(&c_nodes[i].link, &c_head.link);
		InsertLinkBefore(&r_nodes[i].link, &r_head.link);
	}
	c_InsertLinkAfter(&c_nodes[2].link, &c_nodes[1].link);
	InsertLinkAfter(&r_nodes[2].link, &r_nodes[1].link);
	CHECK(same_state("InsertLinkAfter(l, l->prev)") == 0, "InsertLinkAfter(l, l->prev)");
	CHECK(c_nodes[2].link.prev == &c_nodes[2].link && r_nodes[2].link.prev == &r_nodes[2].link,
		"the aliased insert did not rewrite l->prev in both implementations");
	return 0;
}

int main(void)
{
	if (abi_checks() || clear_checks() || single_checks() || multi_checks()
		|| unlink_relink_checks() || duplicate_absent_checks() || alias_checks())
		return 1;
	puts("link_ops differential harness: PASS");
	return 0;
}
