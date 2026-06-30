cmake_minimum_required(VERSION 3.20)
project(Uncharted3Recomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Uncharted 3 - Drakes Deception (ps3recomp port)"
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Emplacement du SDK ps3recomp
# ---------------------------------------------------------------------------
if(NOT DEFINED PS3RECOMP_DIR)
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
message(STATUS "ps3recomp SDK: ${PS3RECOMP_DIR}")

# ---------------------------------------------------------------------------
# Bibliotheque runtime ps3recomp
# Si une lib pre-construite existe, on l'importe ; sinon on build le SDK
# en sous-repertoire (le runtime se construit avec le projet).
# ---------------------------------------------------------------------------
set(PS3RECOMP_RUNTIME_DIR "${PS3RECOMP_DIR}/build" CACHE PATH
    "Directory containing the pre-built ps3recomp_runtime library")

if(EXISTS "${PS3RECOMP_RUNTIME_DIR}/libps3recomp_runtime.a" OR
   EXISTS "${PS3RECOMP_RUNTIME_DIR}/ps3recomp_runtime.lib")
    add_library(ps3recomp_runtime STATIC IMPORTED GLOBAL)
    if(WIN32)
        set_target_properties(ps3recomp_runtime PROPERTIES
            IMPORTED_LOCATION "${PS3RECOMP_RUNTIME_DIR}/ps3recomp_runtime.lib")
    else()
        set_target_properties(ps3recomp_runtime PROPERTIES
            IMPORTED_LOCATION "${PS3RECOMP_RUNTIME_DIR}/libps3recomp_runtime.a")
    endif()
    target_include_directories(ps3recomp_runtime INTERFACE
        "${PS3RECOMP_DIR}/include")
else()
    message(STATUS "Building ps3recomp runtime from source")
    add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                     EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# Code recompile genere (recompiled/ppu_recomp_*.cpp)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RECOMPILED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled/*.c"
)
if(RECOMPILED_SOURCES)
    list(LENGTH RECOMPILED_SOURCES _n_recomp)
    message(STATUS "Uncharted3: ${_n_recomp} fichier(s) recompile(s) inclus")
else()
    message(WARNING "Uncharted3: aucun code recompile dans recompiled/ -- "
                    "lance d'abord le lifting (cf. STEP5_LIFTING.md)")
endif()

# Sources PPU "per-game" : exclues de la lib runtime (cf. root CMakeLists),
# compilees AVEC le jeu car elles incluent le ppu_recomp.h genere.
set(PPU_RUNTIME_SOURCES
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
)

# Lifted content job plus its SPURS policy module. Symbols in policy2 are
# prefixed so both overlapping LS images can be linked into the same harness.
set(SPU_LIFTED_SOURCES
    "${PS3RECOMP_DIR}/spu_gen/edge0/edge0.c"   # Edge geometry job (executor target)
    "${PS3RECOMP_DIR}/spu_gen/policy2/policy.c" # Job-chain policy (parameter builder)
)

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    ${PPU_RUNTIME_SOURCES}
    ${RECOMPILED_SOURCES}
    ${SPU_LIFTED_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"       # shim <dirent.h> + compat MSVC
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"             # ppu_context.h, ppu_ops.h
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled"   # ppu_recomp.h genere
)

# Les sources PPU "per-game" sont en style GCC/MinGW (__builtin_*, __attribute__,
# <dirent.h>). Sous MSVC/clang-cl on force-inclut un header de compat (no-op sur
# gcc/clang natif). Le shim <dirent.h> vient du dossier compat/ ci-dessus.
if(MSVC)
    set_source_files_properties(${PPU_RUNTIME_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/FIps3recomp_msvc_compat.h")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ps3recomp_runtime)

# D3D12 backend (libs/video/rsx_d3d12_backend.c) needs the COM GUID library for
# IID_IDXGIFactory4 / ID3D12Device etc. d3d12.lib + dxgi.lib are pulled in via
# #pragma comment in the backend, but dxguid.lib is not — add it explicitly.
if(MSVC)
    target_link_libraries(${PROJECT_NAME} PRIVATE dxguid.lib d3d12.lib dxgi.lib)
endif()

# ---------------------------------------------------------------------------
# Options compilateur
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus
        /bigobj
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
    )
    # The recompiled model uses one host stack frame per guest call (non-tail
    # calls are func_X(ctx); only fallthrough/tail uses the trampoline). Deep
    # guest call chains therefore need a large host stack — the default 1 MB
    # overflows during the engine's initialization. Reserve 512 MB.
    target_link_options(${PROJECT_NAME} PRIVATE /STACK:536870912)
    # Disable identical-COMDAT folding & unused-ref stripping: ICF folds the many
    # trivial identical lifted functions to one address, so crash backtraces (via
    # the .pdb) misattribute frames to the wrong func_XXXX. Keep symbols 1:1 for
    # accurate bring-up diagnostics. (Re-enable for release builds.)
    target_link_options(${PROJECT_NAME} PRIVATE /OPT:NOICF /OPT:NOREF)
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:-O2>)
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
