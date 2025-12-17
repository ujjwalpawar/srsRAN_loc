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
#include "srsran/ran/du_types.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/srs/srs_configuration.h"
#include "srsran/srslog/srslog.h"

using namespace srsran;

namespace {

srslog::basic_logger& positioning_logger = srslog::fetch_basic_logger("POSITIONING", false);

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
  if (value == "semi_persistent") {
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
    return make_unexpected("'periodicity' object is missing and it is mandatory");
  }
  RETURN_IF_ERROR(fetch_number(periodicity.value(), "t_srs", tmp));
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
    std::string rnti_str =
        pos_req.rnti ? fmt::format("{:#x}", to_value(*pos_req.rnti)) : std::string("derived_from_imeisv");

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

    positioning_logger.info(
        "Positioning request received: cell={} imeisv={} rnti={} cell_res={} ue_res={} periodicity={}",
        cell_str,
        pos_req.imeisv.value_or("unknown"),
        rnti_str,
        resources_to_add.empty() ? -1 : resources_to_add.front().id.cell_res_id,
        resources_to_add.empty() ? 0u : static_cast<unsigned>(resources_to_add.front().id.ue_res_id),
        periodicity_str);

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
