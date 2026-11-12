# mt_io: HTTPS, WebSockets, Kalshi request signing, stats, control channel.
add_library(mt_io STATIC
  http_client.cpp
  websocket_client.cpp
  auth.cpp
  stats.cpp
  control_channel.cpp
)
target_link_libraries(mt_io
  PUBLIC mt_core
  PRIVATE ixwebsocket::ixwebsocket OpenSSL::Crypto)
