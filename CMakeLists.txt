cmake_minimum_required(VERSION 3.20)
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH
    "Path to psxrecomp")
if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake")
    message(FATAL_ERROR
        "PSXRecomp runtime is missing: ${PSXRECOMP_ROOT}/runtime/runtime.cmake\n"
        "This project expects the psxrecomp framework checkout at ${PSXRECOMP_ROOT}.\n"
        "Run: git submodule update --init --recursive\n"
        "If psxrecomp/ is not a submodule, set -DPSXRECOMP_ROOT=<path-to-psxrecomp>.")
endif()

project(VagrantStoryRecomp LANGUAGES C CXX)

# --- Opt-in product surfaces (OFF by default in the framework) -------------
# Scaffold prompts / flags fill these. Without recomp-ui, PSX_RECOMP_UI is OFF.
# The wizard (WIZARD_CMAKE_BLOCK / ENABLE_SETUP_WIZARD) is the first-run disc
# picker; releases are compiled from the committed generated/ C, so it never
# needs to generate or rebuild anything on a player's machine.
# recomp-ui submodule present (PSX_RECOMP_UI defaults ON).
# Full netplay UI -- enable after adding recomp-ui + testing:
# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "..." FORCE)
# endif()
# First-run setup wizard -- enable after testing:
# set(PSX_SETUP_WIZARD ON CACHE BOOL "..." FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# This project's own mod catalog. The FRAMEWORK stages it -- together with its
# builtin psx.* packages -- into <exe-dir>/mods/bundled, and refuses to
# configure if a catalog exists here but is not declared. Never copy it by
# hand: five titles did, and framework 4cc04be3's rename of the staged
# directory broke all five silently (bead beads-eio.3.101).
set(PSX_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(NOT IS_DIRECTORY "${PSX_PRELOADED_MODS}")
    set(PSX_PRELOADED_MODS "NONE")
endif()

# Captured overlays (BATTLE.PRG and friends) compiled into the exe with this
# compiler instead of TinyCC at runtime. Built from overlay_captures.json.d by
# compile_overlays.py --static into generated/overlays/ (never committed). The
# disc-code build must stay free of game code, so it never links them.
set(VS_OVERLAY_STATIC_ARGS "")
if(NOT PSXRECOMP_DISC_CODE AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/overlays/overlays_static.c")
    set(VS_OVERLAY_STATIC_ARGS GAME_OVERLAY_STATIC_C
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/overlays/overlays_static.c")
endif()

# PGXP hook flavor: the runtime tracks the GTE's sub-pixel projections through
# the game's own register/memory copies so the PGXP Precision mod can draw
# vertices off the integer grid and texture with perspective (no PS1 wobble).
# Inert until the mod is enabled (Mods page / Settings).
option(VS_PGXP_HOOKS "Build the runtime as the PGXP hook flavor" ON)
set(VS_PGXP_ARGS "")
if(VS_PGXP_HOOKS)
    set(VS_PGXP_ARGS PGXP)
endif()

psxrecomp_add_game_runtime(psx-runtime
    ${VS_PGXP_ARGS}
    ${VS_OVERLAY_STATIC_ARGS}
    PRELOADED_MODS_DIR "${PSX_PRELOADED_MODS}"
    # ENABLE_NETPLAY_IF_PRESENT
    # NETPLAY_LOBBY_URL "ws://netplay.retcomm.net:8765"
    # ENABLE_SETUP_WIZARD
    WINDOW_TITLE "VagrantStory Recompiled"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 1
    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"
    # Committed game C (psxrecomp_cli.py generate; git add generated). Release
    # CI configures with -DPSXRECOMP_REQUIRE_GAME_C=ON so a checkout without
    # it fails instead of building a setup host.
    GEN_MARKER "generated/SLUS_010.40_dispatch.c"
    GEN_FULL_GLOB "generated/SLUS_010.40_full_*.c"
    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"
)

# Vagrant Story's own trusted mod plugins (selected by packages under
# mods/preloaded/packages; inert unless the launcher enables the feature).
target_sources(psx-runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_widescreen_mod.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_prompts.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_game_state.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_stick_camera.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_skip_logos.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_glyphs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_audio_session.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vs_overlay_precompile.c")

# Disc-code builds compile the overlays from the player's disc on first launch
# (src/vs_overlay_precompile.c): stage the script and its address-only seed
# list into the bundled overlay_toolchain/.
if(PSXRECOMP_DISC_CODE)
    add_custom_command(TARGET psx-runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:psx-runtime>/overlay_toolchain"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/vs_overlay_precompile.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/aot/vs_overlay_seeds.json"
            "$<TARGET_FILE_DIR:psx-runtime>/overlay_toolchain/"
        COMMENT "Staging the overlay precompile script and seeds"
        VERBATIM)
endif()

# Windows Volume Mixer: the game labels its audio session with the app icon
# (src/vs_audio_session.c), read from assets/psxrecomp.ico next to the exe.
if(WIN32)
    target_link_libraries(psx-runtime PRIVATE ole32 shell32)
    add_custom_command(TARGET psx-runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:psx-runtime>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"
            "$<TARGET_FILE_DIR:psx-runtime>/assets/psxrecomp.ico"
        COMMENT "Staging assets/psxrecomp.ico"
        VERBATIM)
endif()

# Stage runtime configs next to the exe for direct build/ launches. Only copy
# files that exist — never fail POST_BUILD when game_options.toml is absent.
foreach(_PSX_RUNTIME_CONFIG game.toml game_options.toml)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}")
        add_custom_command(TARGET psx-runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}"
                "$<TARGET_FILE_DIR:psx-runtime>/${_PSX_RUNTIME_CONFIG}"
            COMMENT "Staging ${_PSX_RUNTIME_CONFIG}"
            VERBATIM)
    endif()
endforeach()
