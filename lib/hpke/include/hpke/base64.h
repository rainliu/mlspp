#pragma once

#include <bytes/bytes.h>
using namespace bytes_ns;

namespace hpke {

std::string
to_base64(const bytes& data);

std::string
to_base64url(const bytes& data);

bytes
from_base64(const std::string& enc);

bytes
from_base64url(const std::string& enc);

} // namespace hpke