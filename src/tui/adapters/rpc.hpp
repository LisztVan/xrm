#pragma once
#include "model/common.hpp"
#include <string>
namespace xrm::tui {
Result<Json> request(const std::string &socket, std::string method,
                     Json params = Json::object());
Result<Json> inventory(const std::string &socket);
} // namespace xrm::tui
