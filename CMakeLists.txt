cmake_minimum_required(VERSION 3.16)
project(mesh-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-deprecated-declarations
)

# --- protobuf ---------------------------------------------------------------
# Generate C++ from the meshtastic protobufs (vendored as a git submodule).
find_package(Protobuf REQUIRED)

set(PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/protobufs")
file(GLOB MESHTASTIC_PROTOS "${PROTO_ROOT}/meshtastic/*.proto")
# nanopb.proto lives at the repo root and is imported by deviceonly.proto.
file(GLOB ROOT_PROTOS "${PROTO_ROOT}/nanopb.proto")
set(ALL_PROTOS ${MESHTASTIC_PROTOS} ${ROOT_PROTOS})

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

# Generate all protos in one protoc invocation (handles cross-imports).
add_custom_command(
    OUTPUT ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_PROTO_DIR}/meshtastic"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --proto_path=${PROTO_ROOT}
        --cpp_out=${GENERATED_PROTO_DIR}
        ${ALL_PROTOS}
    DEPENDS ${ALL_PROTOS}
    COMMENT "Generating meshtastic protobuf C++ sources"
    VERBATIM
)

add_library(meshtastic_protos STATIC ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS})
target_include_directories(meshtastic_protos PUBLIC ${GENERATED_PROTO_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(meshtastic_protos PUBLIC ${Protobuf_LIBRARIES})

# --- sdbus-c++ --------------------------------------------------------------
find_package(sdbus-c++ CONFIG REQUIRED)

# --- ncurses / sqlite -------------------------------------------------------
find_path(NCURSES_INCLUDE_DIR ncurses.h)
find_library(NCURSES_LIB NAMES ncursesw ncurses)
find_library(SQLITE3_LIB NAMES sqlite3)
find_path(SQLITE3_INCLUDE_DIR sqlite3.h)

# --- main library -----------------------------------------------------------
add_library(meshcli_core STATIC
    src/util/log.cpp
    src/util/event_loop.cpp
    src/mesh/mesh_codec.cpp
    src/mesh/node_db.cpp
    src/mesh/mesh_service.cpp
    src/ble/bluez_agent.cpp
    src/ble/bluez_client.cpp
    src/stream/stream_client.cpp
    src/store/database.cpp
)
target_include_directories(meshcli_core PUBLIC src ${GENERATED_PROTO_DIR})
target_link_libraries(meshcli_core PUBLIC
    meshtastic_protos
    sdbus-c++
    ${NCURSES_LIB}
    ${SQLITE3_LIB}
    Threads::Threads
)
target_include_directories(meshcli_core PUBLIC ${NCURSES_INCLUDE_DIR} ${SQLITE3_INCLUDE_DIR})

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

add_executable(mesh-cli src/main.cpp src/app/app.cpp src/app/config.cpp)
target_link_libraries(mesh-cli PRIVATE meshcli_tui)

# --- tests ------------------------------------------------------------------
enable_testing()
add_executable(mesh-cli-tests
    tests/test_main.cpp
    tests/test_codec.cpp
    tests/test_routing.cpp
    tests/test_window.cpp
    tests/test_database.cpp
    tests/test_commands.cpp
    tests/test_robustness_stream.cpp
    tests/test_robustness_db.cpp
    tests/test_robustness_edge.cpp
    src/app/config.cpp
)
target_link_libraries(mesh-cli-tests PRIVATE meshcli_tui)
target_include_directories(mesh-cli-tests PRIVATE src ${GENERATED_PROTO_DIR} tests)
add_test(NAME mesh-cli-tests COMMAND mesh-cli-tests)

# Live integration test (requires a real paired device; not part of ctest).
add_executable(mesh-cli-live tests/test_live_send.cpp src/app/config.cpp)
target_link_libraries(mesh-cli-live PRIVATE meshcli_tui)
target_include_directories(mesh-cli-live PRIVATE src ${GENERATED_PROTO_DIR})

install(TARGETS mesh-cli RUNTIME DESTINATION bin)
