cmake_minimum_required(VERSION 3.25)
project(rfgrt VERSION 0.1.18 LANGUAGES C CXX)

# One source tree, two architectures (configure each in its own build folder, tools\build.ps1 does both):
#   Win32 (build-x86): rfgrt.asi for today's 32-bit rfg.exe; its ray tracing core runs in rfgrt_host.exe.
#   x64   (build-x64): rtcore and rfgrt_host.exe; and rfgrt.asi for a 64-bit rfg.exe (the 64-bit port), with the core
#                      on a thread of the game's own process (RFGRT_LOCAL_CORE).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(RFGRT_X64 ON)
else()
    set(RFGRT_X64 OFF)
endif()

# Static CRT: no redistributable needed, and no sharing of the game's MSVCR100 heap.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shader compilers from the Windows SDK: fxc for the plugin's D3D11 shaders (SM 5.0), dxc for the core's D3D12 ones.
file(GLOB _sdk_bins "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT _sdk_bins COMPARE NATURAL ORDER DESCENDING)
find_program(RFGRT_FXC fxc PATHS ${_sdk_bins} NO_DEFAULT_PATH REQUIRED)
find_program(RFGRT_DXC dxc PATHS ${_sdk_bins} NO_DEFAULT_PATH REQUIRED)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

# name profile source -> ${SHADER_OUT}/name.h with g_name[]
function(fxc_shader out_var name profile source)
    set(out ${SHADER_OUT}/${name}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${RFGRT_FXC} /nologo /T ${profile} /E main /O3 /Fh ${out} /Vn g_${name} ${source}
        DEPENDS ${source} COMMENT "fxc ${name}" VERBATIM)
    set(${out_var} ${${out_var}} ${out} PARENT_SCOPE)
endfunction()
file(GLOB CORE_SHADER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/*.hlsli)
function(dxc_shader out_var name profile source)
    set(out ${SHADER_OUT}/${name}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${RFGRT_DXC} -nologo -T ${profile} -E main -O3 -Fh ${out} -Vn g_${name} ${source}
        DEPENDS ${source} ${CORE_SHADER_INCLUDES} COMMENT "dxc ${name}" VERBATIM)
    set(${out_var} ${${out_var}} ${out} PARENT_SCOPE)
endfunction()

# --- MinHook ---------------------------------------------------------------------------------------------------------
if(RFGRT_X64)
    set(_hde third_party/minhook/src/hde/hde64.c)
else()
    set(_hde third_party/minhook/src/hde/hde32.c)
endif()
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    ${_hde})
target_include_directories(minhook PUBLIC third_party/minhook/include)

# --- the ray tracing core (64-bit only: 32-bit processes get no hardware ray tracing) --------------------------------
if(RFGRT_X64)
    set(CORE_SHADERS)
    dxc_shader(CORE_SHADERS depth_pattern_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/depth_pattern.hlsl)
    dxc_shader(CORE_SHADERS depth_compare_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/depth_compare.hlsl)
    dxc_shader(CORE_SHADERS sun_shadow_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/sun_shadow.hlsl)
    dxc_shader(CORE_SHADERS sun_shadow_filter_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/sun_shadow_filter.hlsl)
    dxc_shader(CORE_SHADERS sun_shadow_temporal_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/sun_shadow_temporal.hlsl)
    dxc_shader(CORE_SHADERS skin_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/skin.hlsl)
    dxc_shader(CORE_SHADERS gi_trace_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/gi_trace.hlsl)
    dxc_shader(CORE_SHADERS gi_temporal_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/gi_temporal.hlsl)
    dxc_shader(CORE_SHADERS gi_filter_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/gi_filter.hlsl)
    dxc_shader(CORE_SHADERS gi_atrous_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/gi_atrous.hlsl)
    dxc_shader(CORE_SHADERS light_shadow_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/light_shadow.hlsl)
    dxc_shader(CORE_SHADERS reflect_trace_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/reflect_trace.hlsl)
    dxc_shader(CORE_SHADERS cache_update_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/cache_update.hlsl)
    dxc_shader(CORE_SHADERS cache_resolve_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/cache_resolve.hlsl)
    dxc_shader(CORE_SHADERS sky_update_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/sky_update.hlsl)
    dxc_shader(CORE_SHADERS sky_resolve_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/sky_resolve.hlsl)
    dxc_shader(CORE_SHADERS probe_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/probe.hlsl)
    dxc_shader(CORE_SHADERS rr_guides_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/rr_guides.hlsl)
    dxc_shader(CORE_SHADERS rr_finalize_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/rr_finalize.hlsl)
    dxc_shader(CORE_SHADERS glass_light_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/glass_light.hlsl)
    dxc_shader(CORE_SHADERS glass_filter_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/glass_filter.hlsl)
    dxc_shader(CORE_SHADERS sun_tint_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/sun_tint.hlsl)
    dxc_shader(CORE_SHADERS fog_inject_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/fog_inject.hlsl)
    dxc_shader(CORE_SHADERS fog_integrate_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/fog_integrate.hlsl)
    dxc_shader(CORE_SHADERS fog_apply_cs cs_6_5 ${CMAKE_CURRENT_SOURCE_DIR}/src/rtcore/shaders/fog_apply.hlsl)
    add_library(rtcore STATIC src/rtcore/rtcore.cpp src/rtcore/scene.cpp src/rtcore/tlas.cpp src/rtcore/gpu_pool.cpp src/rtcore/skinning.cpp src/rtcore/dlss.cpp ${CORE_SHADERS})
    target_include_directories(rtcore PUBLIC src/rtcore src/shared PRIVATE ${SHADER_OUT})
    target_compile_definitions(rtcore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(rtcore PRIVATE /W4 /permissive- /utf-8 /Zi)
    target_link_libraries(rtcore PUBLIC d3d12 dxgi)

    add_executable(rfgrt_host host/main.cpp)
    target_link_libraries(rfgrt_host PRIVATE rtcore)
    target_compile_definitions(rfgrt_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(rfgrt_host PRIVATE /W4 /permissive- /utf-8 /Zi)
    target_link_options(rfgrt_host PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)

    # DLSS Ray Reconstruction (GI.Denoiser=2): NVIDIA's DLSS SDK in third_party/dlss, fetched by tools/fetch_dlss.ps1
    # (out of git). Without it the core builds without Ray Reconstruction and the GI keeps its own denoiser.
    set(RFGRT_DLSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dlss")
    set(RFGRT_DLSS_RUNTIME "${RFGRT_DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")
    if(EXISTS "${RFGRT_DLSS_DIR}/include/nvsdk_ngx.h" AND EXISTS "${RFGRT_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
       AND EXISTS "${RFGRT_DLSS_RUNTIME}")
        message(STATUS "DLSS SDK found: the core has Ray Reconstruction")
        target_include_directories(rtcore SYSTEM PRIVATE "${RFGRT_DLSS_DIR}/include")
        target_compile_definitions(rtcore PRIVATE RFGRT_DLSS=1)
        target_link_libraries(rtcore PUBLIC "${RFGRT_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
        add_custom_command(TARGET rfgrt_host POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${RFGRT_DLSS_RUNTIME}" "$<TARGET_FILE_DIR:rfgrt_host>/nvngx_dlssd.dll")
    else()
        message(STATUS "DLSS SDK not found (tools/fetch_dlss.ps1): the core without Ray Reconstruction")
    endif()

    # The shader patcher's offline check on the game's extracted shaders (tools/shaderpatch/main.cpp).
    add_executable(shaderpatch tools/shaderpatch/main.cpp src/rfgrt/patch/dxbc.cpp src/rfgrt/patch/ambient.cpp
        src/rfgrt/patch/light_pass.cpp src/rfgrt/patch/shadow_floor.cpp src/rfgrt/patch/glass.cpp
        src/rfgrt/patch/spec_occlusion.cpp src/rfgrt/patch/fog.cpp src/rfgrt/patch/sun_tint.cpp)
    target_include_directories(shaderpatch PRIVATE src/rfgrt)
    target_compile_definitions(shaderpatch PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(shaderpatch PRIVATE /W4 /permissive- /utf-8 /Zi)
    target_link_libraries(shaderpatch PRIVATE d3d11 d3dcompiler)

    # OpenRFG64's names table, for a game that builds the plugin and writes the table beside it after each link
    # (tools/names_table/main.cpp, src/shared/openrfg64_column.h): rfgrt_names <OpenRFG.exe> <rfgrt.names>.
    add_executable(rfgrt_names tools/names_table/main.cpp src/shared/openrfg64_column.cpp)
    target_include_directories(rfgrt_names PRIVATE src/rfgrt src/shared)
    target_compile_definitions(rfgrt_names PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(rfgrt_names PRIVATE /W4 /permissive- /utf-8)
endif()

# --- the plugin --------------------------------------------------------------------------------------------------------
set(PLUGIN_SHADERS)
fxc_shader(PLUGIN_SHADERS fullscreen_vs vs_5_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/shaders/fullscreen_vs.hlsl)
fxc_shader(PLUGIN_SHADERS pattern_ps ps_5_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/shaders/pattern_ps.hlsl)
fxc_shader(PLUGIN_SHADERS copy_depth_ps ps_5_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/shaders/copy_depth_ps.hlsl)
fxc_shader(PLUGIN_SHADERS apply_result_ps ps_5_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/shaders/apply_result_ps.hlsl)
fxc_shader(PLUGIN_SHADERS debug_view_ps ps_5_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/shaders/debug_view_ps.hlsl)
fxc_shader(PLUGIN_SHADERS radiance_view_ps ps_5_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/shaders/radiance_view_ps.hlsl)
fxc_shader(PLUGIN_SHADERS fog_composite_ps ps_5_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/shaders/fog_composite_ps.hlsl)

add_library(rfgrt SHARED
    src/rfgrt/main.cpp
    src/rfgrt/core/log.cpp
    src/rfgrt/core/config.cpp
    src/rfgrt/core/session.cpp
    src/rfgrt/core/build_check.cpp
    src/rfgrt/core/active.cpp
    src/rfgrt/core/gameloop.cpp
    src/rfgrt/game/rfg.cpp
    src/rfgrt/game/openrfg64.cpp
    src/shared/openrfg64_column.cpp
    src/rfgrt/hooks/hooks.cpp
    src/rfgrt/harness/screenshot.cpp
    src/rfgrt/harness/shader_dump.cpp
    src/rfgrt/test/automation.cpp
    src/rfgrt/test/input_script.cpp
    src/rfgrt/render/d3d_state.cpp
    src/rfgrt/render/seam.cpp
    src/rfgrt/render/gi.cpp
    src/rfgrt/render/lights.cpp
    src/rfgrt/render/sky_particles.cpp
    src/rfgrt/rt/link.cpp
    src/rfgrt/mirror/geometry.cpp
    src/rfgrt/mirror/geometry_census.cpp
    src/rfgrt/mirror/instances.cpp
    src/rfgrt/mirror/textures.cpp
    src/rfgrt/patch/dxbc.cpp
    src/rfgrt/patch/ambient.cpp
    src/rfgrt/patch/light_pass.cpp
    src/rfgrt/patch/shadow_floor.cpp
    src/rfgrt/patch/glass.cpp
    src/rfgrt/patch/sun_tint.cpp
    src/rfgrt/patch/spec_occlusion.cpp
    src/rfgrt/patch/fog.cpp
    src/rfgrt/patch/eye_adaption.cpp
    src/rfgrt/present/presenter.cpp
    src/rfgrt/present/framegen.cpp
    ${PLUGIN_SHADERS})
set_target_properties(rfgrt PROPERTIES SUFFIX ".asi")
target_include_directories(rfgrt PRIVATE src/rfgrt src/shared ${SHADER_OUT})
target_compile_definitions(rfgrt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE RFGRT_VERSION="${PROJECT_VERSION}")
target_compile_options(rfgrt PRIVATE /W4 /permissive- /utf-8 /Zi)
target_link_options(rfgrt PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO)
target_link_libraries(rfgrt PRIVATE minhook d3d11 dxgi)
if(RFGRT_X64)
    target_compile_definitions(rfgrt PRIVATE RFGRT_LOCAL_CORE)
    target_link_libraries(rfgrt PRIVATE rtcore d3d12)  # (d3d12: the presenter's own device, present/presenter.cpp)
endif()

# DLSS Frame Generation (the presenter, x64 only: Streamline is 64-bit): NVIDIA's Streamline SDK in
# third_party/streamline, out of git, fetched by tools/fetch_streamline.ps1. Its runtime DLLs go next to the plugin,
# which loads them from there; without them the plugin builds with frame generation unavailable.
set(RFGRT_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline")
set(RFGRT_STREAMLINE_RUNTIMES sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)
set(RFGRT_STREAMLINE_FOUND ${RFGRT_X64})
if(NOT EXISTS "${RFGRT_STREAMLINE_DIR}/include/sl.h")
    set(RFGRT_STREAMLINE_FOUND FALSE)
endif()
foreach(runtime IN LISTS RFGRT_STREAMLINE_RUNTIMES)
    if(NOT EXISTS "${RFGRT_STREAMLINE_DIR}/bin/x64/${runtime}")
        set(RFGRT_STREAMLINE_FOUND FALSE)
    endif()
endforeach()
if(RFGRT_STREAMLINE_FOUND)
    message(STATUS "Streamline SDK found: the plugin has DLSS Frame Generation")
    target_include_directories(rfgrt SYSTEM PRIVATE "${RFGRT_STREAMLINE_DIR}/include")
    target_compile_definitions(rfgrt PRIVATE RFGRT_STREAMLINE=1)
    # the presenter's two passes (present/presenter.cpp): the motion from the depth, the UI layer
    set(PRESENT_SHADERS)
    dxc_shader(PRESENT_SHADERS fg_motion_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/present/fg_motion.hlsl)
    dxc_shader(PRESENT_SHADERS fg_ui_cs cs_6_0 ${CMAKE_CURRENT_SOURCE_DIR}/src/rfgrt/present/fg_ui.hlsl)
    target_sources(rfgrt PRIVATE ${PRESENT_SHADERS})
    foreach(runtime IN LISTS RFGRT_STREAMLINE_RUNTIMES)
        add_custom_command(TARGET rfgrt POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${RFGRT_STREAMLINE_DIR}/bin/x64/${runtime}"
                    "$<TARGET_FILE_DIR:rfgrt>/${runtime}"
            VERBATIM)
    endforeach()
elseif(RFGRT_X64)
    message(STATUS "Streamline SDK not found (tools/fetch_streamline.ps1): the plugin without frame generation")
endif()

# Version resources (src/shared/version.rc.in).
set(RC_FILENAME "rfgrt.asi")
set(RC_INTERNAL_NAME "rfgrt")
set(RC_FILE_TYPE "VFT_DLL")
set(RC_DESCRIPTION "rfgrt - path tracing for Red Faction Guerrilla Re-Mars-tered")
configure_file(src/shared/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/rfgrt_version.rc @ONLY)
target_sources(rfgrt PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rfgrt_version.rc)
if(RFGRT_X64)
    set(RC_FILENAME "rfgrt_host.exe")
    set(RC_INTERNAL_NAME "rfgrt_host")
    set(RC_FILE_TYPE "VFT_APP")
    set(RC_DESCRIPTION "rfgrt host - the 64-bit ray tracing process for Red Faction Guerrilla Re-Mars-tered")
    configure_file(src/shared/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/rfgrt_host_version.rc @ONLY)
    target_sources(rfgrt_host PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rfgrt_host_version.rc)
endif()
