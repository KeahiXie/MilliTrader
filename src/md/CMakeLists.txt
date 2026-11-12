# mt_md: Kalshi market data - REST and WebSocket parsing, order book, gap recovery.
add_library(mt_md STATIC
  market_order_book.cpp
  kalshi_book_builder.cpp
  kalshi_rest.cpp
  kalshi_ws_protocol.cpp
  market_data_consumer.cpp
)
target_link_libraries(mt_md
  PUBLIC mt_io
  PRIVATE nlohmann_json::nlohmann_json)
