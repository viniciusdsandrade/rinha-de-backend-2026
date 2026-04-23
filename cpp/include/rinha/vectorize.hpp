#pragma once

#include <string>

#include "rinha/types.hpp"

namespace rinha {

bool vectorize(const Payload& payload, QueryVector& vector, std::string& error);

}  // namespace rinha
