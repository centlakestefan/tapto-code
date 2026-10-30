cmake_minimum_required(VERSION 3.16)
project(tapto-code VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- libtapto ---------------------------------------------------------------
# The config store, the secret resolver, provider resolution and the three
# provider clients with the agent loop inside are libtapto's, shared with
# tapto-word and tapto-vnc. It is fetched from its own repository at a tag --
# it used to be a copy vendored here, one of five kept byte-identical by hand
# -- and it brings its own dependency pins (nlohmann_json, cpp-httplib,
# OpenSSL), exported on the `tapto::tapto` target, so nothing is repeated here.
#
# Bump LIBTAPTO_PIN to take a library change. -DLIBTAPTO_TAG=main builds
# against an unreleased library, and -DFETCHCONTENT_SOURCE_DIR_LIBTAPTO=<path>
# against a working tree, which is how a change spanning both is developed.
#
# LIBTAPTO_TAG is a cache entry, and a cache entry's default never replaces a
# value already in the cache, so a bumped default would not reach a build
# folder configured before the bump: it kept building the old tag, silently.
# The pin is therefore a plain variable, and the cache remembers which pin it
# last filled in (_LIBTAPTO_PIN_SEEN): a LIBTAPTO_TAG still equal to that pin
# follows the new one; one set by hand to anything else is left alone. A
# cache from before this rule has no record, and its LIBTAPTO_TAG, a vX.Y.Z
# that this file put there, is moved to the pin once, with a message.
#
# What is NOT in the library, and is this program's own: the tool table
# (src/tools.cpp), the CLI and chat loop (src/main.cpp), and src/ui.cpp, which supplies the bodies for the
# tapto::ui functions the library declares -- a terminal status line here.
#
# The library's unit tests run under ctest with ours: this is its only build
# on MSVC, so a warning it trips here is fixed in the library and released.
set(LIBTAPTO_BUILD_TESTS ON CACHE BOOL "" FORCE)
include(FetchContent)
set(LIBTAPTO_PIN "v0.9.0")
if(NOT DEFINED CACHE{LIBTAPTO_TAG})
  set(LIBTAPTO_TAG "${LIBTAPTO_PIN}" CACHE STRING "libtapto version to build against")
elseif(NOT LIBTAPTO_TAG STREQUAL LIBTAPTO_PIN)
  if(DEFINED CACHE{_LIBTAPTO_PIN_SEEN})
    string(COMPARE EQUAL "${LIBTAPTO_TAG}" "${_LIBTAPTO_PIN_SEEN}" _follow_pin)
  elseif(LIBTAPTO_TAG MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(_follow_pin TRUE)
  else()
    set(_follow_pin FALSE)
  endif()
  if(_follow_pin)
    message(STATUS "libtapto: ${LIBTAPTO_TAG} -> ${LIBTAPTO_PIN} (the pin moved)")
    set(LIBTAPTO_TAG "${LIBTAPTO_PIN}" CACHE STRING "libtapto version to build against" FORCE)
  else()
    message(STATUS "libtapto: building ${LIBTAPTO_TAG}, set by hand (the pin is ${LIBTAPTO_PIN})")
  endif()
endif()
set(_LIBTAPTO_PIN_SEEN "${LIBTAPTO_PIN}" CACHE INTERNAL "the pin LIBTAPTO_TAG last followed")
# A -DLIBTAPTO_TAG=... without a type leaves the entry UNINITIALIZED.
set_property(CACHE LIBTAPTO_TAG PROPERTY TYPE STRING)
set_property(CACHE LIBTAPTO_TAG PROPERTY HELPSTRING
  "libtapto version to build against; follows LIBTAPTO_PIN unless set to another")
FetchContent_Declare(libtapto
  GIT_REPOSITORY https://github.com/centlakestefan/libtapto.git
  GIT_TAG        ${LIBTAPTO_TAG}
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libtapto)

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
# IDE or a generator to parse the sources that include it, and a tag that does
# not match project() stops the configure.
#
# project() holds the next release: raise it right after tagging one, so a
# build of main is that version's dev build. See cmake/GitVersion.cmake.
execute_process(COMMAND ${CMAKE_COMMAND} ${TAPTO_VERSION_ARGS} RESULT_VARIABLE _version_result)
if(NOT _version_result EQUAL 0)
  message(FATAL_ERROR "the version stamp failed (above)")
endif()
# The version this build is, for a packaging project that builds this one as
# a subdirectory (its file names; an MSI's own version stays project()'s,
# since Windows takes numbers only there). As of the configure, which is what
# a release build runs from.
file(STRINGS "${TAPTO_VERSION_HEADER}" _version_line REGEX "^#define TAPTO_CODE_VERSION ")
string(REGEX MATCH "\"([^\"]*)\"" _version_quoted "${_version_line}")
set(TAPTO_CODE_VERSION_FULL "${CMAKE_MATCH_1}")
message(STATUS "tapto-code ${TAPTO_CODE_VERSION_FULL}")

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
  src/prompt.cpp
  src/session.cpp
  src/tools.cpp
  src/ui.cpp
)
target_include_directories(tapto-code PRIVATE include "${TAPTO_GENERATED_DIR}")
add_dependencies(tapto-code tapto_version)
# The library exports nlohmann_json, httplib, OpenSSL, Threads and, on Windows,
# advapi32/crypt32/wintrust (dl elsewhere, for its plugin loader), together
# with CPPHTTPLIB_OPENSSL_SUPPORT.
target_link_libraries(tapto-code PRIVATE tapto::tapto)
tapto_code_warnings(tapto-code)

install(TARGETS tapto-code RUNTIME DESTINATION bin)

# --- Plugins ----------------------------------------------------------------
# The plugins (image, jira, mcp, memory, skills) are libtapto's, built by its
# CMakeLists into a `plugins` folder beside tapto-code and installed to
# bin/plugins (lib/tapto-code/plugins outside Windows) with tapto_plugin(),
# which a plugin of tapto-code's own would use too. tapto-code is built after
# them, so a build in build/ finds them. TAPTO_CODE_PLUGINS keeps the list
# under the name the installer reads (every plugin it signs and scans for
# runtime DLLs).
get_property(_tapto_plugins GLOBAL PROPERTY TAPTO_PLUGINS)
if(_tapto_plugins)
  add_dependencies(tapto-code ${_tapto_plugins})
  set_property(GLOBAL PROPERTY TAPTO_CODE_PLUGINS ${_tapto_plugins})
endif()

# --- Tests ------------------------------------------------------------------
# Plain executables with assertions of their own, so `ctest` needs no test
# framework fetched or installed. Run with: ctest --test-dir build --output-on-failure
#
# The library's own test (test-libtapto, covering the config store, secrets,
# the /compact trimmer and more) is registered by libtapto's own CMakeLists.
enable_testing()

# The text editor tool's line-ending behaviour and the status-line labels.
# Links the library for config/paths and the library's do-nothing ui: a static
# library only pulls the objects a test references, and the day one of them
# reaches provider.cpp it would otherwise fail to link on tapto::ui::print_error.
add_executable(test-edits
  tests/test_edits.cpp
  src/tools.cpp
  "${libtapto_SOURCE_DIR}/test/ui_null.cpp"
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

# System prompt composition: system-prompt(-file) and the -append keys, and
# how a prompt file is found, read and, when unusable, skipped.
add_executable(test-prompt
  tests/test_prompt.cpp
  src/prompt.cpp
)
target_include_directories(test-prompt PRIVATE include)
target_link_libraries(test-prompt PRIVATE tapto::tapto)
tapto_code_warnings(test-prompt)

add_test(NAME prompt COMMAND test-prompt)

# The saved conversation behind --resume: what a save writes reads back the
# same, and a damaged or foreign file is refused rather than half-loaded.
add_executable(test-session
  tests/test_session.cpp
  src/session.cpp
)
target_include_directories(test-session PRIVATE include)
target_link_libraries(test-session PRIVATE tapto::tapto)
tapto_code_warnings(test-session)

add_test(NAME session COMMAND test-session)

# The memory plugin and the plugin folder it lives in: where it is (beside the
# local config, under the same encoded working directory), who may reach it
# (the editor tool alone, by a spelling that resolves to it -- no other tool,
# no command cwd or %p -- and only while it is loaded rw), and what the system
# prompt carries of it (the first 50 lines, the index). Loads the module built
# moments ago, so it compiles libtapto's loader itself, without the signature
# check (the library's copy is then never linked in).
add_executable(test-memory
  tests/test_memory.cpp
  "${libtapto_SOURCE_DIR}/src/plugins.cpp"
  src/tools.cpp
  "${libtapto_SOURCE_DIR}/test/ui_null.cpp"
)
target_include_directories(test-memory PRIVATE include)
target_compile_definitions(test-memory PRIVATE TAPTO_PLUGINS_ALLOW_UNSIGNED)
target_link_libraries(test-memory PRIVATE tapto::tapto)
add_dependencies(test-memory tapto-plugin-memory)
tapto_code_warnings(test-memory)

add_test(NAME memory COMMAND test-memory $<TARGET_FILE:tapto-plugin-memory>)

# Tool output too long for the conversation: the head and tail inline, the
# whole of it in the session's @tmp folder, which the model can read and
# nothing can write.
add_executable(test-overflow
  tests/test_overflow.cpp
  src/tools.cpp
  "${libtapto_SOURCE_DIR}/test/ui_null.cpp"
)
target_include_directories(test-overflow PRIVATE include)
target_link_libraries(test-overflow PRIVATE tapto::tapto)
tapto_code_warnings(test-overflow)

add_test(NAME overflow COMMAND test-overflow)

# The plugin loader and the plugins are tested by libtapto's own tests
# (test-plugins), which run here too.

# Markdown table rendering: wrapping (not clipping), alignment, inline styling
# and CJK width are exercised at explicit widths via render_markdown (pure,
# no stdout).
add_executable(test-ui
  tests/test_ui.cpp
  src/ui.cpp
)
target_include_directories(test-ui PRIVATE include)
target_link_libraries(test-ui PRIVATE tapto::tapto)
tapto_code_warnings(test-ui)

add_test(NAME ui COMMAND test-ui)
