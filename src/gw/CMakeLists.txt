# mt_gateway_api: OrderGateway interface and client order id helpers, shared by every gateway.
add_library(mt_gateway_api STATIC order_gateway.cpp)
target_link_libraries(mt_gateway_api PUBLIC mt_core)

# mt_gateways: gateway thread loop, paper gateway and Kalshi demo gateway.
add_library(mt_gateways STATIC
  gateway_loop.cpp
  paper_gateway.cpp
  kalshi_rest_gateway.cpp
)
target_link_libraries(mt_gateways
  PUBLIC mt_gateway_api mt_md mt_io
  PRIVATE nlohmann_json::nlohmann_json)
