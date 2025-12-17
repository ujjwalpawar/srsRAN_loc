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

#include "srs_schedule_file_exporter.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>

using namespace srsran;

namespace {

std::string to_string_group_hopping(srs_group_or_sequence_hopping value)
{
  switch (value) {
    case srs_group_or_sequence_hopping::group_hopping:
      return "group";
    case srs_group_or_sequence_hopping::sequence_hopping:
      return "sequence";
    default:
      return "neither";
  }
}

nlohmann::json build_resource_json(const srs_config::srs_resource& res)
{
  nlohmann::json j;
  j["cell_res_id"] = res.id.cell_res_id;
  j["ue_res_id"]   = fmt::underlying(res.id.ue_res_id);
  j["nof_ports"]   = fmt::underlying(res.nof_ports);

  nlohmann::json mapping;
  mapping["start_symbol"]      = res.res_mapping.start_pos;
  mapping["nof_symbols"]       = static_cast<unsigned>(res.res_mapping.nof_symb);
  mapping["repetition_factor"] = static_cast<unsigned>(res.res_mapping.rept_factor);
  j["res_mapping"]             = std::move(mapping);

  j["freq_domain_pos"]   = res.freq_domain_pos;
  j["freq_domain_shift"] = res.freq_domain_shift;

  nlohmann::json freq_hop;
  freq_hop["b_srs"] = res.freq_hop.b_srs;
  freq_hop["b_hop"] = res.freq_hop.b_hop;
  freq_hop["c_srs"] = res.freq_hop.c_srs;
  j["freq_hop"]     = std::move(freq_hop);

  nlohmann::json tx_comb;
  tx_comb["size"]         = fmt::underlying(res.tx_comb.size);
  tx_comb["offset"]       = res.tx_comb.tx_comb_offset;
  tx_comb["cyclic_shift"] = res.tx_comb.tx_comb_cyclic_shift;
  j["tx_comb"]            = std::move(tx_comb);

  j["sequence_id"]               = res.sequence_id;
  j["group_or_sequence_hopping"] = to_string_group_hopping(res.grp_or_seq_hop);
  j["resource_type"]             = std::string(to_string(res.res_type));

  nlohmann::json periodicity;
  if (res.periodicity_and_offset) {
    periodicity["t_srs"] = static_cast<unsigned>(res.periodicity_and_offset->period);
    periodicity["offset"] = res.periodicity_and_offset->offset;
  } else {
    periodicity["t_srs"] = 0;
    periodicity["offset"] = 0;
  }
  j["periodicity"] = std::move(periodicity);

  return j;
}

std::string build_resource_key(const srs_config::srs_resource& res,
                               const nr_cell_global_id_t&      cell_id,
                               rnti_t                          rnti,
                               bool                            positioning_requested,
                               const std::optional<std::string>& imeisv)
{
  return fmt::format("{}-{}-{}-{}-{}-{}-{}-{}-{}-{}-{}-{}-{}-{}-{}-{}-{}",
                     cell_id.plmn_id.to_string(),
                     cell_id.nci.value(),
                     fmt::underlying(rnti),
                     positioning_requested ? 1 : 0,
                     imeisv.value_or(""),
                     res.id.cell_res_id,
                     fmt::underlying(res.id.ue_res_id),
                     fmt::underlying(res.nof_ports),
                     res.res_mapping.start_pos,
                     static_cast<unsigned>(res.res_mapping.nof_symb),
                     static_cast<unsigned>(res.res_mapping.rept_factor),
                     res.freq_domain_pos,
                     res.freq_domain_shift,
                     res.freq_hop.b_srs,
                     res.freq_hop.b_hop,
                     res.freq_hop.c_srs,
                     fmt::underlying(res.tx_comb.size),
                     res.tx_comb.tx_comb_offset);
}

} // namespace

srs_schedule_file_exporter::srs_schedule_file_exporter(std::string output_path) : path(std::move(output_path))
{
}

srs_schedule_file_exporter::~srs_schedule_file_exporter()
{
  std::lock_guard<std::mutex> lock(mtx);
  std::error_code             ec;
  std::filesystem::remove(path, ec);
}

void srs_schedule_file_exporter::handle_schedule(const srs_schedule_descriptor& descriptor)
{
  const std::string key =
      build_resource_key(descriptor.resource, descriptor.cell_id, descriptor.rnti, descriptor.positioning_requested, descriptor.imeisv);

  nlohmann::json payload;
  payload["cmd"] = "positioning_request";

  nlohmann::json cell;
  cell["plmn"] = descriptor.cell_id.plmn_id.to_string();
  cell["nci"]  = descriptor.cell_id.nci.value();

  nlohmann::json schedule;
  if (descriptor.imeisv) {
    schedule["imeisv"] = *descriptor.imeisv;
  }
  schedule["rnti"] = fmt::format("{:#x}", to_value(descriptor.rnti));
  schedule["slot"] = {{"sfn", descriptor.slot.sfn()}, {"slot", descriptor.slot.slot_index()}};
  schedule["schedule_id"] = descriptor.schedule_id;
  schedule["resource"]    = build_resource_json(descriptor.resource);

  cell["schedule"] = std::move(schedule);
  payload["cells"] = nlohmann::json::array({cell});

  std::lock_guard<std::mutex> lock(mtx);
  // Only emit once per resource until a stop is received.
  if (!active_keys.insert(key).second) {
    return;
  }

  std::ofstream ofs(path, std::ios::trunc);
  if (!ofs.is_open()) {
    return;
  }
  ofs << payload.dump(2);
  ofs.flush();
}

void srs_schedule_file_exporter::handle_stop(const srs_schedule_stop_descriptor& descriptor)
{
  const std::string key =
      build_resource_key(descriptor.resource, descriptor.cell_id, descriptor.rnti, descriptor.positioning_requested, descriptor.imeisv);

  nlohmann::json payload;
  payload["cmd"] = "positioning_stop";

  nlohmann::json cell;
  cell["plmn"] = descriptor.cell_id.plmn_id.to_string();
  cell["nci"]  = descriptor.cell_id.nci.value();

  nlohmann::json schedule;
  schedule["rnti"]     = fmt::format("{:#x}", to_value(descriptor.rnti));
  schedule["resource"] = build_resource_json(descriptor.resource);
  schedule["action"]   = "stop";
  if (descriptor.imeisv) {
    schedule["imeisv"] = *descriptor.imeisv;
  }
  if (descriptor.positioning_requested) {
    schedule["positioning_requested"] = true;
  }

  cell["schedule"] = std::move(schedule);
  payload["cells"] = nlohmann::json::array({cell});

  std::lock_guard<std::mutex> lock(mtx);
  active_keys.erase(key);

  std::ofstream               ofs(path, std::ios::trunc);
  if (!ofs.is_open()) {
    return;
  }
  ofs << payload.dump(2);
  ofs.flush();
}
