#include "lob/stochastic/event_types.hpp"

#include <stdexcept>

namespace lob::stochastic {

EventType event_type_from_index(const std::size_t index) {
  if (index >= kEventTypeCount) {
    throw std::out_of_range{"event type index out of range"};
  }
  return static_cast<EventType>(index);
}

}  // namespace lob::stochastic
