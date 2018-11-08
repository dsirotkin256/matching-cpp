include_directories(inc)
add_executable(
  matching_service
  main.cpp
)

if (ENABLE_CLANG_TIDY)
  message("Do clang tidy ${CMAKE_CXX_CLANG_TIDY}")
  set_target_properties(
    matching PROPERTIES
    CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY}"
  )
endif()

if(ENABLE_TESTS)
    add_subdirectory(t)
endif()
