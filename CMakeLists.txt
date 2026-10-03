# tinydesk host build: the core, the demo apps, a desktop port (POSIX or
# Windows console), unit tests and examples.
#
#   cmake -B build && cmake --build build && ./build/tinydesk
#   ctest --test-dir build --output-on-failure
#
# The ESP32-C6 firmware is a separate ESP-IDF project in ports/esp32c6.
cmake_minimum_required(VERSION 3.16)
project(tinydesk C)

# On a PC the file root is <working directory>/tinydesk_fs: allow long paths.
add_compile_definitions(TD_PATH_MAX=512)

option(TD_WITH_TDSH "Embed TinyDesk Shell in the Terminal app" ON)
option(TD_BUILD_TESTS "Build unit tests" ON)
option(TD_BUILD_EXAMPLES "Build examples" ON)

set(TDSH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tdsh)

if(MSVC)
    set(TD_WARNINGS /W4 /WX /utf-8)   # sources contain UTF-8 icon glyphs
else()
    set(TD_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------- core
add_library(tinydesk_core STATIC
    src/draw.c
    src/event.c
    src/input.c
    src/render.c
    src/screen.c
    src/td.c
    src/theme.c
    src/timer.c
    src/utf8.c
    src/vterm.c
    src/widgets.c
    src/wm.c
)
target_include_directories(tinydesk_core PUBLIC include)
set_target_properties(tinydesk_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(tinydesk_core PRIVATE ${TD_WARNINGS})

# ------------------------------------------------ protocols (MQTT, Modbus)
add_library(tinydesk_proto STATIC
    proto/td_modbus.c
    proto/td_mqtt.c
    proto/td_mqtt_conf.c
    proto/td_sock.c
    proto/td_tls.c
)
target_include_directories(tinydesk_proto PUBLIC proto)
set_target_properties(tinydesk_proto PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(tinydesk_proto PRIVATE ${TD_WARNINGS})
if(WIN32)
    target_link_libraries(tinydesk_proto PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tinydesk_proto PUBLIC Threads::Threads)
endif()

# MQTT over TLS needs mbedTLS 3.x. Point TD_MBEDTLS_DIR at its source tree,
# or leave it empty to use ESP-IDF's copy when one is installed. Without it
# the host build still works, only mqtts is refused.
set(TD_MBEDTLS_DIR "" CACHE PATH "mbedTLS 3.x source tree (for MQTT over TLS)")
# Release builds set this, so a program without TLS is never published.
option(TD_REQUIRE_TLS "Stop when no mbedTLS source tree is found" OFF)
if(NOT TD_MBEDTLS_DIR)
    foreach(cand "$ENV{IDF_PATH}/components/mbedtls/mbedtls"
                 "C:/Espressif/frameworks/esp-idf-v5.3.1/components/mbedtls/mbedtls"
                 "/mnt/c/Espressif/frameworks/esp-idf-v5.3.1/components/mbedtls/mbedtls"
                 "$ENV{HOME}/esp/esp-idf/components/mbedtls/mbedtls")
        if(EXISTS "${cand}/include/mbedtls/ssl.h")
            set(TD_MBEDTLS_DIR "${cand}")
            break()
        endif()
    endforeach()
endif()
if(TD_MBEDTLS_DIR)
    message(STATUS "tinydesk: MQTT over TLS with mbedTLS from ${TD_MBEDTLS_DIR}")
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(GEN_FILES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${TD_MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)
    target_link_libraries(tinydesk_proto PUBLIC mbedtls mbedx509 mbedcrypto)
    target_compile_definitions(tinydesk_proto PUBLIC TD_HAVE_TLS)
    if(WIN32)
        target_link_libraries(tinydesk_proto PUBLIC crypt32 bcrypt)
    endif()
elseif(TD_REQUIRE_TLS)
    message(FATAL_ERROR "tinydesk: TD_REQUIRE_TLS is set but no mbedTLS source tree was found; "
                        "set TD_MBEDTLS_DIR (for example a checkout of https://github.com/Mbed-TLS/mbedtls, v3.6.x)")
else()
    message(STATUS "tinydesk: no mbedTLS found, MQTT over TLS disabled")
endif()

# ---------------------------------------------------------------- apps
add_library(tinydesk_apps STATIC
    apps/about.c
    apps/apps.c
    apps/counter.c
    apps/datetime.c
    apps/desktop.c
    apps/editor.c
    apps/files.c
    apps/logview.c
    apps/modbus.c
    apps/mqtt.c
    apps/network.c
    apps/session.c
    apps/settings.c
    apps/sysmon.c
    apps/taskmgr.c
    apps/terminal.c
    apps/update.c
)
target_include_directories(tinydesk_apps PUBLIC apps)
target_link_libraries(tinydesk_apps PUBLIC tinydesk_core tinydesk_proto)
set_target_properties(tinydesk_apps PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(tinydesk_apps PRIVATE ${TD_WARNINGS})

# ------------------------------------------------------------ host HAL
if(WIN32)
    add_library(tinydesk_hal STATIC ports/windows/hal_win.c)
else()
    add_library(tinydesk_hal STATIC ports/posix/hal_posix.c)
endif()
target_include_directories(tinydesk_hal PUBLIC ports/common)
target_link_libraries(tinydesk_hal PUBLIC tinydesk_core)
set_target_properties(tinydesk_hal PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(tinydesk_hal PRIVATE ${TD_WARNINGS})

# -------------------------------------------------------------- TinyDesk Shell
if(TD_WITH_TDSH)
    set(TDSH_CORE_SRCS
        ${TDSH_ROOT}/src/core/tdsh_builtin.c
        ${TDSH_ROOT}/src/core/tdsh_core.c
        ${TDSH_ROOT}/src/core/tdsh_parser.c
        ${TDSH_ROOT}/src/core/tdsh_path.c
        ${TDSH_ROOT}/src/core/tdsh_script.c
        ${TDSH_ROOT}/src/core/tdsh_terminal.c
        ${TDSH_ROOT}/src/core/tdsh_board.c
    )
    if(WIN32)
        # The TinyDesk Shell core expects newlib/glibc stdio; the compat layer adds
        # per-thread stdin/stdout and funopen() on the Windows C runtime.
        # (TinyDesk Shell's Windows port, ports/windows in the submodule.)
        set(TDSH_WIN_DIR ${TDSH_ROOT}/ports/windows)
        # The compat layer itself must see the real C-runtime stdio, so it is
        # built separately, without the forced include.
        add_library(tdsh_wincompat STATIC ${TDSH_WIN_DIR}/tdsh_win_compat.c)
        target_include_directories(tdsh_wincompat PUBLIC ${TDSH_WIN_DIR})
        add_library(tdsh STATIC
            ${TDSH_CORE_SRCS}
            ${TDSH_WIN_DIR}/tdsh_platform_win.c
            ${TDSH_WIN_DIR}/tdsh_win_commands.c
        )
        target_include_directories(tdsh PUBLIC ${TDSH_ROOT}/include ${TDSH_WIN_DIR})
        # ifconfig and ping use the IP Helper API.
        target_link_libraries(tdsh PUBLIC tdsh_wincompat iphlpapi ws2_32)
        set(TDSH_COMPAT_FLAG -include ${TDSH_WIN_DIR}/tdsh_win_compat.h)
        target_compile_options(tdsh PRIVATE ${TDSH_COMPAT_FLAG})
    else()
        find_package(Threads REQUIRED)
        add_library(tdsh STATIC
            ${TDSH_CORE_SRCS}
            ${TDSH_ROOT}/ports/posix/tdsh_posix.c
            ${TDSH_ROOT}/ports/posix/tdsh_posix_commands.c
            ${TDSH_ROOT}/ports/posix/tdsh_nano_posix.c
        )
        target_include_directories(tdsh PUBLIC ${TDSH_ROOT}/include ${TDSH_ROOT}/ports/posix)
        target_link_libraries(tdsh PUBLIC Threads::Threads)
    endif()
    # Third-party code: built as GNU C11, without tinydesk's -Werror.
    set_target_properties(tdsh PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(tdsh PRIVATE -w)
endif()

# ---------------------------------------------------------- executable
add_executable(tinydesk
    ports/common/host_main.c
    ports/common/td_fs_stdio.c
    $<IF:$<BOOL:${WIN32}>,ports/windows/main.c,ports/posix/main.c>
)
target_link_libraries(tinydesk PRIVATE tinydesk_apps tinydesk_hal)
set_target_properties(tinydesk PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(tinydesk PRIVATE ${TD_WARNINGS})
if(TD_WITH_TDSH)
    target_sources(tinydesk PRIVATE ports/common/tdsh_bridge_host.c ports/common/td_proto_cmds.c)
    target_compile_definitions(tinydesk PRIVATE TD_WITH_TDSH)
    target_link_libraries(tinydesk PRIVATE tdsh)
    if(WIN32)
        # The bridge assigns stdin/stdout for the shell thread, and the
        # commands print through the shell thread's stdout.
        set_source_files_properties(ports/common/tdsh_bridge_host.c ports/common/td_proto_cmds.c PROPERTIES
            COMPILE_OPTIONS "${TDSH_COMPAT_FLAG}")
    endif()
endif()

# Simulated terminal for end-to-end checks (see tools/tdsim.c).
add_executable(tdsim
    tools/tdsim.c
    tools/net_fake.c
    ports/common/host_main.c
    ports/common/td_fs_stdio.c
)
target_link_libraries(tdsim PRIVATE tinydesk_apps tinydesk_hal)
set_target_properties(tdsim PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(tdsim PRIVATE ${TD_WARNINGS})
if(TD_WITH_TDSH)
    target_sources(tdsim PRIVATE ports/common/tdsh_bridge_host.c ports/common/td_proto_cmds.c)
    target_compile_definitions(tdsim PRIVATE TD_WITH_TDSH)
    target_link_libraries(tdsim PRIVATE tdsh)
endif()

# Render a raw capture from real hardware (see tools/serial_probe.py).
add_executable(vtshot tools/vtshot.c)
target_link_libraries(vtshot PRIVATE tinydesk_core)
set_target_properties(vtshot PROPERTIES C_STANDARD 11)
target_compile_options(vtshot PRIVATE ${TD_WARNINGS})

# ------------------------------------------------------------ examples
if(TD_BUILD_EXAMPLES)
    foreach(ex hello_window counter_app)
        add_executable(${ex} examples/${ex}.c)
        target_link_libraries(${ex} PRIVATE tinydesk_core tinydesk_hal)
        set_target_properties(${ex} PROPERTIES C_STANDARD 11)
        target_compile_options(${ex} PRIVATE ${TD_WARNINGS})
    endforeach()
endif()

# --------------------------------------------------------------- tests
if(TD_BUILD_TESTS)
    enable_testing()
    add_executable(test_console_access ${TDSH_ROOT}/tests/test_console_access.c)
    target_include_directories(test_console_access PRIVATE ${TDSH_ROOT}/include)
    set_target_properties(test_console_access PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(test_console_access PRIVATE ${TD_WARNINGS})
    add_test(NAME test_console_access COMMAND test_console_access)
    foreach(t test_utf8 test_input test_render test_vterm test_wm test_widgets)
        add_executable(${t} tests/${t}.c)
        target_link_libraries(${t} PRIVATE tinydesk_core)
        set_target_properties(${t} PROPERTIES C_STANDARD 11)
        target_compile_options(${t} PRIVATE ${TD_WARNINGS})
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
    add_executable(test_proto tests/test_proto.c)
    target_link_libraries(test_proto PRIVATE tinydesk_proto)
    set_target_properties(test_proto PROPERTIES C_STANDARD 11)
    target_compile_options(test_proto PRIVATE ${TD_WARNINGS})
    add_test(NAME test_proto COMMAND test_proto)
    add_executable(test_editor tests/test_editor.c)
    target_link_libraries(test_editor PRIVATE tinydesk_apps)
    set_target_properties(test_editor PROPERTIES C_STANDARD 11)
    target_compile_options(test_editor PRIVATE ${TD_WARNINGS})
    add_test(NAME test_editor COMMAND test_editor)
endif()
