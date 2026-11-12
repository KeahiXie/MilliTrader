# mt_fix: FIX 4.4 codec, session layer, TCP helpers and FixGateway (std + POSIX sockets only).
add_library(mt_fix STATIC
  codec.cpp
  sequence_store.cpp
  session.cpp
  tcp.cpp
  fix_gateway.cpp
)
target_include_directories(mt_fix PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mt_fix PUBLIC mt_gateway_api mt_core)
