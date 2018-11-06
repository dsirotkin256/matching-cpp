
# Disable in-source builds to prevent source tree corruption.
if(" ${CMAKE_SOURCE_DIR}" STREQUAL " ${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "
FATAL: In-source builds are not allowed.
       You should create a separate directory for build files.
")
endif()

cmake_minimum_required (VERSION 3.10)
project(MatchingEngine)

option(LLVM_ENABLE_DEBUG "Compile a debug version when using LLVM/clang" NO)
option(ENABLE_TESTS "Build tests" NO)
option(ENABLE_EXAMPLES "Build examples" NO)
option(ENABLE_BENCHMARKS "Build benchmarks" NO)

# Compiler configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcxx-exceptions -fexceptions -fcoroutines-ts -stdlib=libc++ -Weverything -Wdocumentation -Wdeprecated -Wno-switch-enum -Wno-float-equal -Wno-unused-parameter -Wno-unused-member-function -Wno-exit-time-destructors -Wno-documentation-unknown-command -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-disabled-macro-expansion -Wno-weak-vtables -Wno-padded -Wno-missing-prototypes")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++abi -lc++")

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(CMAKE_HAVE_THREADS_LIBRARY TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
set(CMAKE_HAVE_PTHREAD_H TRUE)
find_package(Threads)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(matcher main.cpp)

find_package(Boost 1.66)
if(Boost_FOUND)
    message("Boost Found")
elseif(NOT Boost_FOUND)
    error("Boost Not Found")
    include_directories(${Boost_INCLUDE_DIRS})
    target_link_libraries(matcher ${Boost_LIBRARIES})
endif()
