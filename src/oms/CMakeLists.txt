# mt_oms: order lifecycle, pre-trade risk, positions. Pure logic, no I/O.
add_library(mt_oms STATIC
  order_manager.cpp
  risk_manager.cpp
  position_keeper.cpp
)
target_link_libraries(mt_oms PUBLIC mt_core)
