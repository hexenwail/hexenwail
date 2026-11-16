/* cull_clear -- reset every texture bucket's indirect draw command before
 * cull_mark refills it: count 0, one instance.  One invocation per bucket.
 *
 * No #version line: gl_worldcull.c passes HIZ_COMPUTE_VERSION as a separate
 * glShaderSource chunk, and EmbedShaders.cmake prepends its own for the
 * offline SPIR-V compile.  Moved from gl_worldcull.c's clear_indirect_src
 * (#450).
 */
#include "cull.inc"

layout(local_size_x = 64) in;

struct IndirectCmd {
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

SSBO_RW(3, 0) buffer IndirectBuffer {
    IndirectCmd cmds[];
};

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uint(u_ccounts0.z)) return;
    cmds[id].count = 0u;
    cmds[id].instanceCount = 1u;
}
