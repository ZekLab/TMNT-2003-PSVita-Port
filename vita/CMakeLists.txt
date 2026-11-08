cmake_minimum_required(VERSION 3.20)

# PS Vita build of the translated TMNT_EN.EXE (VitaSDK). The Mac build is ../Makefile.
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "Set VITASDK (e.g. export VITASDK=$HOME/vitasdk)")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
endif()

project(TMNTVita LANGUAGES C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(GEN "${ROOT}/recomp/out/c")
# ON: the translated code is not in the VPK; it is loaded from tmnt_code.so, which the user makes with tools/make_vita_files.py.
option(TMNT_CODE_SO "Load the translated code from tmnt_code.so" ON)
if (NOT EXISTS "${GEN}/dispatch.c" AND NOT TMNT_CODE_SO)
    message(FATAL_ERROR "Missing generated C in ${GEN}; run recomp/gen_c.py and recomp/gen_tables.py first")
endif()

file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS "${GEN}/guest_*.c")
set(RT_SOURCES
    ${ROOT}/rt/runtime.c ${ROOT}/rt/win32.c ${ROOT}/rt/com.c ${ROOT}/rt/d3d9.c ${ROOT}/rt/d3d9_gl.c
    ${ROOT}/rt/plat_vita.c ${ROOT}/rt/dinput.c ${ROOT}/rt/dsound.c ${ROOT}/rt/game_tmnt.c ${ROOT}/rt/movie.c ${ROOT}/rt/par.c ${ROOT}/rt/code_so.c)

set(COMMON_FLAGS -std=gnu11 -O2 -mtune=cortex-a9 -mfpu=neon -fno-strict-aliasing)

if (NOT TMNT_CODE_SO)
    # Translated code: warnings off, as in the Mac build.
    add_library(guest STATIC ${GUEST_SOURCES} ${GEN}/dispatch.c ${GEN}/imports.c)
    target_compile_options(guest PRIVATE ${COMMON_FLAGS} -w)
    # Guest registers in locals (guest.h GUEST_GPR_LOCAL): with them in the CPU struct GCC reloads them after every guest store.
    target_compile_definitions(guest PRIVATE GUEST_GENERATED_STACK_GUARD=0 GUEST_GPR_LOCAL=1 GUEST_X87_LOCAL=1 GUEST_MEMBASE_LOCAL=1 GUEST_FLAGS_LOCAL=1)
    target_include_directories(guest PRIVATE ${ROOT}/rt)
endif()

add_executable(TMNTVita ${RT_SOURCES})
target_compile_options(TMNTVita PRIVATE ${COMMON_FLAGS} -Wall -Wno-unused-function)
target_compile_definitions(TMNTVita PRIVATE GUEST_GENERATED_STACK_GUARD=0)
target_include_directories(TMNTVita PRIVATE ${ROOT}/rt)
# vita-elf-create appends SCE data after the text segment; headroom avoids failures near a 64 KB boundary (Rogue Squadron).
target_link_options(TMNTVita PRIVATE -Wl,--defsym=__sce_headroom=0x2000)

# vitaGL built here with NO_DEBUG=1 HAVE_SHADER_CACHE=1 (build/vitagl), when present, before the SDK's copy.
if (EXISTS ${ROOT}/build/vitagl/libvitaGL.a)
    target_link_directories(TMNTVita PRIVATE ${ROOT}/build/vitagl)
endif()

if (TMNT_CODE_SO)
    target_compile_definitions(TMNTVita PRIVATE TMNT_CODE_SO=1)
    # The module for local testing (build/vita/tmnt_code.so), when the translated code is here; users make their own.
    if (EXISTS "${GEN}/dispatch.c")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_custom_target(tmnt_code ALL COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/make_vita_files.py --code-only ${CMAKE_CURRENT_BINARY_DIR}
            WORKING_DIRECTORY ${ROOT} VERBATIM)
    endif()
else()
    target_link_libraries(TMNTVita PRIVATE guest)
endif()

target_link_libraries(TMNTVita PRIVATE
    # vitaGL/vitashark reference SceShaccCgExt, which references taihen; GNU ld resolves left to right.
    vitaGL
    vitashark
    SceShaccCgExt
    taihen_stub
    mathneon
    SceShaccCg_stub
    SceKernelDmacMgr_stub
    SceGxm_stub
    SceDisplay_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceCtrl_stub
    SceAudio_stub
    ScePower_stub
    SceSysmodule_stub
    SceVideodec_stub
    SceIofilemgr_stub
    stdc++
    pthread
    m
)

# ---- SELF + VPK ----
set(VITA_TITLEID "TMNT02003")
set(VITA_VERSION "00.01")
# ATTRIBUTE2=12: extended memory budget for the app.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

vita_create_self(eboot.bin TMNTVita UNSAFE)
# Bubble icon and LiveArea are built locally from the game's art (tools/vita/livearea_from_art.py); without them the VPK gets the system defaults.
set(LIVEAREA_FILES)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg0.png AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/frame1.png)
    foreach(file icon0.png livearea/contents/bg0.png livearea/contents/frame1.png livearea/contents/startup.png livearea/contents/template.xml)
        list(APPEND LIVEAREA_FILES FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${file} sce_sys/${file})
    endforeach()
else()
    message(STATUS "No LiveArea images in vita/sce_sys; run tools/vita/livearea_from_art.py to add them")
endif()
vita_create_vpk(TMNTVita.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME "TMNT"
    ${LIVEAREA_FILES}
)
