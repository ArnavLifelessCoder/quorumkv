cmake_minimum_required(VERSION 3.20)
project(quorumkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(QUORUMKV_SANITIZER "none" CACHE STRING "Sanitizer: none, address, or thread")
set_property(CACHE QUORUMKV_SANITIZER PROPERTY STRINGS none address thread)
if(NOT QUORUMKV_SANITIZER MATCHES "^(none|address|thread)$")
  message(FATAL_ERROR "Unknown QUORUMKV_SANITIZER: ${QUORUMKV_SANITIZER}")
endif()
if(WIN32 AND NOT MSVC)
  message(FATAL_ERROR "Windows builds require MSVC or clang-cl with the Windows SDK")
endif()
if(WIN32 AND QUORUMKV_SANITIZER STREQUAL "thread")
  message(FATAL_ERROR "ThreadSanitizer requires the Linux toolchain")
endif()

# Apply sanitizer instrumentation to dependencies as well as project code.
if(MSVC AND QUORUMKV_SANITIZER STREQUAL "address")
  foreach(flag_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    string(REGEX REPLACE "/RTC[1su]+" "" ${flag_var} "${${flag_var}}")
    string(REPLACE "/Od" "/O1" ${flag_var} "${${flag_var}}")
    string(REPLACE "/O2" "/O1" ${flag_var} "${${flag_var}}")
  endforeach()
  add_compile_options(/fsanitize=address /Zi)
  add_link_options(/INCREMENTAL:NO)
elseif(NOT MSVC AND QUORUMKV_SANITIZER STREQUAL "address")
  add_compile_options(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(NOT MSVC AND QUORUMKV_SANITIZER STREQUAL "thread")
  add_compile_options(-O1 -g -fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

add_library(quorumkv_options INTERFACE)
target_include_directories(quorumkv_options INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(quorumkv_options INTERFACE cxx_std_20)
if(MSVC)
  target_compile_options(quorumkv_options INTERFACE /W4 /WX /permissive- /Zc:__cplusplus)
else()
  target_compile_options(quorumkv_options INTERFACE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(quorumkv_options INTERFACE $<$<CONFIG:Release>:-march=native>)
endif()

include(CTest)
add_library(quorumkv src/rpc/codec.cpp src/raft/node.cpp src/sim/support.cpp src/sim/cluster.cpp)
target_link_libraries(quorumkv PUBLIC quorumkv_options)
if(BUILD_TESTING)
  include(FetchContent)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)
  add_executable(quorumkv_tests
    tests/main.cpp
    tests/toolchain_test.cpp
    tests/interfaces_test.cpp
    tests/codec_test.cpp
    tests/codec_fuzz_test.cpp
    tests/election_test.cpp
    tests/sim_support_test.cpp
    tests/sim_cluster_test.cpp
    tests/election_fuzz_test.cpp
  )
  target_link_libraries(quorumkv_tests PRIVATE quorumkv GTest::gtest)
  include(GoogleTest)
  gtest_discover_tests(quorumkv_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120)
endif()
