cmake_minimum_required(VERSION 3.16)
project(tapto-code VERSION 0.1.0 LANGUAGES CXX)

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

# --- Target -----------------------------------------------------------------
add_executable(tapto-code
  src/main.cpp
  src/config.cpp
  src/commands.cpp
  src/paths.cpp
  src/tools.cpp
  src/ui.cpp
  src/claude.cpp
  src/openai.cpp
  src/gemini.cpp
)
target_include_directories(tapto-code PRIVATE include)
target_compile_definitions(tapto-code PRIVATE
  TAPTO_VERSION="${PROJECT_VERSION}"
  CPPHTTPLIB_OPENSSL_SUPPORT
)
target_link_libraries(tapto-code PRIVATE
  nlohmann_json::nlohmann_json
  httplib::httplib
  OpenSSL::SSL
  OpenSSL::Crypto
)

if(MSVC)
  target_compile_options(tapto-code PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(tapto-code PRIVATE -Wall -Wextra)
endif()

install(TARGETS tapto-code RUNTIME DESTINATION bin)

# --- Test driver: wide-table clip exercise ----------------------------------
add_executable(test-table src/test_table.cpp src/ui.cpp)
target_include_directories(test-table PRIVATE include)
if(MSVC)
  target_compile_options(test-table PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(test-table PRIVATE -Wall -Wextra)
endif()
