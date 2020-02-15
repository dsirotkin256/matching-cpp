# Disable in-source builds to prevent source tree corruption.
if(" ${CMAKE_SOURCE_DIR}" STREQUAL " ${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "
FATAL: In-source builds are not allowed.
       You should create a separate directory for build files.
")
endif()

cmake_minimum_required (VERSION 3.10)
project(MatchingEngine)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)

find_package(Threads REQUIRED)

option(ENABLE_TESTS "Build tests" NO)
option(ENABLE_EXAMPLES "Build examples" NO)
option(ENABLE_BENCHMARKS "Build benchmarks" NO)

# Compiler configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -march=native -mtune=native -O3 -fexceptions -Wdeprecated -Wno-switch-enum -Wno-float-equal -Wno-unused-parameter -Wno-unused-member-function -Wno-exit-time-destructors -Wno-documentation-unknown-command -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-disabled-macro-expansion -Wno-weak-vtables -Wno-padded -fno-omit-frame-pointer -Werror -lpthread")
set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(CMAKE_HAVE_THREADS_LIBRARY TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
set(CMAKE_HAVE_PTHREAD_H TRUE)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

conan_basic_setup(TARGETS)

add_definitions(-DBOOST_ERROR_CODE_HEADER_ONLY)

add_subdirectory(Matching)

add_subdirectory(Service)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

