# Crashpad crash-reporting integration (design: docs/crash-reporting.md).
#
# Builds from the three submodules under third_party/ with a POSIX(Linux)
# source set. One static library carries everything (client, snapshot,
# minidump, handler, util, mini_chromium); the shield runtime links it for
# the in-process client API and the standalone crashpad_handler executable
# links the same library plus its POSIX entry point. Static-archive
# semantics keep unused objects out of each consumer.
#
# Upstream DEPS nesting is flattened (git forbids gitlinks inside a
# gitlink); the include roots below make the upstream #include forms
# resolve unchanged:
#   -I third_party/crpad   -> "client/...", "third_party/lss/lss.h"
#   -I third_party/mini_chromium -> "base/...", "build/build_config.h"
#   -I <repo root>         -> "third_party/lss/lss/linux_syscall_support.h"
#                             (CRASHPAD_LSS_SOURCE_EMBEDDED path)
#
# HTTP transport uses the dlopen()-based libcurl implementation, so no
# libcurl link dependency is introduced (upload itself stays disabled by
# default; see docs/crash-reporting.md).

set(CRASHPAD_ROOT "${PROJECT_SOURCE_DIR}/third_party/crashpad")
set(CRASHPAD_MINI_CHROMIUM_ROOT
    "${PROJECT_SOURCE_DIR}/third_party/mini_chromium")

if(NOT EXISTS "${CRASHPAD_ROOT}/client/crashpad_client.h"
   OR NOT EXISTS "${CRASHPAD_MINI_CHROMIUM_ROOT}/build/build_config.h"
   OR NOT EXISTS
       "${PROJECT_SOURCE_DIR}/third_party/lss/lss/linux_syscall_support.h")
  message(FATAL_ERROR
      "SHIELD_ENABLE_CRASHPAD=ON but a crashpad submodule is missing. "
      "Run: git submodule update --init")
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# Collect .cc/.h from one crashpad directory, dropping platform-specific
# and test files by filename pattern (win/mac/ios/fuchsia/android/cros are
# not part of the Linux source set upstream builds either). Test files come
# in both spellings: *_test.cc and test_*.cc (test_modules.cc,
# test_output_stream.cc) — the latter needs gtest headers that non-test
# builds do not carry.
function(shield_crashpad_glob outvar dir)
  file(GLOB entries LIST_DIRECTORIES false
       "${CRASHPAD_ROOT}/${dir}/*.cc"
       "${CRASHPAD_ROOT}/${dir}/*.h")
  set(acc "")
  foreach(f IN LISTS entries)
    get_filename_component(name "${f}" NAME)
    if(name MATCHES
           "(^test_.*|_test.*|_fuzzer.*|_win|_mac|_ios|_tvos|_fuchsia|_android|_cros)\\.(cc|h)$")
      continue()
    endif()
    list(APPEND acc "${f}")
  endforeach()
  set(${outvar} "${acc}" PARENT_SCOPE)
endfunction()

set(CRASHPAD_SOURCE_DIRS
    client
    minidump
    snapshot
    snapshot/crashpad_types
    snapshot/elf
    snapshot/linux
    snapshot/minidump
    snapshot/posix
    snapshot/sanitized
    snapshot/x86
    handler
    handler/linux
    tools
    util
    util/file
    util/linux
    util/misc
    util/net
    util/numeric
    util/posix
    util/process
    util/stdlib
    util/stream
    util/string
    util/synchronization
    util/thread)

set(CRASHPAD_SOURCES "")
foreach(d IN LISTS CRASHPAD_SOURCE_DIRS)
  shield_crashpad_glob(dir_sources "${d}")
  list(APPEND CRASHPAD_SOURCES ${dir_sources})
endforeach()

# dlopen()-based libcurl transport instead of socket+boringssl, the system
# zlib header via upstream's wrapper, and the compat shim sources. The
# android-only handler_trampoline executable and initial signal
# dispositions sources stay out (upstream gates both on crashpad_is_android;
# client code only passes the trampoline path as a string).
list(REMOVE_ITEM CRASHPAD_SOURCES
     "${CRASHPAD_ROOT}/util/net/http_transport_socket.cc"
     "${CRASHPAD_ROOT}/handler/linux/handler_trampoline.cc"
     "${CRASHPAD_ROOT}/util/linux/initial_signal_dispositions.cc")
list(APPEND CRASHPAD_SOURCES
     "${CRASHPAD_ROOT}/util/net/http_transport_libcurl.cc"
     "${CRASHPAD_ROOT}/compat/linux/sys/mman_memfd_create.cc")

file(GLOB_RECURSE CRASHPAD_MINI_CHROMIUM_SOURCES
     "${CRASHPAD_MINI_CHROMIUM_ROOT}/base/*.cc"
     "${CRASHPAD_MINI_CHROMIUM_ROOT}/base/*.h")
list(FILTER CRASHPAD_MINI_CHROMIUM_SOURCES EXCLUDE REGEX
     "/(apple|mac|win|fuchsia)/")
list(FILTER CRASHPAD_MINI_CHROMIUM_SOURCES EXCLUDE REGEX
     "(^test_.*|_test.*|_win|_mac|_apple|_fuchsia)\\.(cc|h)$")

add_library(crashpad_client STATIC
    ${CRASHPAD_SOURCES}
    ${CRASHPAD_MINI_CHROMIUM_SOURCES})

target_include_directories(crashpad_client PUBLIC
    "${CRASHPAD_ROOT}"
    "${CRASHPAD_MINI_CHROMIUM_ROOT}"
    "${PROJECT_SOURCE_DIR}")

# Upstream compat layer (compat/BUILD.gn): compat/linux/signal.h is an
# include_next shim that supplies SS_AUTODISARM / SA_EXPOSE_TAGBITS missing
# from glibc; compat/non_win provides windows.h & friends so win-shaped
# minidump headers compile on Linux. Header-only besides the one .cc
# picked up below.
target_include_directories(crashpad_client PUBLIC
    "${CRASHPAD_ROOT}/compat/linux"
    "${CRASHPAD_ROOT}/compat/non_win")

set_target_properties(crashpad_client PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

target_compile_definitions(crashpad_client PUBLIC
    CRASHPAD_LSS_SOURCE_EMBEDDED
    CRASHPAD_ZLIB_SOURCE_SYSTEM
    # Upstream builds zlib with ZLIB_CONST (util/BUILD.gn), which makes
    # zlib.h's z_const (next_in et al.) actually const.
    ZLIB_CONST)

# Upstream builds assume GNU extensions on Linux (SS_AUTODISARM /
# SA_EXPOSE_TAGBITS in glibc's signal.h sit behind __USE_GNU).
target_compile_definitions(crashpad_client PRIVATE _GNU_SOURCE)

target_link_libraries(crashpad_client PUBLIC
    Threads::Threads
    ZLIB::ZLIB
    ${CMAKE_DL_LIBS})

# Third-party code: keep its warnings out of the shield build log.
if(MSVC)
  target_compile_options(crashpad_client PRIVATE /W0)
else()
  target_compile_options(crashpad_client PRIVATE -w)
endif()

# Standalone out-of-process handler, installed next to the shield binary
# (bin/; the runtime locates it there). The target's only source is an
# absolute path, which by default suppresses CMAKE_RUNTIME_OUTPUT_DIRECTORY
# — pin the output explicitly.
add_executable(crashpad_handler "${CRASHPAD_ROOT}/handler/main.cc")
set_target_properties(crashpad_handler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(crashpad_handler PRIVATE crashpad_client)
