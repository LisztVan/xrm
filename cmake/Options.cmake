set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(XRM_BUILD_TESTING "Build XRM tests" ON)
option(XRM_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(XRM_REQUIRE_REAL_XRAY "Require XRM_TEST_XRAY to name an executable Xray core" OFF)
set(XRM_TEST_XRAY "" CACHE FILEPATH "Optional real Xray binary for real-core tests")
if(NOT XRM_TEST_XRAY AND DEFINED ENV{XRM_TEST_XRAY})
  set(XRM_TEST_XRAY "$ENV{XRM_TEST_XRAY}" CACHE FILEPATH
      "Optional real Xray binary for real-core tests" FORCE)
endif()
if(XRM_REQUIRE_REAL_XRAY)
  if(NOT XRM_TEST_XRAY OR NOT IS_ABSOLUTE "${XRM_TEST_XRAY}" OR
     NOT EXISTS "${XRM_TEST_XRAY}")
    message(FATAL_ERROR
      "XRM_REQUIRE_REAL_XRAY=ON requires XRM_TEST_XRAY to name an absolute Xray binary")
  endif()
endif()

if(XRM_ENABLE_ASAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "XRM_ENABLE_ASAN requires GCC or Clang")
  endif()
endif()
