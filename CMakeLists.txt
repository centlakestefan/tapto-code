cmake_minimum_required(VERSION 3.16)
project(tapto-code VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- libtapto ---------------------------------------------------------------
# The config store, the secret resolver, provider resolution and the three
# provider clients with the agent loop inside are libtapto's, shared with
# tapto-word and tapto-vnc. It is vendored here as a byte-identical copy of the
# one in tapto-word (diff -r against ../tapto-word/libtapto must be empty), and
# it brings its own dependency pins (nlohmann_json, cpp-httplib, OpenSSL),
# exported on the `tapto::tapto` target, so nothing is repeated here.
#
# What is NOT in the library, and is this program's own: the tool table
# (src/tools.cpp), the allow-listed commands (src/commands.cpp), the CLI and
# chat loop (src/main.cpp), and src/ui.cpp, which supplies the bodies for the
# tapto::ui functions the library declares -- a terminal status line here.
#
# The library's unit tests run under ctest with ours: this is its only build
# on MSVC, so a warning it trips here is fixed upstream and re-copied.
set(LIBTAPTO_BUILD_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(libtapto)

# --- Version stamp ----------------------------------------------------------
# Puts the version and the originating commit into the binary, so `version` and
# the first line of every trace name the build that produced them. Regenerated
# on each build, not at configure time; see cmake/GitVersion.cmake.
set(TAPTO_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(TAPTO_VERSION_HEADER "${TAPTO_GENERATED_DIR}/tapto/version.h")

set(TAPTO_VERSION_ARGS
  -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
  -DIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
  -DOUT_FILE=${TAPTO_VERSION_HEADER}
  -DPROJECT_VERSION=${PROJECT_VERSION}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake
)

# Once at configure time as well, so the header exists before anything asks an
# IDE or a generator to parse the sources that include it.
execute_process(COMMAND ${CMAKE_COMMAND} ${TAPTO_VERSION_ARGS})

add_custom_target(tapto_version ALL
  COMMAND ${CMAKE_COMMAND} ${TAPTO_VERSION_ARGS}
  BYPRODUCTS ${TAPTO_VERSION_HEADER}
  COMMENT "Stamping version ${PROJECT_VERSION}"
  VERBATIM
)

function(tapto_code_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /EHsc /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# --- Target -----------------------------------------------------------------
add_executable(tapto-code
  src/main.cpp
  src/commands.cpp
  src/tools.cpp
  src/ui.cpp
)
target_include_directories(tapto-code PRIVATE include "${TAPTO_GENERATED_DIR}")
add_dependencies(tapto-code tapto_version)
# The library exports nlohmann_json, httplib, OpenSSL, Threads and, on Windows,
# advapi32/crypt32, together with CPPHTTPLIB_OPENSSL_SUPPORT.
target_link_libraries(tapto-code PRIVATE tapto::tapto)
tapto_code_warnings(tapto-code)

install(TARGETS tapto-code RUNTIME DESTINATION bin)

# --- Tests ------------------------------------------------------------------
# Plain executables with assertions of their own, so `ctest` needs no test
# framework fetched or installed. Run with: ctest --test-dir build --output-on-failure
#
# The library's own test (test-libtapto, covering the config store, secrets,
# the /compact trimmer and more) is registered by libtapto/CMakeLists.txt.
enable_testing()

# The text editor tool's line-ending behaviour and the status-line labels.
# Links the library for config/paths and the library's do-nothing ui: a static
# library only pulls the objects a test references, and the day one of them
# reaches provider.cpp it would otherwise fail to link on tapto::ui::print_error.
add_executable(test-edits
  tests/test_edits.cpp
  src/tools.cpp
  src/commands.cpp
  libtapto/test/ui_null.cpp
)
target_include_directories(test-edits PRIVATE include)
target_link_libraries(test-edits PRIVATE tapto::tapto)
tapto_code_warnings(test-edits)

add_test(NAME edits COMMAND test-edits)
# Config store: set/unset/save must preserve comments, blank lines, and ordering
# a user added by hand, rather than rebuilding the file from its key/value table.
# Config now lives in libtapto (src/config.cpp moved there in the re-org), so
# this links the library instead of compiling the source in; the /compact
# trimmer test was folded into libtapto's test-libtapto.
add_executable(test-config
  tests/test_config.cpp
)
target_include_directories(test-config PRIVATE include)
target_link_libraries(test-config PRIVATE tapto::tapto)
tapto_code_warnings(test-config)

add_test(NAME config COMMAND test-config)
