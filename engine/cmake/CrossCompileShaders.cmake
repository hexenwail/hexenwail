# CrossCompileShaders.cmake -- cross-compile the Vulkan SPIR-V that
# CompileSpirv.cmake produces down to the per-driver formats SDL_GPU needs:
# DXIL for D3D12 and MSL for Metal, plus a reflection JSON per stage.
#
#   cmake -DSPV_DIR=<dir> -DCROSS_DIR=<dir> -DSHADERCROSS=<shadercross> \
#         -P CrossCompileShaders.cmake
#
# Nothing in the engine consumes the .dxil/.msl yet; the SDL_GPU backend is
# what will.  Until then this is a correctness check with a byproduct: the
# reflection JSON records exactly which descriptor sets each stage uses, so a
# shader that declares a sampler in the wrong set for its stage fails here
# rather than in the backend -- that is what caught the composite fragment
# shader's compute numbering (GitHub #448).
#
# Only the Vulkan dialect is cross-compiled.  The OpenGL SPIR-V is a check
# artifact; DXIL and MSL must come from the Vulkan SPIR-V, which is the one
# that carries descriptor sets.  MSL is left at shadercross's default version
# (1.2.0).  DXBC and metallib are out of scope (#448): DXBC is the legacy
# D3D11 format and metallib needs the Metal toolchain.
#
# Do not pass -c/--cull: culling changes the reflection output, and the JSON
# is supposed to describe the shader as the backend will see it.

if(NOT SPV_DIR OR NOT CROSS_DIR OR NOT SHADERCROSS)
    message(FATAL_ERROR "CrossCompileShaders: SPV_DIR, CROSS_DIR and SHADERCROSS are required")
endif()

file(MAKE_DIRECTORY "${CROSS_DIR}")
file(GLOB VK_SPV "${SPV_DIR}/*.vk.spv")
list(SORT VK_SPV)

if(NOT VK_SPV)
    message(FATAL_ERROR "CrossCompileShaders: no Vulkan SPIR-V in ${SPV_DIR}")
endif()

set(failed 0)
set(total 0)
foreach(spv ${VK_SPV})
    get_filename_component(name "${spv}" NAME)
    string(REGEX REPLACE "\\.vk\\.spv$" "" base "${name}")
    if(base MATCHES "\\.vert$")
        set(stage vertex)
    elseif(base MATCHES "\\.frag$")
        set(stage fragment)
    elseif(base MATCHES "\\.comp$")
        set(stage compute)
    else()
        message(SEND_ERROR "CrossCompileShaders: cannot infer stage for ${name}")
        math(EXPR failed "${failed} + 1")
        continue()
    endif()
    foreach(dest DXIL MSL JSON)
        string(TOLOWER "${dest}" ext)
        execute_process(
            COMMAND "${SHADERCROSS}" "${spv}"
                    -s SPIRV -d ${dest} -t ${stage} -e main
                    -o "${CROSS_DIR}/${base}.${ext}"
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE err)
        math(EXPR total "${total} + 1")
        if(NOT rc EQUAL 0)
            message(SEND_ERROR "Cross-compile (${dest}) failed for ${name}:\n${out}${err}")
            math(EXPR failed "${failed} + 1")
        endif()
    endforeach()
endforeach()

if(failed EQUAL 0)
    message(STATUS "CrossCompileShaders: ${total} cross-compiles -> DXIL/MSL/JSON")
else()
    message(FATAL_ERROR "CrossCompileShaders: ${failed} of ${total} cross-compiles failed")
endif()
