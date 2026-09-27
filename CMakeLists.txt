cmake_minimum_required(VERSION 3.28)
project(xrm VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/Options.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Targets.cmake)

if(XRM_BUILD_TESTING)
  include(CTest)
  add_subdirectory(tests)
endif()
