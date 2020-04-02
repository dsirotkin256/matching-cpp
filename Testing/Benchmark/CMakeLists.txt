add_executable(
  benchmark
  main.cpp
)

target_link_libraries(
  benchmark
  PRIVATE
    matching
    Threads::Threads
    CONAN_PKG::jemalloc
    CONAN_PKG::benchmark
)

