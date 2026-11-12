# mt_core: value types, clocks, the SPSC queue, thread helpers. No I/O.
add_library(mt_core STATIC
  types.cpp
  clock.cpp
  instruments.cpp
)
target_include_directories(mt_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mt_core PUBLIC Threads::Threads)
