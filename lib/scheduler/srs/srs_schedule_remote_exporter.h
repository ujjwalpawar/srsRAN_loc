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

#include "srsran/scheduler/config/scheduler_expert_config.h"
#include "srsran/scheduler/srs_schedule_exporter.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace srsran {

/// Exporter that forwards positioning schedules to remote control endpoints via WebSocket.
class srs_schedule_remote_exporter : public srs_schedule_exporter
{
public:
  explicit srs_schedule_remote_exporter(const scheduler_positioning_export_config& config);
  ~srs_schedule_remote_exporter() override;

  void handle_schedule(const srs_schedule_descriptor& descriptor) override;
  void handle_stop(const srs_schedule_stop_descriptor& descriptor) override;

private:
  struct endpoint {
    std::string address;
    unsigned    port;
    std::string path;
  };

  void worker_loop();
  bool send_payload_to_endpoint(const endpoint& endpoint, const std::string& payload);

  std::vector<endpoint> endpoints;
  std::mutex            mtx;
  std::condition_variable cv;
  std::deque<std::string> pending;
  std::unordered_set<std::string> active_keys;
  std::thread           worker;
  std::atomic<bool>     stopping{false};
};

} // namespace srsran
