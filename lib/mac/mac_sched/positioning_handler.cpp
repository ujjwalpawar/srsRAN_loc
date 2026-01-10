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

#ifndef SRSRAN_HAS_ENTERPRISE

#include "positioning_handler.h"
#include "srsran/mac/mac_cell_control_information_handler.h"
#include "srsran/mac/mac_cell_manager.h"
#include "srsran/scheduler/scheduler_positioning_handler.h"
#include "srsran/support/async/async_no_op_task.h"
#include "srsran/support/executors/task_executor.h"

using namespace srsran;

namespace {

class simple_positioning_handler final : public positioning_handler
{
public:
  simple_positioning_handler(scheduler_positioning_handler& sched_, du_cell_index_t cell_index_) :
    sched(sched_), cell_index(cell_index_)
  {
  }

  async_task<mac_cell_positioning_measurement_response>
  handle_positioning_measurement_request(const mac_cell_positioning_measurement_request& req) override
  {
    positioning_measurement_request sched_req;
    sched_req.pos_rnti   = req.rnti.value_or(rnti_t::INVALID_RNTI);
    sched_req.ue_index   = req.ue_index;
    sched_req.imeisv     = req.imeisv;
    sched_req.cell_index = cell_index;
    sched_req.srs_to_measure = req.srs_to_meas;

    sched.handle_positioning_measurement_request(sched_req);

    // This implementation only forwards the request; results are handled elsewhere.
    return launch_no_op_task(mac_cell_positioning_measurement_response{});
  }

  async_task<void>
  handle_positioning_measurement_stop(const mac_cell_positioning_measurement_stop_request& req) override
  {
    sched.handle_positioning_measurement_stop(cell_index, req.pos_rnti);
    return launch_no_op_task();
  }

  void handle_srs_indication(const mac_srs_indication_message& msg) override
  {
    // No-op: upstream handling not required for exporting schedules.
    (void)msg;
  }

private:
  scheduler_positioning_handler& sched;
  du_cell_index_t                cell_index;
};

} // namespace

std::unique_ptr<positioning_handler> srsran::create_positioning_handler(scheduler_positioning_handler& sched,
                                                                        du_cell_index_t                cell_index,
                                                                        task_executor&                 ctrl_exec,
                                                                        srslog::basic_logger&          logger)
{
  (void)ctrl_exec;
  (void)logger;
  return std::make_unique<simple_positioning_handler>(sched, cell_index);
}

#endif
