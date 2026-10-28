#include "lob/core/types.hpp"

namespace lob::core {

void require_valid_timestamp(const TimestampNs timestamp) {
  if (!is_valid_timestamp(timestamp)) {
    throw std::invalid_argument{"timestamp must be nonnegative nanoseconds"};
  }
}

void require_valid_quantity(const QuantityLots quantity) {
  if (!is_valid_quantity(quantity)) {
    throw std::invalid_argument{"quantity must be positive lots"};
  }
}

}  // namespace lob::core
