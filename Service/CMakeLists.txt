include_directories(inc)

add_executable(
  matching_service
  main.cpp
)

target_link_libraries(
    matching_service
    PUBLIC
        matching
        Threads::Threads
        CONAN_PKG::boost
)

if(ENABLE_TESTS)
    add_subdirectory(t)
endif()
