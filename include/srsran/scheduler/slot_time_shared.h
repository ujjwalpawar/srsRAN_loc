// Header-only shared slot tracker used for best-effort timing correlation.
#pragma once

#include "srsran/ran/slot_point.h"
#include <array>
#include <mutex>

namespace srsran {

struct slot_time_shared {
  static constexpr unsigned max_numerologies = 5;
  inline static std::mutex                                  mtx{};
  inline static std::array<slot_point, max_numerologies>    last_slots{};
};

inline void update_last_slot(slot_point slot)
{
  if (!slot.valid()) {
    return;
  }
  std::lock_guard<std::mutex> lock(slot_time_shared::mtx);
  const unsigned              numerology = slot.numerology();
  if (numerology < slot_time_shared::max_numerologies) {
    slot_time_shared::last_slots[numerology] = slot;
  }
}

inline bool get_last_slot(subcarrier_spacing scs, slot_point& out)
{
  std::lock_guard<std::mutex> lock(slot_time_shared::mtx);
  const unsigned              numerology = to_numerology_value(scs);
  if (numerology >= slot_time_shared::max_numerologies) {
    return false;
  }
  const slot_point& slot = slot_time_shared::last_slots[numerology];
  if (!slot.valid()) {
    return false;
  }
  out = slot;
  return true;
}

} // namespace srsran
