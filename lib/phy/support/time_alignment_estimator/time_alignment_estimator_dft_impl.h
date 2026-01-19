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
#include "srsran/phy/generic_functions/dft_processor.h"
#include "srsran/phy/support/re_buffer.h"
#include "srsran/phy/support/time_alignment_estimator/time_alignment_estimator.h"
#include "srsran/ran/phy_time_unit.h"
#include "srsran/srsvec/zero.h"
#include "srsran/support/math/math_utils.h"
#include <unordered_map>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>

namespace srsran {

/// UDP packet header structure
struct __attribute__((packed)) iq_udp_packet_header {
  uint64_t timestamp;
  char imeisv[16];
  uint16_t c_rnti;
  uint16_t ta_flags;                 // Bit-mask flags (bit0=1 -> TA came from RAR)
  int32_t ta_value;
  uint64_t ta_update_time;           // Time when TA was last updated
  uint16_t subframe_index;           // Subframe index within the radio frame (0..9)
  uint16_t slot_index;               // Slot index within the radio frame
  uint16_t nof_symbols;              // Number of OFDM symbols carrying SRS
  uint16_t nof_subcarriers;          // Number of subcarriers carrying SRS
  uint32_t nof_correlation;
  uint32_t nof_iq_samples;
  uint32_t nof_slices;               // Number of antenna slices
};

/// UDP packet with dynamic data
struct iq_udp_packet {
  iq_udp_packet_header header;
  std::vector<float> correlation;    // Real correlation values
  std::vector<float> iq_samples;     // Freq-domain symbols from ALL slices (flattened, I/Q interleaved)
  std::vector<uint16_t> srs_symbols;     // OFDM symbol indices carrying SRS
  std::vector<uint16_t> srs_subcarriers; // Subcarrier indices carrying SRS
};

/// Asynchronous UDP sender using lock-free queue
class async_udp_sender {
public:
  async_udp_sender(const std::string& ip, uint16_t port);
  ~async_udp_sender();
  
  // Non-blocking enqueue
  bool send_async(const iq_udp_packet& packet);
  
  // Get queue statistics
  size_t get_queue_size() const { return queue_size_.load(); }
  uint64_t get_dropped_count() const { return dropped_packets_.load(); }
  
private:
  void sender_thread();
  
  int sockfd_;
  struct sockaddr_in dest_addr_;
  
  std::queue<iq_udp_packet> packet_queue_;
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::atomic<size_t> queue_size_{0};
  std::atomic<uint64_t> dropped_packets_{0};
  
  std::thread sender_thread_obj_;
  std::atomic<bool> running_{true};
  
  static constexpr size_t MAX_QUEUE_SIZE = 100;
};

/// DFT-based implementation of the time alignment estimator.
class time_alignment_estimator_dft_impl : public time_alignment_estimator
{
public:
  /// Maximum number of PRB.
  static constexpr unsigned max_nof_re = MAX_NOF_PRBS * NOF_SUBCARRIERS_PER_RB;

  /// Minimum DFT size to satisfy the minimum TA measurement.
  static const unsigned min_dft_size;

  /// Maximum DFT size to fit all the channel bandwidth.
  static const unsigned max_dft_size;

  /// Collection of DFT processors type.
  using collection_dft_processors = std::unordered_map<unsigned, std::unique_ptr<dft_processor>>;

  /// Required DFT direction.
  static constexpr dft_processor::direction dft_direction = dft_processor::direction::INVERSE;

  /// \brief Creates the time alignment estimator instance.
  ///
  /// \remark An assertion is triggered if the DFT processor direction or the DFT size are not as expected.
  time_alignment_estimator_dft_impl(collection_dft_processors dft_processors_);

  // See interface for documentation.
  time_alignment_measurement estimate(span<const cf_t>                symbols,
                                      bounded_bitset<max_nof_symbols> mask,
                                      subcarrier_spacing              scs,
                                      double                          max_ta) override;

  // See interface for documentation.
  time_alignment_measurement estimate(const re_buffer_reader<cf_t>&   symbols,
                                      bounded_bitset<max_nof_symbols> mask,
                                      subcarrier_spacing              scs,
                                      double                          max_ta) override;

  // See interface for documentation.
  time_alignment_measurement
  estimate(span<const cf_t> symbols, unsigned stride, subcarrier_spacing scs, double max_ta) override;

  // See interface for documentation.
  time_alignment_measurement
  estimate(const re_buffer_reader<cf_t>& symbols, unsigned stride, subcarrier_spacing scs, double max_ta) override;

  // See interface for documentation.
  time_alignment_measurement
  estimate_with_logfile(const re_buffer_reader<cf_t>& symbols,
                        unsigned                      stride,
                        subcarrier_spacing            scs,
                        double                        max_ta,
                        std::string                   filename,
                        uint16_t                      rnti,
                        uint16_t                      subframe_index,
                        uint16_t                      slot_index,
                        span<const uint16_t>          srs_symbols,
                        span<const uint16_t>          srs_subcarriers) override;

private:
  /// DFT processors.
  collection_dft_processors dft_processors;

  /// Get the DFT that best suits the maximum number of resource elements.
  dft_processor& get_idft(unsigned nof_required_re);

  /// Estimates the TA assuming the complex symbols are already in the DFT input.
  static time_alignment_measurement
  estimate_ta_correlation(span<const float> correlation, unsigned stride, subcarrier_spacing scs, double max_ta);

  /// Buffer for storing the IDFT magnitude square.
  std::vector<float> idft_abs2;
  
  /// Async UDP sender for IQ samples (optional, lazy initialized)
  mutable std::unique_ptr<async_udp_sender> udp_sender_;
}; // namespace srsran

} // namespace srsran
