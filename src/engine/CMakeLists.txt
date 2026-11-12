# mt_engine: the trading engine, its run loop, the journal and replay.
add_library(mt_engine STATIC
  strategy.cpp
  trade_engine.cpp
  journal.cpp
  replay.cpp
  engine_loop.cpp
)
target_link_libraries(mt_engine PUBLIC mt_oms mt_md mt_io)

# Stamp git describe into every journal so replay knows which build wrote it (configure time).
execute_process(COMMAND git describe --always --dirty
                WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                OUTPUT_VARIABLE MT_GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT MT_GIT_DESCRIBE)
  set(MT_GIT_DESCRIBE "unknown")
endif()
target_compile_definitions(mt_engine PRIVATE MT_BUILD_ID="${MT_GIT_DESCRIBE}")
