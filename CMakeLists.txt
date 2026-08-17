cmake_minimum_required(VERSION 3.16)
project(tapto-code VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Dependencies -----------------------------------------------------------
# Fetched at configure time; pinned to known-good tags. Bump as needed.
include(FetchContent)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

# HTTPS support. OpenSSL must be installed and findable; on Windows/MSVC point
# CMake at it with -DOPENSSL_ROOT_DIR=C:/path/to/openssl if it isn't auto-found.
find_package(OpenSSL REQUIRED)

# Header-only; REQUIRE_OPENSSL makes httplib link OpenSSL (plus crypt32 on
# Windows) and propagate CPPHTTPLIB_OPENSSL_SUPPORT to consumers.
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE INTERNAL "")
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.15.3
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json httplib)

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

# --- Target -----------------------------------------------------------------
add_executable(tapto-code
  src/main.cpp
  src/config.cpp
  src/commands.cpp
  src/paths.cpp
  src/secret.cpp
  src/tools.cpp
  src/ui.cpp
  src/encoding.cpp
  src/claude.cpp
  src/openai.cpp
  src/gemini.cpp
)
target_include_directories(tapto-code PRIVATE include "${TAPTO_GENERATED_DIR}")
add_dependencies(tapto-code tapto_version)
# TAPTO_VERSION was defined here and read nowhere: main.cpp had moved on to
# TAPTO_CODE_VERSION with a hardcoded fallback, so the version was whatever the
# source said and never what the build said. It comes from the generated header
# now, which is the one place that can be right.
target_compile_definitions(tapto-code PRIVATE
  CPPHTTPLIB_OPENSSL_SUPPORT
)
target_link_libraries(tapto-code PRIVATE
  nlohmann_json::nlohmann_json
  httplib::httplib
  OpenSSL::SSL
  OpenSSL::Crypto
)

# CredReadW / CredFree, for reading API keys out of Windows Credential Manager.
# Most Windows toolchains link advapi32 by default; say so explicitly so the
# build doesn't depend on that.
if(WIN32)
  target_link_libraries(tapto-code PRIVATE advapi32)
endif()

if(MSVC)
  target_compile_options(tapto-code PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(tapto-code PRIVATE -Wall -Wextra)
endif()

install(TARGETS tapto-code RUNTIME DESTINATION bin)

# --- Tests ------------------------------------------------------------------
# Plain executables with assertions of their own, so `ctest` needs no test
# framework fetched or installed. Run with: ctest --test-dir build --output-on-failure
enable_testing()

add_executable(test-edits
  tests/test_edits.cpp
  src/tools.cpp
  src/commands.cpp
  src/config.cpp
  src/paths.cpp
)
target_include_directories(test-edits PRIVATE include)
target_link_libraries(test-edits PRIVATE nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(test-edits PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(test-edits PRIVATE -Wall -Wextra)
endif()

add_test(NAME edits COMMAND test-edits)
