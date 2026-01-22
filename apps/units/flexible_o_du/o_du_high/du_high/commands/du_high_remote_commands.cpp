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

#include "apps/units/flexible_o_du/o_du_high/du_high/commands/du_high_remote_commands.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "srsran/phy/support/dmrs_measurement_queue.h"
#include "srsran/ran/du_types.h"
#include "srsran/ran/dmrs.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/rnti.h"
#include "srsran/ran/sch/sch_dmrs_power.h"
#include "srsran/ran/subcarrier_spacing.h"
#include "srsran/ran/srs/srs_configuration.h"
#include "srsran/scheduler/result/vrb_alloc.h"
#include "srsran/scheduler/ue_identity_tracker.h"
#include "srsran/scheduler/ta_shared.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/math/math_utils.h"
#include <chrono>

using namespace srsran;

namespace {

srslog::basic_logger& positioning_logger = srslog::fetch_basic_logger("POSITIONING", false);
srslog::basic_logger& dmrs_logger = srslog::fetch_basic_logger("DMRS_EXPORT", false);

expected<nr_cell_global_id_t, std::string> parse_nr_cgi_from_json(const nlohmann::json& cell)
{
  auto plmn_key = cell.find("plmn");
  if (plmn_key == cell.end()) {
    return make_unexpected("'plmn' object is missing and it is mandatory");
  }
  if (!plmn_key->is_string()) {
    return make_unexpected("'plmn' object value type should be a string");
  }

  auto nci_key = cell.find("nci");
  if (nci_key == cell.end()) {
    return make_unexpected("'nci' object is missing and it is mandatory");
  }
  if (!nci_key->is_number_unsigned()) {
    return make_unexpected("'nci' object value type should be an integer");
  }

  auto plmn = plmn_identity::parse(plmn_key.value().get_ref<const nlohmann::json::string_t&>());
  if (!plmn) {
    return make_unexpected("Invalid PLMN identity value");
  }
  auto nci = nr_cell_identity::create(nci_key->get<uint64_t>());
  if (!nci) {
    return make_unexpected("Invalid NR cell identity value");
  }
  nr_cell_global_id_t nr_cgi;
  nr_cgi.nci     = nci.value();
  nr_cgi.plmn_id = plmn.value();
  return nr_cgi;
}

} // namespace

error_type<std::string> ssb_modify_remote_command::execute(const nlohmann::json& json)
{
  auto cells_key = json.find("cells");
  if (cells_key == json.end()) {
    return make_unexpected("'cells' object is missing and it is mandatory");
  }
  if (!cells_key->is_array()) {
    return make_unexpected("'cells' object value type should be an array");
  }

  auto cells_items = cells_key->items();
  if (cells_items.begin() == cells_items.end()) {
    return make_unexpected("'cells' object does not contain any cell entries");
  }

  srs_du::du_param_config_request req;
  for (const auto& cell : cells_items) {
    auto ssb_block_power_key = cell.value().find("ssb_block_power_dbm");
    if (ssb_block_power_key == cell.value().end()) {
      return make_unexpected("'ssb_block_power_dbm' object is missing and it is mandatory");
    }
    if (!ssb_block_power_key->is_number_integer()) {
      return make_unexpected("'ssb_block_power_dbm' object value type should be an integer");
    }
    int ssb_block_power_value = ssb_block_power_key->get<int>();
    if (ssb_block_power_value < -60 || ssb_block_power_value > 50) {
      return make_unexpected(
          fmt::format("'ssb_block_power_dbm' value out of range, received '{}', valid range is from -60 to 50",
                      ssb_block_power_value));
    }

    auto nr_cgi = parse_nr_cgi_from_json(cell.value());
    if (!nr_cgi) {
      return make_unexpected(nr_cgi.error());
    }

    req.cells.emplace_back(nr_cgi.value(), ssb_block_power_value);
  }

  if (configurator.handle_operator_config_request(req).success) {
    return {};
  }

  return make_unexpected("SSB modify command procedure failed to be applied by the DU");
}

namespace {

#define RETURN_IF_ERROR(expr)                                                                                               \
  if (auto _err = (expr); !_err)                                                                                            \
    return _err;

template <typename T>
error_type<std::string> fetch_number(const nlohmann::json& parent, const char* field, T& value)
{
  auto it = parent.find(field);
  if (it == parent.end()) {
    return make_unexpected(fmt::format("'{}' object is missing and it is mandatory", field));
  }
  if (!it->is_number_integer()) {
    return make_unexpected(fmt::format("'{}' object value type should be an integer", field));
  }
  value = it->get<T>();
  return {};
}

error_type<std::string> parse_rnti(const nlohmann::json& obj, rnti_t& rnti)
{
  auto it = obj.find("rnti");
  if (it == obj.end()) {
    return make_unexpected("'rnti' object is missing and it is mandatory");
  }
  if (it->is_number_unsigned()) {
    rnti = to_rnti(it->get<uint32_t>());
    return {};
  }
  if (it->is_string()) {
    const auto& str = it->get_ref<const std::string&>();
    try {
      rnti = to_rnti(static_cast<uint32_t>(std::stoul(str, nullptr, 0)));
      return {};
    } catch (const std::exception& e) {
      return make_unexpected(fmt::format("Failed to parse rnti string '{}': {}", str, e.what()));
    }
  }
  return make_unexpected("'rnti' object value type should be a string or integer");
}

error_type<std::string>
resolve_stop_rnti(const nlohmann::json& schedule, const std::optional<std::string>& imeisv, rnti_t& pos_rnti)
{
  if (schedule.find("rnti") != schedule.end()) {
    return parse_rnti(schedule, pos_rnti);
  }
  if (imeisv) {
    pos_rnti = make_positioning_rnti(*imeisv);
    return {};
  }
  return make_unexpected("'rnti' object is missing and 'imeisv' object is not provided");
}

srs_group_or_sequence_hopping
parse_group_or_sequence_hopping(std::string_view value, error_type<std::string>& err)
{
  if (value == "group") {
    return srs_group_or_sequence_hopping::group_hopping;
  }
  if (value == "sequence") {
    return srs_group_or_sequence_hopping::sequence_hopping;
  }
  if (value != "neither") {
    err = make_unexpected(fmt::format("Invalid group_or_sequence_hopping value '{}'", value));
  }
  return srs_group_or_sequence_hopping::neither;
}

srs_resource_type parse_resource_type(std::string_view value, error_type<std::string>& err)
{
  if (value == "aperiodic") {
    return srs_resource_type::aperiodic;
  }
  if (value == "semi_persistent" || value == "semi-persistent") {
    return srs_resource_type::semi_persistent;
  }
  if (value == "periodic") {
    return srs_resource_type::periodic;
  }
  err = make_unexpected(fmt::format("Invalid SRS resource_type '{}'", value));
  return srs_resource_type::aperiodic;
}

error_type<std::string>
parse_srs_resource(const nlohmann::json& resource_json, srs_config::srs_resource& resource)
{
  uint32_t tmp = 0;
  RETURN_IF_ERROR(fetch_number(resource_json, "cell_res_id", tmp));
  resource.id.cell_res_id = tmp;

  RETURN_IF_ERROR(fetch_number(resource_json, "ue_res_id", tmp));
  resource.id.ue_res_id = static_cast<srs_config::srs_res_id>(tmp);

  RETURN_IF_ERROR(fetch_number(resource_json, "nof_ports", tmp));
  resource.nof_ports = static_cast<srs_config::srs_resource::nof_srs_ports>(tmp);

  const auto& mapping = resource_json.find("res_mapping");
  if (mapping == resource_json.end() || !mapping->is_object()) {
    return make_unexpected("'res_mapping' object is missing and it is mandatory");
  }
  RETURN_IF_ERROR(fetch_number(mapping.value(), "start_symbol", tmp));
  resource.res_mapping.start_pos = static_cast<uint8_t>(tmp);
  RETURN_IF_ERROR(fetch_number(mapping.value(), "nof_symbols", tmp));
  resource.res_mapping.nof_symb = static_cast<srs_nof_symbols>(tmp);
  RETURN_IF_ERROR(fetch_number(mapping.value(), "repetition_factor", tmp));
  resource.res_mapping.rept_factor = static_cast<srs_nof_symbols>(tmp);

  RETURN_IF_ERROR(fetch_number(resource_json, "freq_domain_pos", tmp));
  resource.freq_domain_pos = static_cast<uint8_t>(tmp);
  RETURN_IF_ERROR(fetch_number(resource_json, "freq_domain_shift", tmp));
  resource.freq_domain_shift = static_cast<uint16_t>(tmp);

  const auto& freq_hop = resource_json.find("freq_hop");
  if (freq_hop == resource_json.end() || !freq_hop->is_object()) {
    return make_unexpected("'freq_hop' object is missing and it is mandatory");
  }
  RETURN_IF_ERROR(fetch_number(freq_hop.value(), "b_srs", tmp));
  resource.freq_hop.b_srs = static_cast<uint8_t>(tmp);
  RETURN_IF_ERROR(fetch_number(freq_hop.value(), "b_hop", tmp));
  resource.freq_hop.b_hop = static_cast<uint8_t>(tmp);
  RETURN_IF_ERROR(fetch_number(freq_hop.value(), "c_srs", tmp));
  resource.freq_hop.c_srs = static_cast<uint16_t>(tmp);

  const auto& tx_comb = resource_json.find("tx_comb");
  if (tx_comb == resource_json.end() || !tx_comb->is_object()) {
    return make_unexpected("'tx_comb' object is missing and it is mandatory");
  }
  RETURN_IF_ERROR(fetch_number(tx_comb.value(), "size", tmp));
  resource.tx_comb.size = static_cast<tx_comb_size>(tmp);
  RETURN_IF_ERROR(fetch_number(tx_comb.value(), "offset", tmp));
  resource.tx_comb.tx_comb_offset = static_cast<uint8_t>(tmp);
  RETURN_IF_ERROR(fetch_number(tx_comb.value(), "cyclic_shift", tmp));
  resource.tx_comb.tx_comb_cyclic_shift = static_cast<uint8_t>(tmp);

  RETURN_IF_ERROR(fetch_number(resource_json, "sequence_id", tmp));
  resource.sequence_id = static_cast<uint16_t>(tmp);

  auto hop_it = resource_json.find("group_or_sequence_hopping");
  if (hop_it == resource_json.end() || !hop_it->is_string()) {
    return make_unexpected("'group_or_sequence_hopping' object is missing and it is mandatory");
  }
  error_type<std::string> err = {};
  resource.grp_or_seq_hop = parse_group_or_sequence_hopping(hop_it->get_ref<const std::string&>(), err);
  if (!err) {
    return err;
  }

  auto res_type_it = resource_json.find("resource_type");
  if (res_type_it == resource_json.end() || !res_type_it->is_string()) {
    return make_unexpected("'resource_type' object is missing and it is mandatory");
  }
  err               = {};
  resource.res_type = parse_resource_type(res_type_it->get_ref<const std::string&>(), err);
  if (!err) {
    return err;
  }

  const auto& periodicity = resource_json.find("periodicity");
  if (periodicity == resource_json.end() || !periodicity->is_object()) {
    return make_unexpected("'periodicity' object is missing and it is mandatory for positioning");
  }
  RETURN_IF_ERROR(fetch_number(periodicity.value(), "t_srs", tmp));
  if (tmp == 0) {
    return make_unexpected("'periodicity.t_srs' must be greater than 0 for positioning");
  }
  srs_periodicity period = static_cast<srs_periodicity>(tmp);
  RETURN_IF_ERROR(fetch_number(periodicity.value(), "offset", tmp));
  resource.periodicity_and_offset =
      srs_config::srs_periodicity_and_offset{period, static_cast<uint16_t>(tmp)};

  return {};
}

#undef RETURN_IF_ERROR

} // namespace

error_type<std::string> positioning_trigger_remote_command::execute(const nlohmann::json& json)
{
  auto cells_key = json.find("cells");
  if (cells_key == json.end()) {
    return make_unexpected("'cells' object is missing and it is mandatory");
  }
  if (!cells_key->is_array()) {
    return make_unexpected("'cells' object value type should be an array");
  }

  srs_du::du_param_config_request req;
  for (const auto& cell : cells_key->items()) {
    auto nr_cgi = parse_nr_cgi_from_json(cell.value());
    if (!nr_cgi) {
      return make_unexpected(nr_cgi.error());
    }
    nr_cell_global_id_t cell_id = nr_cgi.value();

    auto schedule_key = cell.value().find("schedule");
    if (schedule_key == cell.value().end() || !schedule_key->is_object()) {
      return make_unexpected("'schedule' object is missing and it is mandatory");
    }

    mac_cell_positioning_measurement_request pos_req;
    if (auto imeisv = schedule_key->find("imeisv"); imeisv != schedule_key->end() && imeisv->is_string()) {
      pos_req.imeisv = imeisv->get<std::string>();
    }

    if (auto ue_index_key = schedule_key->find("ue_index");
        ue_index_key != schedule_key->end() && ue_index_key->is_number_unsigned()) {
      pos_req.ue_index = to_du_ue_index(ue_index_key->get<uint32_t>());
    }

    if (auto rnti_field = schedule_key->find("rnti"); rnti_field != schedule_key->end()) {
      pos_req.rnti.emplace();
      if (auto err = parse_rnti(schedule_key.value(), *pos_req.rnti); !err) {
        return err;
      }
    }
    std::optional<unsigned> schedule_sfn;
    std::optional<unsigned> schedule_slot;
    if (auto sfn_field = schedule_key->find("sfn");
        sfn_field != schedule_key->end() && sfn_field->is_number_unsigned()) {
      schedule_sfn = sfn_field->get<unsigned>();
    }
    if (auto slot_field = schedule_key->find("slot");
        slot_field != schedule_key->end() && slot_field->is_number_unsigned()) {
      schedule_slot = slot_field->get<unsigned>();
    }
    if (auto sched_id_field = schedule_key->find("schedule_id");
        sched_id_field != schedule_key->end() && sched_id_field->is_string()) {
      positioning_logger.info("Positioning request schedule_id={}", sched_id_field->get_ref<const std::string&>());
    }
    std::string rnti_str =
        pos_req.rnti ? fmt::format("{:#x}", to_value(*pos_req.rnti)) : std::string("derived_from_imeisv");

    // Optional RAR TA: if present and RNTI known, store it so TA streamer can use it later.
    if (auto rar_ta_field = schedule_key->find("rar_ta");
        rar_ta_field != schedule_key->end() && rar_ta_field->is_number_integer() && pos_req.rnti) {
      const int rar_ta_value = rar_ta_field->get<int>();
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      set_last_ta_with_crnti(now_ns, rar_ta_value, true, to_value(*pos_req.rnti));
      positioning_logger.info("Stashed RAR TA={} for rnti={} (cell={}/{})",
                              rar_ta_value,
                              rnti_str,
                              cell_id.plmn_id,
                              cell_id.nci);
    }
    // If IMEISV and RNTI are both present, store mapping so IQ streaming can emit IMEISV even for neighbour UE.
    if (pos_req.imeisv && pos_req.rnti) {
      ue_identity_tracker::set_imeisv_for_crnti(to_value(*pos_req.rnti), *pos_req.imeisv);
    }

    // Parse UE-specific SRS resources.
    std::vector<srs_config::srs_resource> resources_to_add;
    auto all_res_key = schedule_key->find("all_resources");
    if (all_res_key != schedule_key->end() && all_res_key->is_array()) {
      resources_to_add.reserve(all_res_key->size());
      for (const auto& res_entry : all_res_key.value()) {
        if (!res_entry.is_object()) {
          return make_unexpected("Each entry in 'all_resources' must be an object");
        }
        srs_config::srs_resource res{};
        if (auto err = parse_srs_resource(res_entry, res); !err) {
          return err;
        }
        resources_to_add.push_back(res);
      }
    }

    // Fallback: accept a single 'resource' object when all_resources is absent, but log it.
    if (resources_to_add.empty()) {
      auto res_entry = schedule_key->find("resource");
      if (res_entry != schedule_key->end() && res_entry->is_object()) {
        srs_config::srs_resource res{};
        if (auto err = parse_srs_resource(res_entry.value(), res); !err) {
          return err;
        }
        resources_to_add.push_back(res);
        positioning_logger.warning("Positioning request for cell={} imeisv={} rnti={} missing all_resources; using "
                                   "single 'resource' entry instead",
                                   fmt::format("{}/{}", cell_id.plmn_id, cell_id.nci),
                                   pos_req.imeisv.value_or("unknown"),
                                   rnti_str);
      }
    }

    if (resources_to_add.empty()) {
      return make_unexpected("'all_resources' array is missing or empty and no fallback 'resource' provided");
    }

    // Add all resources and a single resource set containing their IDs.
    for (const auto& res : resources_to_add) {
      pos_req.srs_to_meas.srs_res_list.push_back(res);
    }

    srs_config::srs_resource_set set;
    set.id = srs_config::srs_res_set_id::MIN_SRS_RES_SET_ID;
    for (const auto& res : resources_to_add) {
      set.srs_res_id_list.push_back(res.id.ue_res_id);
    }
    set.srs_res_set_usage = srs_usage::codebook;
    // Use the first resource type to set the set type (assumes homogeneous types).
    if (!resources_to_add.empty()) {
      auto res_type = resources_to_add.front().res_type;
      if (res_type == srs_resource_type::periodic) {
        set.res_type.emplace<srs_config::srs_resource_set::periodic_resource_type>();
      } else if (res_type == srs_resource_type::semi_persistent) {
        set.res_type.emplace<srs_config::srs_resource_set::semi_persistent_resource_type>();
      } else {
        set.res_type.emplace<srs_config::srs_resource_set::aperiodic_resource_type>();
      }
    }
    pos_req.srs_to_meas.srs_res_set_list.push_back(set);

    std::string cell_str = fmt::format("{}/{}", cell_id.plmn_id, cell_id.nci);
    // Use the first resource to log the UE-specific configuration summary.
    std::string periodicity_str = "n/a";
    if (!resources_to_add.empty() && resources_to_add.front().periodicity_and_offset) {
      periodicity_str = fmt::format("{}@{}",
                                    static_cast<unsigned>(resources_to_add.front().periodicity_and_offset->period),
                                    resources_to_add.front().periodicity_and_offset->offset);
    }

    const std::string sfn_str  = schedule_sfn ? fmt::format("{}", *schedule_sfn) : "n/a";
    const std::string slot_str = schedule_slot ? fmt::format("{}", *schedule_slot) : "n/a";
    positioning_logger.info(
        "Positioning request received: cell={} imeisv={} rnti={} cell_res={} ue_res={} periodicity={} sfn={} slot={}",
        cell_str,
        pos_req.imeisv.value_or("unknown"),
        rnti_str,
        resources_to_add.empty() ? -1 : resources_to_add.front().id.cell_res_id,
        resources_to_add.empty() ? 0u : static_cast<unsigned>(resources_to_add.front().id.ue_res_id),
        periodicity_str,
        sfn_str,
        slot_str);

    srs_du::du_cell_param_config_request cell_req;
    cell_req.nr_cgi      = cell_id;
    cell_req.positioning = std::move(pos_req);
    req.cells.push_back(std::move(cell_req));
  }

  if (req.cells.empty()) {
    return make_unexpected("No positioning requests provided");
  }

  const auto& result = configurator.handle_operator_config_request(req);
  if (result.success) {
    return {};
  }
  return make_unexpected("Positioning request failed to be applied by the DU");
}

error_type<std::string> positioning_stop_remote_command::execute(const nlohmann::json& json)
{
  auto cells_key = json.find("cells");
  if (cells_key == json.end()) {
    return make_unexpected("'cells' object is missing and it is mandatory");
  }
  if (!cells_key->is_array()) {
    return make_unexpected("'cells' object value type should be an array");
  }

  srs_du::du_param_config_request req;
  for (const auto& cell : cells_key->items()) {
    auto nr_cgi = parse_nr_cgi_from_json(cell.value());
    if (!nr_cgi) {
      return make_unexpected(nr_cgi.error());
    }
    nr_cell_global_id_t cell_id = nr_cgi.value();

    auto schedule_key = cell.value().find("schedule");
    if (schedule_key == cell.value().end() || !schedule_key->is_object()) {
      return make_unexpected("'schedule' object is missing and it is mandatory");
    }

    std::optional<std::string> imeisv;
    if (auto imeisv_key = schedule_key->find("imeisv"); imeisv_key != schedule_key->end() && imeisv_key->is_string()) {
      imeisv = imeisv_key->get<std::string>();
    }

    rnti_t pos_rnti = rnti_t::INVALID_RNTI;
    if (auto err = resolve_stop_rnti(schedule_key.value(), imeisv, pos_rnti); !err) {
      return err;
    }

    mac_cell_positioning_measurement_stop_request stop_req;
    stop_req.pos_rnti = pos_rnti;

    srs_du::du_cell_param_config_request cell_req;
    cell_req.nr_cgi          = cell_id;
    cell_req.positioning_stop = std::move(stop_req);
    req.cells.push_back(std::move(cell_req));
  }

  if (req.cells.empty()) {
    return make_unexpected("No positioning stops provided");
  }

  const auto& result = configurator.handle_operator_config_request(req);
  if (result.success) {
    return {};
  }
  return make_unexpected("Positioning stop failed to be applied by the DU");
}

#define RETURN_IF_ERROR(expr)                                                                                               \
  if (auto _err = (expr); !_err) {                                                                                          \
    return _err;                                                                                                            \
  }

error_type<std::string> dmrs_schedule_remote_command::execute(const nlohmann::json& json)
{
  auto cells_key = json.find("cells");
  if (cells_key == json.end()) {
    return make_unexpected("'cells' object is missing and it is mandatory");
  }
  if (!cells_key->is_array()) {
    return make_unexpected("'cells' object value type should be an array");
  }

  auto cells_items = cells_key->items();
  if (cells_items.begin() == cells_items.end()) {
    return make_unexpected("'cells' object does not contain any cell entries");
  }

  auto fetch_bool = [](const nlohmann::json& parent, const char* field, bool& value) -> error_type<std::string> {
    auto it = parent.find(field);
    if (it == parent.end()) {
      return make_unexpected(fmt::format("'{}' object is missing and it is mandatory", field));
    }
    if (!it->is_boolean()) {
      return make_unexpected(fmt::format("'{}' object value type should be a boolean", field));
    }
    value = it->get<bool>();
    return {};
  };

  auto parse_dmrs_type = [](const std::string& value) -> expected<dmrs_type, std::string> {
    if (value == "type1") {
      return dmrs_type::TYPE1;
    }
    if (value == "type2") {
      return dmrs_type::TYPE2;
    }
    return make_unexpected(fmt::format("Unsupported dmrs.config_type '{}'", value));
  };

  auto parse_nominal_rbg_size = [](unsigned value) -> expected<nominal_rbg_size, std::string> {
    switch (value) {
      case 2:
        return nominal_rbg_size::P2;
      case 4:
        return nominal_rbg_size::P4;
      case 8:
        return nominal_rbg_size::P8;
      case 16:
        return nominal_rbg_size::P16;
      default:
        return make_unexpected(fmt::format("Unsupported rbg_size '{}'", value));
    }
  };

  for (const auto& cell : cells_items) {
    auto nr_cgi = parse_nr_cgi_from_json(cell.value());
    if (!nr_cgi) {
      return make_unexpected(nr_cgi.error());
    }

    auto schedule_key = cell.value().find("schedule");
    if (schedule_key == cell.value().end() || !schedule_key->is_object()) {
      return make_unexpected("'schedule' object is missing and it is mandatory");
    }

    rnti_t rnti = rnti_t::INVALID_RNTI;
    if (auto err = parse_rnti(schedule_key.value(), rnti); !err) {
      return err;
    }

    std::string imeisv = "unknown";
    if (auto imeisv_key = schedule_key->find("imeisv"); imeisv_key != schedule_key->end() && imeisv_key->is_string()) {
      imeisv = imeisv_key->get<std::string>();
    }

    if (imeisv != "unknown") {
      ue_identity_tracker::set_imeisv_for_crnti(to_value(rnti), imeisv);
    }

    unsigned sfn  = 0;
    unsigned slot = 0;
    RETURN_IF_ERROR(fetch_number(schedule_key.value(), "sfn", sfn));
    RETURN_IF_ERROR(fetch_number(schedule_key.value(), "slot", slot));

    if (auto rar_ta_field = schedule_key->find("rar_ta");
        rar_ta_field != schedule_key->end() && rar_ta_field->is_number_integer()) {
      const int rar_ta_value = rar_ta_field->get<int>();
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      set_last_ta_with_crnti(now_ns, rar_ta_value, true, to_value(rnti));
      dmrs_logger.info("DMRS schedule stashed RAR TA={} for rnti={}", rar_ta_value, fmt::format("{:#x}", to_value(rnti)));
    }

    auto bwp_key = schedule_key->find("bwp");
    if (bwp_key == schedule_key->end() || !bwp_key->is_object()) {
      return make_unexpected("'bwp' object is missing and it is mandatory");
    }
    unsigned bwp_crb_start = 0;
    unsigned bwp_nof_prbs  = 0;
    unsigned bwp_scs_khz   = 0;
    RETURN_IF_ERROR(fetch_number(bwp_key.value(), "crb_start", bwp_crb_start));
    RETURN_IF_ERROR(fetch_number(bwp_key.value(), "nof_prbs", bwp_nof_prbs));
    RETURN_IF_ERROR(fetch_number(bwp_key.value(), "scs_khz", bwp_scs_khz));
    if (bwp_crb_start + bwp_nof_prbs > MAX_RB) {
      return make_unexpected(
          fmt::format("BWP exceeds max RBs (crb_start={} nof_prbs={} max={})", bwp_crb_start, bwp_nof_prbs, MAX_RB));
    }

    subcarrier_spacing scs = to_subcarrier_spacing(std::to_string(bwp_scs_khz));
    if (scs == subcarrier_spacing::invalid) {
      return make_unexpected(fmt::format("Unsupported scs_khz '{}'", bwp_scs_khz));
    }

    auto symbols_key = schedule_key->find("symbols");
    if (symbols_key == schedule_key->end() || !symbols_key->is_object()) {
      return make_unexpected("'symbols' object is missing and it is mandatory");
    }
    unsigned start_symbol = 0;
    unsigned nof_symbols  = 0;
    RETURN_IF_ERROR(fetch_number(symbols_key.value(), "start", start_symbol));
    RETURN_IF_ERROR(fetch_number(symbols_key.value(), "length", nof_symbols));

    auto dmrs_key = schedule_key->find("dmrs");
    if (dmrs_key == schedule_key->end() || !dmrs_key->is_object()) {
      return make_unexpected("'dmrs' object is missing and it is mandatory");
    }

    uint64_t symbol_mask = 0;
    uint64_t ports_mask  = 0;
    RETURN_IF_ERROR(fetch_number(dmrs_key.value(), "symbol_mask", symbol_mask));
    RETURN_IF_ERROR(fetch_number(dmrs_key.value(), "ports_mask", ports_mask));

    auto config_type_key = dmrs_key->find("config_type");
    if (config_type_key == dmrs_key->end() || !config_type_key->is_string()) {
      return make_unexpected("'dmrs.config_type' object is missing and it is mandatory");
    }
    auto dmrs_type_value = parse_dmrs_type(config_type_key->get<std::string>());
    if (!dmrs_type_value) {
      return make_unexpected(dmrs_type_value.error());
    }

    unsigned scrambling_id              = 0;
    unsigned nof_cdm_groups_no_data     = 0;
    bool     n_scid                     = false;
    bool     transform_precoding        = false;
    unsigned dmrs_id                    = 0;

    RETURN_IF_ERROR(fetch_number(dmrs_key.value(), "scrambling_id", scrambling_id));
    RETURN_IF_ERROR(fetch_number(dmrs_key.value(), "num_cdm_groups_no_data", nof_cdm_groups_no_data));
    RETURN_IF_ERROR(fetch_bool(dmrs_key.value(), "n_scid", n_scid));
    RETURN_IF_ERROR(fetch_bool(dmrs_key.value(), "transform_precoding", transform_precoding));
    if (transform_precoding) {
      RETURN_IF_ERROR(fetch_number(dmrs_key.value(), "dmrs_id", dmrs_id));
    }

    auto rb_alloc_key = schedule_key->find("rb_allocation");
    if (rb_alloc_key == schedule_key->end() || !rb_alloc_key->is_object()) {
      return make_unexpected("'rb_allocation' object is missing and it is mandatory");
    }

    bounded_bitset<MAX_RB> rb_mask;
    rb_mask.resize(bwp_crb_start + bwp_nof_prbs);

    auto rb_alloc_type_key = rb_alloc_key->find("type");
    if (rb_alloc_type_key == rb_alloc_key->end() || !rb_alloc_type_key->is_string()) {
      return make_unexpected("'rb_allocation.type' object is missing and it is mandatory");
    }
    const std::string rb_alloc_type = rb_alloc_type_key->get<std::string>();
    if (rb_alloc_type == "type1") {
      unsigned rb_start = 0;
      unsigned rb_length = 0;
      RETURN_IF_ERROR(fetch_number(rb_alloc_key.value(), "start", rb_start));
      RETURN_IF_ERROR(fetch_number(rb_alloc_key.value(), "length", rb_length));
      if (rb_start + rb_length > bwp_nof_prbs) {
        return make_unexpected(
            fmt::format("Type1 RB allocation exceeds BWP (start={} length={} bwp_prbs={})",
                        rb_start,
                        rb_length,
                        bwp_nof_prbs));
      }
      rb_mask.fill(bwp_crb_start + rb_start, bwp_crb_start + rb_start + rb_length);
    } else if (rb_alloc_type == "type0") {
      auto rbg_mask_key = rb_alloc_key->find("rbg_mask");
      if (rbg_mask_key == rb_alloc_key->end() || !rbg_mask_key->is_string()) {
        return make_unexpected("'rb_allocation.rbg_mask' object is missing and it is mandatory");
      }

      unsigned rbg_size = 0;
      RETURN_IF_ERROR(fetch_number(rb_alloc_key.value(), "rbg_size", rbg_size));
      auto rbg_size_value = parse_nominal_rbg_size(rbg_size);
      if (!rbg_size_value) {
        return make_unexpected(rbg_size_value.error());
      }

      unsigned rbg_count = 0;
      RETURN_IF_ERROR(fetch_number(rb_alloc_key.value(), "rbg_count", rbg_count));

      rbg_bitmap rbgs(rbg_count);
      try {
        rbgs.from_uint64(std::stoull(rbg_mask_key->get<std::string>(), nullptr, 0));
      } catch (const std::exception& e) {
        return make_unexpected(fmt::format("Failed to parse rbg_mask '{}': {}", rbg_mask_key->get<std::string>(), e.what()));
      }

      crb_interval bwp_rbs(bwp_crb_start, bwp_crb_start + bwp_nof_prbs);
      prb_bitmap prbs = convert_rbgs_to_prbs(rbgs, bwp_rbs, rbg_size_value.value());
      for (size_t prb = 0; prb != prbs.size(); ++prb) {
        if (prbs.test(prb)) {
          rb_mask.set(bwp_crb_start + prb);
        }
      }
    } else {
      return make_unexpected(fmt::format("Unsupported rb_allocation.type '{}'", rb_alloc_type));
    }

    dmrs_pusch_estimator::configuration cfg;
    cfg.slot = slot_point(scs, sfn, slot);
    cfg.rnti = rnti;
    cfg.enable_udp = true;
    if (transform_precoding) {
      cfg.sequence_config = dmrs_pusch_estimator::low_papr_sequence_configuration{.n_rs_id = dmrs_id};
    } else {
      unsigned nof_tx_layers = static_cast<unsigned>(count_ones(ports_mask));
      if (nof_tx_layers == 0) {
        nof_tx_layers = 1;
      }
      cfg.sequence_config = dmrs_pusch_estimator::pseudo_random_sequence_configuration{
          .type          = dmrs_type_value.value(),
          .nof_tx_layers = nof_tx_layers,
          .scrambling_id = scrambling_id,
          .n_scid        = n_scid};
    }
    cfg.scaling      = convert_dB_to_amplitude(-get_sch_to_dmrs_ratio_dB(nof_cdm_groups_no_data));
    cfg.c_prefix     = cyclic_prefix::NORMAL;
    cfg.symbols_mask.resize(get_nsymb_per_slot(cfg.c_prefix));
    cfg.symbols_mask.from_uint64(symbol_mask);
    cfg.rb_mask      = rb_mask;
    cfg.first_symbol = start_symbol;
    cfg.nof_symbols  = nof_symbols;

    get_dmrs_measurement_queue().push(dmrs_measurement_request{.config = std::move(cfg)});

    dmrs_logger.info("DMRS schedule received: cell={}/{} imeisv={} rnti={} sfn={} slot={}",
                     nr_cgi->plmn_id.to_string(),
                     nr_cgi->nci.value(),
                     imeisv,
                     fmt::format("{:#x}", to_value(rnti)),
                     sfn,
                     slot);
  }

  return {};
}

#undef RETURN_IF_ERROR
