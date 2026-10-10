#include "pulse/engine/digest.hpp"

#include <cstdio>

namespace pulse {

std::string Digest::hex() const {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h_));
  return buf;
}

}  // namespace pulse
