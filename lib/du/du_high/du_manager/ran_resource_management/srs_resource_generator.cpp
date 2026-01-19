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

#include "srs_resource_generator.h"

using namespace srsran;
using namespace srs_du;

std::vector<du_srs_resource> srsran::srs_du::generate_cell_srs_list(const du_cell_config& du_cell_cfg)
{
  std::vector<du_srs_resource> srs_res_list;
  // TX comb offsets values, depending on the TX comb value, as per TS 38.331, \c transmissionComb, \c SRS-Resource,
  // \c SRS-Config.
  std::vector<unsigned> tx_comb_offsets = du_cell_cfg.srs_cfg.tx_comb == srsran::tx_comb_size::n2
                                              ? std::vector<unsigned>{0U, 1U}
                                              : std::vector<unsigned>{0U, 1U, 2U, 3U};

  // Cyclic Shifts values, depending on the TX comb value, as per TS 38.331, \c cyclicShift, \c SRS-Resource,
  // \c SRS-Config.
  const unsigned        max_cs  = du_cell_cfg.srs_cfg.tx_comb == srsran::tx_comb_size::n2 ? 8U : 12U;
  const unsigned        cs_step = max_cs / static_cast<unsigned>(du_cell_cfg.srs_cfg.cyclic_shift_reuse_factor);
  std::vector<unsigned> cs_values;
  for (unsigned cs = 0; cs < max_cs; cs += cs_step) {
    cs_values.push_back(cs);
  }

  // Compute the available Sequence IDs.
  // NOTE: we only consider the number of orthogonal sequences that can be generated, as per TS 38.211,
  // Section 6.4.1.4.2, which is 30.
  // NOTE: Contiguous PCIs will be assigned different (sequence ID % 30) values, which means that their
  // SRS resources will be orthogonal.
  constexpr unsigned max_seq_id_values = 30U;
  constexpr unsigned nof_sequence_ids  = 1024;
  const unsigned seq_id_step = max_seq_id_values / static_cast<unsigned>(du_cell_cfg.srs_cfg.sequence_id_reuse_factor);
  std::vector<unsigned> seq_id_values;
  for (unsigned seq_id = 0; seq_id < max_seq_id_values; seq_id += seq_id_step) {
    seq_id_values.push_back((static_cast<unsigned>(du_cell_cfg.pci) + seq_id) % nof_sequence_ids);
  }

  // Force SRS resources to use a fixed symbol start.
  static constexpr unsigned fixed_srs_symbol_start = 12U;
  const unsigned           nof_srs_symbols         = static_cast<unsigned>(du_cell_cfg.srs_cfg.nof_symbols);
  srsran_assert(fixed_srs_symbol_start + nof_srs_symbols <= NOF_OFDM_SYM_PER_SLOT_NORMAL_CP,
                "Fixed SRS symbol start {} with nof_symbols {} exceeds slot size",
                fixed_srs_symbol_start,
                nof_srs_symbols);

  const ofdm_symbol_range srs_res_symbols{fixed_srs_symbol_start, fixed_srs_symbol_start + nof_srs_symbols};

  // We use the counter to define the cell resource ID.
  unsigned srs_res_cnt = 0;
  for (auto tx_comb_offset : tx_comb_offsets) {
    for (auto cs : cs_values) {
      for (auto seq_id : seq_id_values) {
        du_srs_resource srs_res;
        srs_res.cell_res_id    = srs_res_cnt;
        srs_res.tx_comb_offset = tx_comb_offset;
        srs_res.symbols        = srs_res_symbols;
        srs_res.sequence_id    = seq_id;
        srs_res.cs             = cs;
        srs_res_list.push_back(srs_res);
        ++srs_res_cnt;
      }
    }
  }
  return srs_res_list;
}
