#include "gw/order_gateway.hpp"

#include <charconv>

namespace mt {

std::string make_client_order_id(uint32_t session_id, OrderId order_id) {
    return "mt" + std::to_string(session_id) + "-" + std::to_string(order_id);
}

OrderId parse_client_order_id(std::string_view client_order_id, uint32_t session_id) {
    const std::string prefix = "mt" + std::to_string(session_id) + "-";
    if (client_order_id.substr(0, prefix.size()) != prefix) return 0;
    const std::string_view digits = client_order_id.substr(prefix.size());
    OrderId id = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (error != std::errc{} || end != digits.data() + digits.size()) return 0;
    return id;
}

}  // namespace mt
