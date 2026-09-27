include(FetchContent)

# Source archives live outside every build tree. Each preset still compiles its
# own dependency objects in its own binary directory.
set(XRM_DEPENDENCY_SOURCES "${PROJECT_SOURCE_DIR}/.deps/sources" CACHE PATH
    "Reusable source directory for fetched XRM dependencies")

find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libarchive
  URL https://codeload.github.com/libarchive/libarchive/tar.gz/refs/tags/v3.8.9
  URL_HASH SHA256=744346f6bca195c8f894f847bb32a16e9bcae6002624a58fadc81e80f595b3cb
  SOURCE_DIR "${XRM_DEPENDENCY_SOURCES}/libarchive"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(libarchive)

set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libuv
  URL https://github.com/libuv/libuv/archive/refs/tags/v1.51.0.tar.gz
  SOURCE_DIR "${XRM_DEPENDENCY_SOURCES}/libuv"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(libuv)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
  SOURCE_DIR "${XRM_DEPENDENCY_SOURCES}/nlohmann_json"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.5.0.tar.gz
  SOURCE_DIR "${XRM_DEPENDENCY_SOURCES}/cli11"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(CLI11)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui_dep
  URL https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/refs/tags/v7.0.1
  SOURCE_DIR "${XRM_DEPENDENCY_SOURCES}/ftxui_dep"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(ftxui_dep)

if(XRM_BUILD_TESTING)
  FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
    SOURCE_DIR "${XRM_DEPENDENCY_SOURCES}/catch2"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
