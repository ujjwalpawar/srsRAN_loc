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

#include "srsran/ran/rnti.h"
#include "srsran/ran/slot_point.h"
#include "srsran/ran/srs/srs_configuration.h"
#include <optional>
#include <string>

namespace srsran {

/// Captures the metadata required for an exported SRS schedule entry.
struct srs_schedule_descriptor {
  /// Absolute slot where the UE transmits its SRS.
  slot_point slot;
  /// UE identifier (C-RNTI or positioning RNTI).
  rnti_t rnti;
  /// Selected SRS resource parameters.
  srs_config::srs_resource resource;
  /// Indicates whether this opportunity is meant for positioning (multi-RU) purposes.
  bool positioning_requested = false;
  /// IMEISV associated with the UE, if available.
  std::optional<std::string> imeisv;
  /// Unique identifier assigned by the scheduler so external consumers can de-duplicate notifications.
  std::string schedule_id;
};

/// Lightweight hook that receives replicated SRS schedules.
class srs_schedule_exporter
{
public:
  virtual ~srs_schedule_exporter() = default;

  /// Invoked whenever the scheduler successfully places an SRS opportunity in the resource grid.
  virtual void handle_schedule(const srs_schedule_descriptor& descriptor) = 0;
};

} // namespace srsran
