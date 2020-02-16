file(GLOB MATCHING_SOURCES src/*.cpp inc/*.h)
include_directories(inc)

add_library(matching ${MATCHING_SOURCES})

target_link_libraries(
    matching
    PUBLIC
    CONAN_PKG::spdlog
    CONAN_PKG::jemalloc
)

target_include_directories(
    matching
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include/matching>
)
