cmake_minimum_required(VERSION 3.29)
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
option(QUORUMKV_BUILD_MUTATION_TESTS "Build test-only unsafe election simulator executables" OFF)
if(QUORUMKV_BUILD_MUTATION_TESTS AND NOT BUILD_TESTING)
  message(FATAL_ERROR "Mutation executables require BUILD_TESTING")
endif()
add_library(quorumkv src/rpc/codec.cpp src/raft/node.cpp src/raft/log.cpp src/raft/file_storage.cpp src/sim/support.cpp src/sim/cluster.cpp src/sim/replication_safety.cpp)
if(WIN32)
  target_sources(quorumkv PRIVATE src/platform/windows/file.cpp)
else()
  target_sources(quorumkv PRIVATE src/platform/posix/file.cpp)
endif()
target_link_libraries(quorumkv PUBLIC quorumkv_options)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
    tests/log_test.cpp
    tests/replication_test.cpp
    tests/replication_safety_test.cpp
    tests/figure8_test.cpp
    tests/file_storage_test.cpp
    tests/sim_support_test.cpp
    tests/sim_cluster_test.cpp
    tests/election_fuzz_test.cpp
  )
  target_link_libraries(quorumkv_tests PRIVATE quorumkv GTest::gtest)
  target_compile_definitions(quorumkv_tests PRIVATE QUORUMKV_TEST_ROOT="${PROJECT_BINARY_DIR}")
  include(GoogleTest)
  set_property(TARGET quorumkv_tests PROPERTY TEST_LAUNCHER
    "${Python3_EXECUTABLE};${PROJECT_SOURCE_DIR}/scripts/test_watchdog.py")
  gtest_discover_tests(quorumkv_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120
    TEST_LIST quorumkv_test_names PROPERTIES TIMEOUT 90)
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${PROJECT_SOURCE_DIR}/tests/timeouts.cmake")
  if(QUORUMKV_BUILD_MUTATION_TESTS)
    foreach(mutation IN ITEMS COUNT_STALE_VOTES REPLY_BEFORE_PERSIST)
      string(TOLOWER "${mutation}" suffix)
      # The normal quorumkv library never receives these definitions. Its node object
      # is not extracted from the archive when the executable supplies its own node.
      add_executable(quorumkv_mutant_${suffix}
        src/raft/node.cpp tests/main.cpp tests/election_fuzz_test.cpp tests/election_test.cpp)
      target_compile_definitions(quorumkv_mutant_${suffix} PRIVATE QUORUMKV_TEST_${mutation})
      target_link_libraries(quorumkv_mutant_${suffix} PRIVATE quorumkv GTest::gtest)
    endforeach()
  endif()
endif()
