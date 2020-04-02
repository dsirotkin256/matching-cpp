add_executable(
  matching_service
  main.cpp
)

target_link_libraries(
  matching_service
  PRIVATE
    matching
    Threads::Threads
    CONAN_PKG::boost
    CONAN_PKG::nlohmann_json
    CONAN_PKG::jemalloc
    CONAN_PKG::tbb
    CONAN_PKG::folly
)
