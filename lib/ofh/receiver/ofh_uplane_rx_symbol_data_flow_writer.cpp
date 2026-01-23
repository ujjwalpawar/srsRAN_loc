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

#include "ofh_uplane_rx_symbol_data_flow_writer.h"
#include "srsran/instrumentation/traces/ofh_traces.h"
#include "srsran/ofh/serdes/ofh_uplane_message_decoder_properties.h"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace srsran;
using namespace ofh;

namespace {

struct ofh_ul_dump_config {
  bool        enabled;
  unsigned    subframe;
  unsigned    slot;
  unsigned    symbol;
  unsigned    port;
  unsigned    sfn_mod;
  std::string path_prefix;
};

unsigned parse_env_or_default(const char* name, unsigned default_value)
{
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return default_value;
  }

  char*         end_ptr = nullptr;
  unsigned long parsed  = std::strtoul(value, &end_ptr, 10);
  if (end_ptr == value) {
    return default_value;
  }

  return static_cast<unsigned>(parsed);
}

const ofh_ul_dump_config& get_ul_dump_config()
{
  static const ofh_ul_dump_config cfg = []() {
    ofh_ul_dump_config out{};
    const char*        enable_env = std::getenv("SRSRAN_OFH_DUMP_ENABLE");
    out.enabled                  = (enable_env != nullptr) && (std::atoi(enable_env) != 0);
    out.subframe                 = parse_env_or_default("SRSRAN_OFH_DUMP_SUBFRAME", 3);
    out.slot                     = parse_env_or_default("SRSRAN_OFH_DUMP_SLOT", 7);
    out.symbol                   = parse_env_or_default("SRSRAN_OFH_DUMP_SYMBOL", 12);
    out.port                     = parse_env_or_default("SRSRAN_OFH_DUMP_PORT", 0);
    out.sfn_mod                  = parse_env_or_default("SRSRAN_OFH_DUMP_SFN_MOD", 0);
    const char* path_env         = std::getenv("SRSRAN_OFH_DUMP_PATH");
    out.path_prefix              = (path_env != nullptr && *path_env != '\0') ? path_env : "/tmp/ofh_ul";
    return out;
  }();
  return cfg;
}

void maybe_log_dump_status(const ofh_ul_dump_config& cfg,
                           unsigned                 sector_id,
                           const slot_point&        slot,
                           unsigned                 symbol,
                           unsigned                 rg_port,
                           srslog::basic_logger&    logger)
{
  if (!cfg.enabled) {
    return;
  }

  static std::atomic<bool> logged_cfg{false};
  if (!logged_cfg.exchange(true)) {
    logger.warning(
        "Sector#{}: OFH UL dump enabled: subframe={} slot={} symbol={} port={} sfn_mod={} path_prefix='{}'",
        sector_id,
        cfg.subframe,
        cfg.slot,
        cfg.symbol,
        cfg.port,
        cfg.sfn_mod,
        cfg.path_prefix);
    std::cerr << "[OFH] UL dump enabled: sector=" << sector_id
              << " subframe=" << cfg.subframe
              << " slot=" << cfg.slot
              << " symbol=" << cfg.symbol
              << " port=" << cfg.port
              << " sfn_mod=" << cfg.sfn_mod
              << " path_prefix='" << cfg.path_prefix << "'"
              << std::endl;
  }

  static std::atomic<unsigned> log_count{0};
  unsigned                     count = log_count.fetch_add(1);
  if (count < 20) {
    const bool sfn_mod_match = (cfg.sfn_mod == 0) || ((slot.sfn() % cfg.sfn_mod) == 0);
    logger.warning(
        "Sector#{}: OFH UL dump check: sfn={} subframe={} slot_in_sf={} slot_in_frame={} symbol={} port={} sfn_mod_match={} (target sf={} slot={} sym={} port={} sfn_mod={})",
        sector_id,
        slot.sfn(),
        slot.subframe_index(),
        slot.subframe_slot_index(),
        slot.slot_index(),
        symbol,
        rg_port,
        sfn_mod_match ? 1 : 0,
        cfg.subframe,
        cfg.slot,
        cfg.symbol,
        cfg.port,
        cfg.sfn_mod);
    std::cerr << "[OFH] UL dump check: sector=" << sector_id
              << " sfn=" << slot.sfn()
              << " subframe=" << slot.subframe_index()
              << " slot_in_sf=" << slot.subframe_slot_index()
              << " slot_in_frame=" << slot.slot_index()
              << " symbol=" << symbol
              << " port=" << rg_port
              << " sfn_mod_match=" << (sfn_mod_match ? 1 : 0)
              << " target(subframe=" << cfg.subframe
              << " slot=" << cfg.slot
              << " symbol=" << cfg.symbol
              << " port=" << cfg.port
              << " sfn_mod=" << cfg.sfn_mod << ")"
              << std::endl;
  }
}

bool should_dump_ul_symbol(const ofh_ul_dump_config& cfg,
                           const slot_point&        slot,
                           unsigned                 symbol,
                           unsigned                 rg_port)
{
  if (!cfg.enabled) {
    return false;
  }
  if (rg_port != cfg.port) {
    return false;
  }
  if (cfg.sfn_mod != 0 && (slot.sfn() % cfg.sfn_mod) != 0) {
    return false;     
  }
  if (slot.subframe_index() != cfg.subframe) {
    return false;
  }
  if (symbol != cfg.symbol) {
    return false;
  }
  if (slot.subframe_slot_index() != cfg.slot && slot.slot_index() != cfg.slot) {
    return false;
  }

  return true;
}

std::string build_dump_path(const ofh_ul_dump_config&    cfg,
                            unsigned                    sfn,
                            unsigned                    rg_port,
                            unsigned                    seq,       
                            const uplane_section_params& section)
{
  std::string path = cfg.path_prefix;
  path += "_sfn" + std::to_string(sfn);
  path += "_sf" + std::to_string(cfg.subframe);
  path += "_slot" + std::to_string(cfg.slot);
  path += "_sym" + std::to_string(cfg.symbol);
  path += "_port" + std::to_string(rg_port);
  path += "_sec" + std::to_string(section.section_id);
  path += "_prb" + std::to_string(section.start_prb);
  path += "_n" + std::to_string(section.nof_prbs);
  path += "_seq" + std::to_string(seq);
  path += ".cbf16";
  return path;
}

void dump_ul_section_iq(const ofh_ul_dump_config&    cfg,
                        unsigned                    sector_id,
                        const slot_point&           slot,
                        unsigned                    symbol,
                        unsigned                    rg_port,
                        const uplane_section_params& section,
                        span<const cbf16_t>         samples,
                        srslog::basic_logger&       logger)
{
  static std::atomic<unsigned> dump_seq{0};
  unsigned                     seq  = dump_seq.fetch_add(1);
  std::string                  path = build_dump_path(cfg, slot.sfn(), rg_port, seq, section);
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    logger.warning("Sector#{}: failed to open OFH dump file '{}'", sector_id, path);
    return;
  }

  out.write(reinterpret_cast<const char*>(samples.data()), sizeof(cbf16_t) * samples.size());
  logger.info("Sector#{}: dumped OFH UL IQ for slot '{}' symbol '{}' port {} to '{}' ({} samples)",
              sector_id,
              slot,
              symbol,
              rg_port,
              path,
              samples.size());
}

} // namespace

void uplane_rx_symbol_data_flow_writer::write_to_resource_grid(unsigned                              eaxc,
                                                               const uplane_message_decoder_results& results)
{
  trace_point access_repo_tp = ofh_tracer.now();

  slot_point            slot       = results.params.slot;
  unsigned              symbol     = results.params.symbol_id;
  const uplink_context& ul_context = ul_context_repo->get(slot, symbol);
  if (ul_context.empty()) {
    logger.warning(
        "Sector#{}: dropped received Open Fronthaul message as no uplink slot context was found for slot '{}', symbol "
        "'{}' and eAxC '{}'",
        sector_id,
        results.params.slot,
        results.params.symbol_id,
        eaxc);

    return;
  }
  ofh_tracer << trace_event("ofh_receiver_access_repo", access_repo_tp);

  // Find resource grid port with eAxC.
  unsigned rg_port = std::distance(ul_eaxc.begin(), std::find(ul_eaxc.begin(), ul_eaxc.end(), eaxc));
  srsran_assert(rg_port < ul_eaxc.size(), "Invalid resource grid port value '{}'", rg_port);

  const ofh_ul_dump_config& dump_cfg = get_ul_dump_config();
  maybe_log_dump_status(dump_cfg, sector_id, slot, symbol, rg_port, logger);
  const bool                do_dump = should_dump_ul_symbol(dump_cfg, slot, symbol, rg_port);

  // The DU cell bandwidth may be narrower than the operating bandwidth of the RU.
  unsigned du_nof_prbs = ul_context.get_grid_nof_prbs();
  for (const auto& section : results.sections) {
    // Drop the whole section when all PRBs are outside the range of the DU bandwidth and the operating bandwidth of the
    // RU is larger.
    if (section.start_prb >= du_nof_prbs) {
      continue;
    }

    // At this point, we have to care about the following cases:
    //   a) The last PRB of the section falls outside the range of the DU cell bandwidth.
    //   b) The last PRB of the section falls inside the range of the DU cell bandwidth.

    // Take care of case (a), takes the first N PRBs inside the section.
    unsigned nof_prbs_to_write = du_nof_prbs - section.start_prb;
    // Take care of case (b), takes all the PRBs inside the section.
    if (section.start_prb + section.nof_prbs < du_nof_prbs) {
      nof_prbs_to_write = section.nof_prbs;
    }

    trace_point write_rg_tp = ofh_tracer.now();
    span<const cbf16_t> samples =
        span<const cbf16_t>(section.iq_samples).first(nof_prbs_to_write * NOF_SUBCARRIERS_PER_RB);
    if (do_dump && !samples.empty()) {
      dump_ul_section_iq(dump_cfg, sector_id, slot, symbol, rg_port, section, samples, logger);
    }

    ul_context_repo->write_grid(
        slot,
        rg_port,
        symbol,
        section.start_prb * NOF_SUBCARRIERS_PER_RB,
        samples);

    ofh_tracer << trace_event("ofh_receiver_write_rg", write_rg_tp);

    logger.debug(
        "Sector#{}: written IQ data into UL resource grid PRB range [{},{}), for slot '{}', symbol '{}' and port '{}'",
        sector_id,
        section.start_prb,
        section.start_prb + nof_prbs_to_write,
        slot,
        symbol,
        rg_port);
  }
}
