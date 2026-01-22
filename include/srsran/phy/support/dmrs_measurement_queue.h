/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "srsran/phy/upper/signal_processors/dmrs_pusch_estimator.h"
#include <mutex>
#include <vector>

namespace srsran {

struct dmrs_measurement_request {
  dmrs_pusch_estimator::configuration config;
};

class dmrs_measurement_queue
{
public:
  void push(dmrs_measurement_request request)
  {
    std::lock_guard<std::mutex> lock(mtx);
    pending.push_back(std::move(request));
  }

  std::vector<dmrs_measurement_request> pop_slot(slot_point slot)
  {
    std::vector<dmrs_measurement_request> out;
    std::lock_guard<std::mutex>           lock(mtx);
    auto                                  it = pending.begin();
    while (it != pending.end()) {
      if (it->config.slot == slot) {
        out.push_back(std::move(*it));
        it = pending.erase(it);
      } else {
        ++it;
      }
    }
    return out;
  }

private:
  std::mutex                        mtx;
  std::vector<dmrs_measurement_request> pending;
};

inline dmrs_measurement_queue& get_dmrs_measurement_queue()
{
  static dmrs_measurement_queue queue;
  return queue;
}

} // namespace srsran
