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

#include "srsran/adt/span.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/slot_point.h"
#include "srsran/scheduler/config/scheduler_expert_config.h"
#include "srsran/scheduler/result/pusch_info.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace srsran {

class dmrs_schedule_remote_exporter
{
public:
  explicit dmrs_schedule_remote_exporter(const scheduler_positioning_export_config& config);
  ~dmrs_schedule_remote_exporter();

  void handle_slot(slot_point slot, const nr_cell_global_id_t& cell_id, span<const ul_sched_info> puschs);

private:
  struct endpoint {
    std::string address;
    unsigned    port = 0;
    std::string path = "/";
  };

  void worker_loop();
  bool send_payload_to_endpoint(const endpoint& peer, const std::string& payload);

  std::vector<endpoint> endpoints;
  std::deque<std::string> pending;
  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<bool> stopping{false};
  std::thread worker;
};

} // namespace srsran
