#pragma once

#include <string>
#include <string_view>

#include "rinha/types.hpp"

namespace rinha {

bool parse_payload(std::string_view body, Payload& payload, std::string& error);

}  // namespace rinha
