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

#include "srsran/scheduler/srs_schedule_exporter.h"
#include <mutex>
#include <string>

namespace srsran {

/// Simple exporter that serializes positioning schedules into a JSON file.
class srs_schedule_file_exporter : public srs_schedule_exporter
{
public:
  explicit srs_schedule_file_exporter(std::string output_path);

  void handle_schedule(const srs_schedule_descriptor& descriptor) override;

private:
  std::string       path;
  std::mutex        mtx;
};

} // namespace srsran
