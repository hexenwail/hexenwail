# CompileSpirv.cmake -- compile the expanded shader sources that
# EmbedShaders.cmake drops into VK_DIR down to SPIR-V, once per target dialect.
#
#   cmake -DVK_DIR=<dir> -DSPV_DIR=<dir> -DGLSLANG=<glslangValidator> \
#         -P CompileSpirv.cmake
#
# Nothing in the engine consumes the .spv yet; the SDL_GPU backend is what
# will.  Until then this is a correctness check with a byproduct: Vulkan
# rejects a uniform, sampler or varying that has no explicit binding or
# location, so a shader that grows a resource and forgets to qualify it fails
# here rather than in the backend.  That is why it runs BOTH frontends and not
# just the Vulkan one -- -G and -V disagree about whether a uniform block may
# omit its binding, and only compiling both catches a layout.inc arm that has
# gone stale.
#
# Do not add -DVULKAN: glslang predefines it, and redefining it is an error.

if(NOT VK_DIR OR NOT SPV_DIR OR NOT GLSLANG)
    message(FATAL_ERROR "CompileSpirv: VK_DIR, SPV_DIR and GLSLANG are required")
endif()

file(MAKE_DIRECTORY "${SPV_DIR}")
file(GLOB VK_SOURCES "${VK_DIR}/*.vert" "${VK_DIR}/*.frag" "${VK_DIR}/*.comp")
list(SORT VK_SOURCES)

if(NOT VK_SOURCES)
    message(FATAL_ERROR "CompileSpirv: no shader sources in ${VK_DIR}")
endif()

set(failed 0)
set(total 0)
foreach(src ${VK_SOURCES})
    get_filename_component(base "${src}" NAME)
    foreach(dialect vulkan opengl)
        if(dialect STREQUAL "vulkan")
            set(args -V --target-env vulkan1.0)
            set(suffix "vk.spv")
        else()
            set(args -G)
            set(suffix "gl.spv")
        endif()
        execute_process(
            COMMAND "${GLSLANG}" ${args} -o "${SPV_DIR}/${base}.${suffix}" "${src}"
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE err)
        math(EXPR total "${total} + 1")
        if(NOT rc EQUAL 0)
            message(SEND_ERROR "SPIR-V (${dialect}) failed for ${base}:\n${out}${err}")
            math(EXPR failed "${failed} + 1")
        endif()
    endforeach()
endforeach()

if(failed EQUAL 0)
    message(STATUS "CompileSpirv: ${total} compiles -> SPIR-V")
else()
    message(FATAL_ERROR "CompileSpirv: ${failed} of ${total} compiles failed")
endif()
