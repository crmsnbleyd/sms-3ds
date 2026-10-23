cmake_minimum_required(VERSION 3.20)

# 3DS toolchain (devkitPro). Pass -DCMAKE_TOOLCHAIN_FILE yourself or let this pick it up from $DEVKITPRO.
if(NOT CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{DEVKITPRO})
    set(ENV{DEVKITPRO} /opt/devkitpro)
  endif()
  set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/3DS.cmake" CACHE FILEPATH "")
endif()

project(sms_3ds C CXX ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(cmake/patches.cmake)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
# -DNDEBUG comes from the decomp's own NDEBUG=1.
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "" FORCE)

# --- Reference checkout (read-only) -----------------------------------------------------------
set(SMS_PC_PORT_DIR ${CMAKE_SOURCE_DIR}/refs/sms-pc-port CACHE PATH "chasem-dev/sms-pc-port checkout (read-only)")
set(SMS_PC_PORT_COMMIT e3cdcca1d3a78f657d2d195c0db084bbe28c5a45)
set(SMS_DECOMP_COMMIT 967436b8c623834d38a57d34923775f45eca8861)
option(SMS_ALLOW_REFS_MISMATCH "Build against refs at other commits than the recorded ones" OFF)
foreach(pair "${SMS_PC_PORT_DIR};${SMS_PC_PORT_COMMIT}" "${SMS_PC_PORT_DIR}/decomp;${SMS_DECOMP_COMMIT}")
  list(GET pair 0 dir)
  list(GET pair 1 want)
  execute_process(COMMAND git -C ${dir} rev-parse HEAD OUTPUT_VARIABLE have
                  OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sms-3ds: ${dir} is not a git checkout (see CLAUDE.md: clone sms-pc-port into refs/)")
  elseif(NOT have STREQUAL want)
    if(SMS_ALLOW_REFS_MISMATCH)
      message(WARNING "sms-3ds: ${dir} is at ${have}, recorded ${want}")
    else()
      message(FATAL_ERROR "sms-3ds: ${dir} is at ${have}, recorded ${want} (-DSMS_ALLOW_REFS_MISMATCH=ON to try anyway)")
    endif()
  endif()
endforeach()

set(SMS_DECOMP ${SMS_PC_PORT_DIR}/decomp)
set(SMS_PORT_SRC ${SMS_PC_PORT_DIR}/src)
set(SMS_PLATFORM ${SMS_PC_PORT_DIR}/platform)
set(SMS_DECOMP_INCLUDE_SUBDIRS include
  libs/dolphin/include libs/JSystem/include libs/THPPlayer/include
  libs/PowerPC_EABI_Support/include libs/TRK_MINNOW_DOLPHIN/include libs/OdemuExi2/include)

# --- Decomp sources and patches ---------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${SMS_PC_PORT_DIR}/tools/gen_sources.py ${SMS_DECOMP} ${CMAKE_BINARY_DIR}/decomp_sources.cmake
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sms-3ds: tools/gen_sources.py failed")
endif()
include(${CMAKE_BINARY_DIR}/decomp_sources.cmake)

# Upstream's PC patches first, then ours on top (both in name order).
file(GLOB _up_patches ${SMS_PC_PORT_DIR}/decomp-patches/*.patch)
file(GLOB _our_decomp_patches CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/patches/decomp/*.patch)
list(SORT _up_patches)
list(SORT _our_decomp_patches)
set(SMS_PATCH_ROOT ${CMAKE_BINARY_DIR}/patched)
sms_apply_patches(decomp SOURCE_ROOT ${SMS_DECOMP} DEST_ROOT ${SMS_PATCH_ROOT}
  PATCHES ${_up_patches} ${_our_decomp_patches} TOUCHED_VAR _decomp_touched)
foreach(lst SMS_DECOMP_CXX_SOURCES SMS_DECOMP_C_SOURCES SMS_DECOMP_PCH_SOURCES)
  sms_substitute_patched(${lst} ${SMS_DECOMP} ${SMS_PATCH_ROOT} "${_decomp_touched}")
endforeach()
set(SMS_DECOMP_INCLUDE_DIRS "")
set(SMS_PATCHED_INCLUDE_DIRS "")
foreach(d ${SMS_DECOMP_INCLUDE_SUBDIRS})
  list(APPEND SMS_DECOMP_INCLUDE_DIRS ${SMS_DECOMP}/${d})
  list(APPEND SMS_PATCHED_INCLUDE_DIRS ${SMS_PATCH_ROOT}/${d})
endforeach()

# Our patches to the PC port's own files (platform/, src/), applied to copies.
file(GLOB _port_patches CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/patches/port/*.patch)
list(SORT _port_patches)
set(SMS_PORT_PATCH_ROOT ${CMAKE_BINARY_DIR}/port-patched)
sms_apply_patches(port SOURCE_ROOT ${SMS_PC_PORT_DIR} DEST_ROOT ${SMS_PORT_PATCH_ROOT}
  PATCHES ${_port_patches} TOUCHED_VAR _port_touched)

# A patched copy of a port file still includes its siblings by "name": give each patched source
# its original directory as an include path (after the patched roots).
function(sms_port_sources out)
  set(result "")
  foreach(rel ${ARGN})
    list(FIND _port_touched ${rel} idx)
    if(idx EQUAL -1)
      list(APPEND result ${SMS_PC_PORT_DIR}/${rel})
    else()
      get_filename_component(origdir ${SMS_PC_PORT_DIR}/${rel} DIRECTORY)
      set_property(SOURCE ${SMS_PORT_PATCH_ROOT}/${rel} APPEND PROPERTY INCLUDE_DIRECTORIES ${origdir})
      list(APPEND result ${SMS_PORT_PATCH_ROOT}/${rel})
    endif()
  endforeach()
  set(${out} ${result} PARENT_SCOPE)
endfunction()

# Header search roots for port code: patched copies shadow the originals.
set(SMS_PORT_INCLUDE_ROOTS src src/port_include platform platform/mods/include platform/gx/include platform/gx/src)
set(SMS_PORT_PATCHED_INCLUDE_DIRS "")
set(SMS_PORT_ORIG_INCLUDE_DIRS "")
foreach(d ${SMS_PORT_INCLUDE_ROOTS})
  list(APPEND SMS_PORT_PATCHED_INCLUDE_DIRS ${SMS_PORT_PATCH_ROOT}/${d})
  list(APPEND SMS_PORT_ORIG_INCLUDE_DIRS ${SMS_PC_PORT_DIR}/${d})
endforeach()

# --- Common flags -----------------------------------------------------------------------------
set(SMS_COMMON_DEFS GEKKO TARGET_PC VERSION_GMSE01 BUILD_VERSION=2 NDEBUG=1)
# CodeWarrior's char is signed; ARM GCC's default is unsigned.
add_compile_options(-fsigned-char)
# CodeWarrior's enums are 4 bytes; bare-metal ARM GCC (AAPCS) sizes them to their values, which
# shifts every struct with an enum member (GX state, structs laid over disc data). Everything that
# shares the game's structs uses 4-byte enums; only the files that include <3ds.h> keep libctru's
# short enums (SMS_3DS_SHORT_ENUM_SOURCES below).
add_compile_options(-fno-short-enums)

set(SMS_GAME_FLAGS
  -w -fno-strict-aliasing -fwrapv -fno-omit-frame-pointer -fno-stack-protector -ffp-contract=off
  -finput-charset=UTF-8 -fexec-charset=CP932
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++03 -fno-gnu-keywords -fpermissive -fno-threadsafe-statics>
  $<$<COMPILE_LANGUAGE:C>:-std=gnu99 -fcommon>
  -include ${SMS_PORT_SRC}/port_compat.h)

# --- Game (decomp) ----------------------------------------------------------------------------
add_library(sms_game STATIC ${SMS_DECOMP_CXX_SOURCES} ${SMS_DECOMP_C_SOURCES})
set_source_files_properties(${SMS_DECOMP_CXX_SOURCES} PROPERTIES LANGUAGE CXX)
set_source_files_properties(${SMS_DECOMP_CXX_SOURCES} ${SMS_DECOMP_C_SOURCES} PROPERTIES
  OBJECT_DEPENDS "${SMS_PATCH_ROOT}/headers.stamp;${SMS_PORT_PATCH_ROOT}/headers.stamp")
if(EXISTS ${SMS_PATCH_ROOT}/include/SMS.pch)
  set(_sms_pch ${SMS_PATCH_ROOT}/include/SMS.pch)
else()
  set(_sms_pch ${SMS_DECOMP}/include/SMS.pch)
endif()
set_source_files_properties(${SMS_DECOMP_PCH_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${_sms_pch}")
target_sources(sms_game PRIVATE
  ${SMS_PATCH_ROOT}/libs/dolphin/src/thp/THPDec.c
  ${SMS_PATCH_ROOT}/libs/dolphin/src/thp/THPAudio.c
  ${SMS_DECOMP}/libs/dolphin/src/pad/Padclamp.c
  ${SMS_PORT_SRC}/savestate_menu.cpp
  ${SMS_PORT_SRC}/pause_tracker.cpp
  ${SMS_PC_PORT_DIR}/ext/core/port_ext.cpp)
# GXVert.h's vertex writers become calls into the GX front end (platform/gx/port_include first).
target_include_directories(sms_game BEFORE PRIVATE
  ${SMS_PC_PORT_DIR}/platform/gx/port_include ${SMS_PC_PORT_DIR}/platform/gx/include
  ${SMS_PORT_PATCH_ROOT}/src/port_include ${SMS_PORT_SRC}/port_include
  ${SMS_PATCHED_INCLUDE_DIRS} ${SMS_DECOMP_INCLUDE_DIRS})
target_compile_definitions(sms_game PRIVATE ${SMS_COMMON_DEFS})
target_compile_options(sms_game PRIVATE ${SMS_GAME_FLAGS})

# The game's global operator new/delete allocate from JKR heaps; rename them so the platform
# layer and libstdc++ keep the host allocator (as upstream does). size_t is unsigned int: 'j'.
file(WRITE ${CMAKE_BINARY_DIR}/game_new_syms.txt
  "_Znwj __sms_game_new\n_Znaj __sms_game_new_array\n_ZdlPv __sms_game_delete\n_ZdaPv __sms_game_delete_array\n")
add_custom_command(TARGET sms_game POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} --redefine-syms=${CMAKE_BINARY_DIR}/game_new_syms.txt $<TARGET_FILE:sms_game>
  COMMENT "Renaming the game's global operator new/delete")

# --- GX front end (register encoding + FIFO parser), renderer seam filled by platform3ds -------
sms_port_sources(SMS_GX_FRONT_SOURCES
  platform/gx/src/gx_api.cpp platform/gx/src/gx_fifo.cpp platform/gx/src/gx_vert.cpp)
set(SMS_GX_GD_SOURCES "")
foreach(f GDBase GDGeometry GDLight GDPixel GDTev GDTransform)
  list(APPEND SMS_GX_GD_SOURCES ${SMS_DECOMP}/libs/dolphin/src/gd/${f}.c)
endforeach()
set_source_files_properties(${SMS_GX_GD_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${SMS_PC_PORT_DIR}/platform/gx/src/gd_host_prefix.h;-w;-fcommon")
# Renderer: gx3ds (citro3d, Phase 2) or the null backend (Phase 1: counts what would be drawn).
set(SMS_3DS_GX c3d CACHE STRING "GX backend: c3d or null")
if(SMS_3DS_GX STREQUAL "null")
  set(SMS_GX_BACKEND_SOURCES platform3ds/gx_null_backend.cpp)
else()
  set(SMS_GX_BACKEND_SOURCES platform3ds/gx3ds/render.cpp platform3ds/gx3ds/vtx.cpp platform3ds/gx3ds/texture.cpp
    platform3ds/gx3ds/tev.cpp)
  # Built with libctru's short enums (they include <citro3d.h>); they share no enum-typed struct
  # members with the GX front end.
  set_source_files_properties(${SMS_GX_BACKEND_SOURCES} PROPERTIES COMPILE_OPTIONS "-fshort-enums")
  ctr_add_shader_library(gx3ds_passthru platform3ds/gx3ds/passthru.v.pica)
  ctr_add_shader_library(gx3ds_gx platform3ds/gx3ds/gx.v.pica)
  dkp_add_embedded_binary_library(gx3ds_shaders gx3ds_passthru gx3ds_gx)
endif()
# The GX thread (both backends): the front end and renderer on core 2, beside the game.
add_library(sms_gx STATIC ${SMS_GX_FRONT_SOURCES} ${SMS_GX_GD_SOURCES} ${SMS_GX_BACKEND_SOURCES}
  platform3ds/gx3ds/gxthread.cpp)
if(TARGET gx3ds_shaders)
  target_link_libraries(sms_gx PRIVATE gx3ds_shaders)
endif()
target_include_directories(sms_gx PRIVATE ${CMAKE_SOURCE_DIR}/platform3ds ${CMAKE_SOURCE_DIR}/platform3ds/gx3ds)
target_include_directories(sms_gx PRIVATE
  ${SMS_PORT_PATCHED_INCLUDE_DIRS} ${SMS_PORT_ORIG_INCLUDE_DIRS}
  ${SMS_PC_PORT_DIR}/platform/gx/port_include
  ${SMS_DECOMP}/include ${SMS_DECOMP}/libs/dolphin/include)
target_compile_definitions(sms_gx PRIVATE SMS_3DS_GX_NULL=1)
target_compile_options(sms_gx PRIVATE -ffp-contract=off $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# --- Platform layer ---------------------------------------------------------------------------
sms_port_sources(SMS_PLATFORM_SOURCES
  platform/port_runtime.cpp
  platform/sdk_stubs.cpp
  platform/os/os_core.cpp
  platform/os/os_thread.cpp
  platform/os/crash_info.cpp
  platform/vi/vi.cpp
  platform/dvd/dvd.cpp
  platform/disc/gcdisc.cpp
  platform/ar/ar.cpp
  platform/card/card.cpp
  platform/audio/ai.cpp
  platform/audio/noaudio.cpp
  platform/audio/dsp_hle.cpp
  platform/audio/dtk.cpp
  platform/endian/endian_dispatch.cpp
  platform/endian/endian_game.cpp
  platform/endian/endian_j3d.cpp
  platform/endian/endian_jaudio.cpp
  platform/endian/endian_jpa.cpp
  platform/endian/endian_seq.cpp
  platform/misc/endian.cpp
  platform/misc/language.cpp
  platform/misc/msl_math.c
  platform/misc/msl_rand.c
  platform/misc/port_cheats.cpp
  platform/misc/port_warp.cpp
  platform/misc/sdk_data.cpp
  platform/mods/modhooks.cpp
  platform/mtx/mtx.cpp
  src/lbl_80004600_darwin.c)
add_executable(sms_3ds
  ${SMS_PLATFORM_SOURCES}
  platform3ds/main_3ds.cpp
  platform3ds/hud.cpp
  platform3ds/log_3ds.cpp
  platform3ds/port_host_3ds.cpp
  platform3ds/stubs_3ds.cpp
  platform3ds/thread_3ds.cpp
  platform3ds/hid_3ds.cpp
  platform3ds/pad_3ds.cpp
  platform3ds/presence_3ds.cpp
  platform3ds/dsp_ndsp.cpp)  # in place of platform/audio/dsp_mixer.cpp: voices played by the 3DS DSP
target_include_directories(sms_3ds PRIVATE
  ${CMAKE_SOURCE_DIR}/platform3ds
  ${SMS_PC_PORT_DIR}/platform/audio
  ${SMS_PORT_PATCHED_INCLUDE_DIRS} ${SMS_PORT_ORIG_INCLUDE_DIRS}
  ${SMS_DECOMP_INCLUDE_DIRS})
target_compile_definitions(sms_3ds PRIVATE ${SMS_COMMON_DEFS} SMS_AUDIO_DSP_HLE=1)
target_compile_options(sms_3ds PRIVATE -fno-strict-aliasing -Wno-unused-parameter
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
# These include <3ds.h> and share structs with libctru, which is built with short enums.
set(SMS_3DS_SHORT_ENUM_SOURCES
  platform3ds/main_3ds.cpp platform3ds/hud.cpp platform3ds/log_3ds.cpp
  platform3ds/port_host_3ds.cpp platform3ds/thread_3ds.cpp platform3ds/hid_3ds.cpp platform3ds/dsp_ndsp.cpp)
set_source_files_properties(${SMS_3DS_SHORT_ENUM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fshort-enums")
set_source_files_properties(${SMS_PC_PORT_DIR}/platform/misc/msl_math.c ${SMS_PC_PORT_DIR}/platform/mtx/mtx.cpp
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
target_link_libraries(sms_3ds PRIVATE
  -Wl,--whole-archive sms_game sms_gx -Wl,--no-whole-archive
  citro3d ctru m)
set_property(TARGET sms_3ds APPEND PROPERTY LINK_OPTIONS -Wl,-Map,${CMAKE_BINARY_DIR}/sms_3ds.map)
# Mixed enum sizes are intended (see -fno-short-enums above): no struct with an enum member crosses
# between libctru/<3ds.h> code and game code.
set_property(TARGET sms_3ds APPEND PROPERTY LINK_OPTIONS -Wl,--no-enum-size-warning)

# --- 3DS package --------------------------------------------------------------------------------
ctr_generate_smdh(${CMAKE_BINARY_DIR}/sms_3ds.smdh
  NAME "sms-3ds"
  DESCRIPTION "Super Mario Sunshine port (Phase 1: headless)"
  AUTHOR "sms-3ds")
ctr_create_3dsx(sms_3ds SMDH ${CMAKE_BINARY_DIR}/sms_3ds.smdh)
# The 3DSX loader rejects relocations that point outside the image (e.g. &x + 0x80000000).
add_custom_command(TARGET sms_3ds_3dsx POST_BUILD
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/scripts/check-3dsx.py ${CMAKE_BINARY_DIR}/sms_3ds.3dsx
  COMMENT "Checking 3DSX relocations")
