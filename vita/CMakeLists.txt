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
if (NOT EXISTS "${GEN}/dispatch.c")
    message(FATAL_ERROR "Missing generated C in ${GEN}; run recomp/gen_c.py and recomp/gen_tables.py first")
endif()

file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS "${GEN}/guest_*.c")
set(RT_SOURCES
    ${ROOT}/rt/runtime.c ${ROOT}/rt/win32.c ${ROOT}/rt/com.c ${ROOT}/rt/d3d9.c ${ROOT}/rt/d3d9_gl.c
    ${ROOT}/rt/plat_vita.c ${ROOT}/rt/dinput.c ${ROOT}/rt/dsound.c ${ROOT}/rt/game_tmnt.c)

set(COMMON_FLAGS -std=gnu11 -O2 -mtune=cortex-a9 -mfpu=neon -fno-strict-aliasing)

# Translated code: warnings off, as in the Mac build.
add_library(guest STATIC ${GUEST_SOURCES} ${GEN}/dispatch.c ${GEN}/imports.c)
target_compile_options(guest PRIVATE ${COMMON_FLAGS} -w)
# Guest registers in locals (guest.h GUEST_GPR_LOCAL): with them in the CPU struct GCC reloads them after every guest store.
target_compile_definitions(guest PRIVATE GUEST_GENERATED_STACK_GUARD=0 GUEST_GPR_LOCAL=1)
target_include_directories(guest PRIVATE ${ROOT}/rt)

add_executable(TMNTVita ${RT_SOURCES})
target_compile_options(TMNTVita PRIVATE ${COMMON_FLAGS} -Wall -Wno-unused-function)
target_compile_definitions(TMNTVita PRIVATE GUEST_GENERATED_STACK_GUARD=0)
target_include_directories(TMNTVita PRIVATE ${ROOT}/rt)
# vita-elf-create appends SCE data after the text segment; headroom avoids failures near a 64 KB boundary (Rogue Squadron).
target_link_options(TMNTVita PRIVATE -Wl,--defsym=__sce_headroom=0x2000)

target_link_libraries(TMNTVita PRIVATE
    guest
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
vita_create_vpk(TMNTVita.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME "TMNT"
)
