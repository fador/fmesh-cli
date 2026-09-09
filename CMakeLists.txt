cmake_minimum_required(VERSION 3.16)
project(fmesh-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)

if(WIN32)
    set(PDC_SDL2_BUILD OFF CACHE BOOL "Disable SDL2" FORCE)
    set(PDC_SDL2_DEPS_BUILD OFF CACHE BOOL "Disable SDL2 Deps" FORCE)
    set(PDC_GL_BUILD OFF CACHE BOOL "Disable GL" FORCE)
    set(PDC_WINCON_BUILD ON CACHE BOOL "Enable WinCon" FORCE)
endif()

if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(
        -Wall
        -Wextra
        -Werror
        -Wno-unused-parameter
        -Wno-deprecated-declarations
    )
endif()

include(FetchContent)

# --- protobuf ---------------------------------------------------------------
# Generate C++ from the meshtastic protobufs (vendored as a git submodule).
find_package(Protobuf)
if(NOT Protobuf_FOUND)
    message(STATUS "Protobuf not found, fetching via FetchContent")
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared" FORCE)
    set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "Use MD instead of MT" FORCE)
    FetchContent_Declare(
        protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG v3.21.12
    )
    FetchContent_MakeAvailable(protobuf)
    set(Protobuf_PROTOC_EXECUTABLE protoc)
    set(Protobuf_LIBRARIES libprotobuf)
    set(Protobuf_INCLUDE_DIRS "${protobuf_SOURCE_DIR}/src")
endif()

set(PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/protobufs")

# Ensure third_party/protobufs submodule is populated
if(NOT EXISTS "${PROTO_ROOT}/meshtastic/mesh.proto")
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        message(STATUS "Meshtastic protobufs not found in ${PROTO_ROOT}, initializing git submodule...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE GIT_SUBMOD_RESULT
        )
        if(NOT GIT_SUBMOD_RESULT EQUAL 0)
            message(WARNING "git submodule update --init --recursive failed with exit code ${GIT_SUBMOD_RESULT}")
        endif()
    endif()
endif()

file(GLOB MESHTASTIC_PROTOS "${PROTO_ROOT}/meshtastic/*.proto")
# nanopb.proto lives at the repo root and is imported by deviceonly.proto.
file(GLOB ROOT_PROTOS "${PROTO_ROOT}/nanopb.proto")
set(ALL_PROTOS ${MESHTASTIC_PROTOS} ${ROOT_PROTOS})

if(NOT ALL_PROTOS)
    message(FATAL_ERROR "No protobuf files found in ${PROTO_ROOT}. Please initialize git submodules by running: git submodule update --init --recursive")
endif()

set(GENERATED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_PROTO_DIR})

set(GENERATED_PROTO_SRCS)
set(GENERATED_PROTO_HDRS)
foreach(PROTO_FILE ${ALL_PROTOS})
    file(RELATIVE_PATH PROTO_REL "${PROTO_ROOT}" "${PROTO_FILE}")
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    get_filename_component(PROTO_DIR ${PROTO_REL} DIRECTORY)
    if(PROTO_DIR)
        set(OUT_DIR "${GENERATED_PROTO_DIR}/${PROTO_DIR}")
    else()
        set(OUT_DIR "${GENERATED_PROTO_DIR}")
    endif()
    list(APPEND GENERATED_PROTO_SRCS "${OUT_DIR}/${PROTO_NAME}.pb.cc")
    list(APPEND GENERATED_PROTO_HDRS "${OUT_DIR}/${PROTO_NAME}.pb.h")
endforeach()

# Locate protoc executable for configure-time generation
if(NOT PROTOC_EXECUTABLE)
    if(Protobuf_PROTOC_EXECUTABLE AND EXISTS "${Protobuf_PROTOC_EXECUTABLE}")
        set(PROTOC_EXECUTABLE "${Protobuf_PROTOC_EXECUTABLE}")
    elseif(PROTOBUF_PROTOC_EXECUTABLE AND EXISTS "${PROTOBUF_PROTOC_EXECUTABLE}")
        set(PROTOC_EXECUTABLE "${PROTOBUF_PROTOC_EXECUTABLE}")
    else()
        find_program(PROTOC_EXECUTABLE NAMES protoc
            HINTS
                "${CMAKE_CURRENT_BINARY_DIR}/_deps/protobuf-build/Debug"
                "${CMAKE_CURRENT_BINARY_DIR}/_deps/protobuf-build/Release"
                "${CMAKE_CURRENT_BINARY_DIR}/_deps/protobuf-build"
        )
    endif()
endif()

file(MAKE_DIRECTORY "${GENERATED_PROTO_DIR}/meshtastic")

set(PROTOC_ARGS
    "--proto_path=${PROTO_ROOT}"
    "--cpp_out=${GENERATED_PROTO_DIR}"
)
if(protobuf_SOURCE_DIR)
    list(APPEND PROTOC_ARGS "--proto_path=${protobuf_SOURCE_DIR}/src")
endif()

if(PROTOC_EXECUTABLE)
    message(STATUS "Generating meshtastic protobuf C++ sources via execute_process...")
    execute_process(
        COMMAND "${PROTOC_EXECUTABLE}"
            ${PROTOC_ARGS}
            ${ALL_PROTOS}
        RESULT_VARIABLE PROTOC_RESULT
        ERROR_VARIABLE PROTOC_ERR
    )
    if(NOT PROTOC_RESULT EQUAL 0)
        message(WARNING "protoc execution returned ${PROTOC_RESULT}: ${PROTOC_ERR}")
    endif()
else()
    message(STATUS "protoc not available at configure time; using existing generated protobufs if present")
endif()

# Re-run configure if any proto definition changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ALL_PROTOS})

set_source_files_properties(${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS} PROPERTIES GENERATED TRUE)

add_library(meshtastic_protos STATIC ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS})
target_include_directories(meshtastic_protos PUBLIC ${GENERATED_PROTO_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(meshtastic_protos PUBLIC ${Protobuf_LIBRARIES})

# --- sdbus-c++ --------------------------------------------------------------
if(NOT WIN32)
    find_package(sdbus-c++ CONFIG REQUIRED)
endif()

# --- ncurses / sqlite -------------------------------------------------------
find_package(SQLite3)
if(NOT SQLite3_FOUND)
    message(STATUS "SQLite3 not found, fetching amalgamation via FetchContent")
    FetchContent_Declare(
        sqlite3
        URL https://sqlite.org/2023/sqlite-amalgamation-3410200.zip
    )
    FetchContent_Populate(sqlite3)
    add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
    set(SQLITE3_LIB sqlite3)
    set(SQLITE3_INCLUDE_DIR ${sqlite3_SOURCE_DIR})
else()
    set(SQLITE3_LIB SQLite::SQLite3)
endif()

if(WIN32)
    message(STATUS "Windows detected, fetching PDCursesMod via FetchContent")
    set(PDC_BUILD_SHARED OFF CACHE BOOL "Build static PDCurses" FORCE)
    set(PDC_WIDE ON CACHE BOOL "Build wide-character PDCurses" FORCE)
    set(PDC_UTF8 ON CACHE BOOL "Build UTF-8 PDCurses" FORCE)
    FetchContent_Declare(
        pdcurses
        GIT_REPOSITORY https://github.com/Bill-Gray/PDCursesMod.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(pdcurses)
    set(NCURSES_LIB wincon_pdcursesstatic)
    set(NCURSES_INCLUDE_DIR "${pdcurses_SOURCE_DIR}")
else()
    find_path(NCURSES_INCLUDE_DIR ncurses.h)
    find_library(NCURSES_LIB NAMES ncursesw ncurses)
    add_compile_definitions(_XOPEN_SOURCE_EXTENDED)
endif()

# --- OpenSSL ----------------------------------------------------------------
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND AND WIN32)
    message(STATUS "OpenSSL not found on Windows, fetching via FetchContent")
    FetchContent_Declare(
        openssl
        GIT_REPOSITORY https://github.com/janbar/openssl-cmake.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(openssl)
    set(OPENSSL_LIBRARIES ssl crypto)
    set(OPENSSL_INCLUDE_DIR ${openssl_SOURCE_DIR}/include)
    set(OpenSSL_FOUND TRUE)
endif()

if(OpenSSL_FOUND OR OPENSSL_FOUND)
    message(STATUS "OpenSSL found, enabling Mesh network features")
    add_compile_definitions(ENABLE_MESH_NET=1)
else()
    message(STATUS "OpenSSL NOT found, Mesh network features disabled")
endif()

# --- zstd -------------------------------------------------------------------
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Disable zstd programs" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Disable zstd tests" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Disable shared zstd" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "Enable static zstd" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "Disable zstd legacy support" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.6
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

if(TARGET libzstd_static)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(libzstd_static PRIVATE -Wno-error -Wno-maybe-uninitialized)
    endif()
endif()

# --- nlohmann_json ----------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# --- main library -----------------------------------------------------------
set(CORE_SRCS
    src/util/log.cpp
    src/util/event_loop.cpp
    src/util/compression.cpp
    src/mesh/mesh_codec.cpp
    src/mesh/node_db.cpp
    src/mesh/mesh_service.cpp
    src/mesh/db_sync.cpp
    src/stream/stream_client.cpp
    src/stream/stream_server.cpp
    src/store/database.cpp
    src/web/http_socket.cpp
    src/web/http_server.cpp
    src/web/web_service.cpp
)

if(NOT WIN32)
    list(APPEND CORE_SRCS
        src/ble/bluez_agent.cpp
        src/ble/bluez_client.cpp
    )
else()
    list(APPEND CORE_SRCS
        src/ble/win_ble_client.cpp
    )
endif()

add_library(meshcli_core STATIC ${CORE_SRCS})
target_include_directories(meshcli_core PUBLIC src ${GENERATED_PROTO_DIR})
target_link_libraries(meshcli_core PUBLIC
    meshtastic_protos
    ${NCURSES_LIB}
    ${SQLITE3_LIB}
    Threads::Threads
    nlohmann_json::nlohmann_json
    libzstd_static
)
if(NOT WIN32)
    target_link_libraries(meshcli_core PUBLIC sdbus-c++)
else()
    target_link_libraries(meshcli_core PUBLIC ws2_32 windowsapp)
    target_compile_definitions(meshcli_core PUBLIC PDC_WIDE PDC_FORCE_UTF8)
endif()
target_include_directories(meshcli_core PUBLIC ${NCURSES_INCLUDE_DIR} ${SQLITE3_INCLUDE_DIR})

if(OpenSSL_FOUND OR OPENSSL_FOUND)
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        target_link_libraries(meshcli_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    else()
        target_link_libraries(meshcli_core PUBLIC ${OPENSSL_LIBRARIES})
    endif()
    if(OPENSSL_INCLUDE_DIR)
        target_include_directories(meshcli_core PUBLIC ${OPENSSL_INCLUDE_DIR})
    endif()
endif()

# --- TUI + executable -------------------------------------------------------
add_library(meshcli_tui STATIC
    src/tui/tui.cpp
    src/tui/window.cpp
    src/tui/window_manager.cpp
    src/tui/input_line.cpp
    src/tui/status_bar.cpp
    src/tui/command.cpp
    src/tui/keybinds.cpp
)
target_link_libraries(meshcli_tui PUBLIC meshcli_core)

add_executable(fmesh-cli src/main.cpp src/app/app.cpp src/app/config.cpp)
target_link_libraries(fmesh-cli PRIVATE meshcli_tui)
if(WIN32)
    target_link_libraries(fmesh-cli PRIVATE winmm)
endif()

# --- tests ------------------------------------------------------------------
enable_testing()
add_executable(fmesh-cli-tests
    tests/test_main.cpp
    tests/test_codec.cpp
    tests/test_routing.cpp
    tests/test_window.cpp
    tests/test_database.cpp
    tests/test_commands.cpp
    tests/test_robustness_stream.cpp
    tests/test_robustness_db.cpp
    tests/test_robustness_edge.cpp
    tests/test_sync_data.cpp
    tests/test_multi_node_sync.cpp
    tests/test_virtual_nodes.cpp
    tests/test_config_modify.cpp
    tests/test_config.cpp
    tests/test_compression.cpp
    tests/test_web_server.cpp
    src/app/config.cpp
)
target_link_libraries(fmesh-cli-tests PRIVATE meshcli_tui)
if(WIN32)
    target_link_libraries(fmesh-cli-tests PRIVATE winmm)
endif()
target_include_directories(fmesh-cli-tests PRIVATE src ${GENERATED_PROTO_DIR} tests)
add_test(NAME fmesh-cli-tests COMMAND fmesh-cli-tests)
set_tests_properties(fmesh-cli-tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Live integration test (requires a real paired device; not part of ctest).
add_executable(fmesh-cli-live tests/test_live_send.cpp src/app/config.cpp)
target_link_libraries(fmesh-cli-live PRIVATE meshcli_tui)
if(WIN32)
    target_link_libraries(fmesh-cli-live PRIVATE winmm)
endif()
target_include_directories(fmesh-cli-live PRIVATE src ${GENERATED_PROTO_DIR})

install(TARGETS fmesh-cli RUNTIME DESTINATION bin)
