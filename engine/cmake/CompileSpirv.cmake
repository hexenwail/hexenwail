# CompileSpirv.cmake -- compile the Vulkan-flavoured shader sources that
# EmbedShaders.cmake drops into VK_DIR down to SPIR-V.
#
#   cmake -DVK_DIR=<dir> -DSPV_DIR=<dir> -DGLSLANG=<glslangValidator> \
#         -P CompileSpirv.cmake
#
# Nothing in the engine consumes the .spv yet; the SDL_GPU backend
# (uhexen2-p4ln.5) is what will.  Until then this is a correctness check with a
# byproduct: Vulkan rejects a uniform block, sampler or varying that has no
# explicit binding or location, so a shader that grows a resource and forgets
# to qualify it fails here rather than in the backend.  uhexen2-p4ln.4.

if(NOT VK_DIR OR NOT SPV_DIR OR NOT GLSLANG)
    message(FATAL_ERROR "CompileSpirv: VK_DIR, SPV_DIR and GLSLANG are required")
endif()

file(MAKE_DIRECTORY "${SPV_DIR}")
file(GLOB VK_SOURCES "${VK_DIR}/*.vert" "${VK_DIR}/*.frag")
list(SORT VK_SOURCES)

set(failed 0)
foreach(src ${VK_SOURCES})
    get_filename_component(base "${src}" NAME)
    execute_process(
        COMMAND "${GLSLANG}" -V --target-env vulkan1.0
                -o "${SPV_DIR}/${base}.spv" "${src}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(SEND_ERROR "SPIR-V compile failed for ${base}:\n${out}${err}")
        math(EXPR failed "${failed} + 1")
    endif()
endforeach()

list(LENGTH VK_SOURCES total)
if(failed EQUAL 0)
    message(STATUS "CompileSpirv: ${total} shaders -> SPIR-V")
else()
    message(FATAL_ERROR "CompileSpirv: ${failed} of ${total} shaders failed")
endif()
