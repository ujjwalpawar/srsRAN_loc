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

#include "srsran/adt/bounded_bitset.h"
#include "srsran/adt/complex.h"
#include "srsran/adt/span.h"
#include "srsran/phy/support/re_buffer.h"
#include "srsran/phy/support/time_alignment_estimator/time_alignment_measurement.h"
#include "srsran/ran/resource_block.h"
#include "srsran/ran/subcarrier_spacing.h"

namespace srsran {

/// Time alignment estimator interface.
class time_alignment_estimator
{
public:
  /// \brief Maximum number of symbols that can be used for estimating the time alignment.
  ///
  /// It is equal to the maximum number of subcarriers that can be contained in an OFDM symbol.
  static constexpr unsigned max_nof_symbols = MAX_NOF_PRBS * NOF_SUBCARRIERS_PER_RB;

  /// Default destructor.
  virtual ~time_alignment_estimator() = default;

  /// \brief Estimates the time alignment from frequency domain symbols from a single antenna port.
  /// \param[in] symbols Complex frequency domain symbols.
  /// \param[in] mask    Distribution of the complex symbols within an OFDM symbol.
  /// \param[in] scs     Subcarrier spacing.
  /// \param[in] max_ta  Maximum absolute time alignment measurement if it is not zero.
  /// \return The measured time alignment.
  /// \remark An assertion is triggered if the number of symbols is not equal to the number of active elements in the
  /// mask, or if the mask size is larger than the maximum supported number of subcarriers.
  virtual time_alignment_measurement estimate(span<const cf_t>                symbols,
                                              bounded_bitset<max_nof_symbols> mask,
                                              subcarrier_spacing              scs,
                                              double                          max_ta = 0.0) = 0;

  /// \brief Estimates the time alignment from frequency domain symbols from one or more antenna ports.
  /// \param[in] symbols Complex frequency domain symbols (one slice for each antenna port).
  /// \param[in] mask    Distribution of the complex symbols within an OFDM symbol.
  /// \param[in] scs     Subcarrier spacing.
  /// \param[in] max_ta  Maximum absolute time alignment measurement if it is not zero.
  /// \return The measured time alignment.
  /// \remark An assertion is triggered if the number of symbols is not equal to the number of active elements in the
  /// mask, or if the mask size is larger than the maximum supported number of subcarriers.
  virtual time_alignment_measurement estimate(const re_buffer_reader<cf_t>&   symbols,
                                              bounded_bitset<max_nof_symbols> mask,
                                              subcarrier_spacing              scs,
                                              double                          max_ta = 0.0) = 0;

  /// \brief Estimates the time alignment from frequency domain symbols from a single antenna port.
  /// \param[in] symbols Complex frequency domain symbols.
  /// \param[in] stride  Distance between the complex symbols within an OFDM symbol.
  /// \param[in] scs     Subcarrier spacing.
  /// \param[in] max_ta  Maximum absolute time alignment measurement if it is not zero.
  /// \return The measured time alignment.
  /// \remark An assertion is triggered if the number of symbols times the stride exceed the frequency domain buffer.
  virtual time_alignment_measurement
  estimate(span<const cf_t> symbols, unsigned stride, subcarrier_spacing scs, double max_ta = 0.0) = 0;

  /// \brief Estimates the time alignment from frequency domain symbols from one or more antenna ports.
  /// \param[in] symbols Complex frequency domain symbols (one slice for each antenna port).
  /// \param[in] stride  Distance between the complex symbols within an OFDM symbol.
  /// \param[in] scs     Subcarrier spacing.
  /// \param[in] max_ta  Maximum absolute time alignment measurement if it is not zero.
  /// \return The measured time alignment.
  /// \remark An assertion is triggered if the number of symbols times the stride exceed the frequency domain buffer.
  virtual time_alignment_measurement
  estimate(const re_buffer_reader<cf_t>& symbols, unsigned stride, subcarrier_spacing scs, double max_ta = 0.0) = 0;

    /// \brief Estimates the time alignment from frequency domain symbols from one or more antenna ports.
  /// \param[in] symbols Complex frequency domain symbols (one slice for each antenna port).
  /// \param[in] stride  Distance between the complex symbols within an OFDM symbol.
  /// \param[in] scs     Subcarrier spacing.
  /// \param[in] max_ta  Maximum absolute time alignment measurement if it is not zero.
  /// \param[in] filename  Filename to save the symbols
  /// \param[in] rnti    C-RNTI of the UE being estimated
  /// \param[in] subframe_index  Subframe index within the radio frame (0..9)
  /// \param[in] slot_index  Slot index within the radio frame
  /// \param[in] srs_symbols  OFDM symbol indices carrying SRS (per UE)
  /// \param[in] srs_subcarriers  Subcarrier indices carrying SRS (per UE)
  /// \param[in] srs_sequence  SRS sequence used for channel estimation (complex, per UE)
  /// \param[in] raw_symbol_index  OFDM symbol index for raw full-symbol capture (0xFFFF if unused)
  /// \param[in] raw_nof_ports  Number of RX ports included in raw_symbol_iq
  /// \param[in] raw_nof_subcarriers  Number of subcarriers per RX port in raw_symbol_iq
  /// \param[in] raw_symbol_iq  Raw received IQ samples for the full symbol (I/Q interleaved, per port)
  /// \return The measured time alignment.
  /// \remark An assertion is triggered if the number of symbols times the stride exceed the frequency domain buffer.
  virtual time_alignment_measurement
  estimate_with_logfile(const re_buffer_reader<cf_t>& symbols,
                        unsigned                      stride,
                        subcarrier_spacing            scs,
                        double                        max_ta         = 0.0,
                        std::string                   filename       = " ",
                        uint16_t                      rnti           = 0,
                        uint16_t                      subframe_index = 0,
                        uint16_t                      slot_index     = 0,
                        span<const uint16_t>          srs_symbols    = span<const uint16_t>{},
                        span<const uint16_t>          srs_subcarriers = span<const uint16_t>{},
                        span<const cf_t>              srs_sequence = span<const cf_t>{},
                        uint16_t                      raw_symbol_index = 0xFFFF,
                        uint16_t                      raw_nof_ports = 0,
                        uint32_t                      raw_nof_subcarriers = 0,
                        span<const cf_t>              raw_symbol_iq = span<const cf_t>{}) = 0;
};

} // namespace srsran
