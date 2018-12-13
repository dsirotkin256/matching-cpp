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

option(LLVM_ENABLE_DEBUG "Compile a debug version when using LLVM/clang" NO)
option(ENABLE_TESTS "Build tests" NO)
option(ENABLE_EXAMPLES "Build examples" NO)
option(ENABLE_BENCHMARKS "Build benchmarks" NO)

# Compiler configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-standalone-debug -march=native -mtune=native -O3 -fexceptions -Wdeprecated -Wno-switch-enum -Wno-float-equal -Wno-unused-parameter -Wno-unused-member-function -Wno-exit-time-destructors -Wno-documentation-unknown-command -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-disabled-macro-expansion -Wno-weak-vtables -Wno-padded")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lc++abi -lc++ -lpthread -ljemalloc")
set(CMAKE_BUILD_TYPE Release)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(CMAKE_HAVE_THREADS_LIBRARY TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
set(CMAKE_HAVE_PTHREAD_H TRUE)
find_package(Threads REQUIRED)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

conan_basic_setup(TARGETS)

add_definitions(-DBOOST_ERROR_CODE_HEADER_ONLY)

add_subdirectory(Matching)

add_subdirectory(Service)

enable_testing()

