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

#include "time_alignment_estimator_dft_impl.h"
#include "srsran/adt/bounded_bitset.h"
#include "srsran/adt/complex.h" 
#include "srsran/adt/span.h"
#include "srsran/phy/support/re_buffer.h"
#include "srsran/phy/support/time_alignment_estimator/time_alignment_measurement.h"
#include "srsran/ran/subcarrier_spacing.h"
#include "srsran/srsvec/compare.h"
#include "srsran/srsvec/copy.h"
#include "srsran/srsvec/dot_prod.h"
#include "srsran/srsvec/modulus_square.h"
#include <algorithm>
#include <utility>
#include <iostream>
#include <iostream>
#include <fstream>
#include <iomanip>  // For std::scientific and std::setprecision
#include <unistd.h>
#include <fcntl.h>
#include "srsran/scheduler/ta_shared.h"
#include "srsran/scheduler/ue_identity_tracker.h"
using namespace srsran;

// Async UDP sender implementation
async_udp_sender::async_udp_sender(const std::string& ip, uint16_t port) {
  // Create UDP socket
  sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd_ < 0) {
    std::cerr << "[UDP_SENDER] Failed to create socket" << std::endl;
    return;
  }
  
  // Set socket to non-blocking
  int flags = fcntl(sockfd_, F_GETFL, 0);
  fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);
  
  // Configure destination address
  std::memset(&dest_addr_, 0, sizeof(dest_addr_));
  dest_addr_.sin_family = AF_INET;
  dest_addr_.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &dest_addr_.sin_addr) <= 0) {
    std::cerr << "[UDP_SENDER] Invalid IP address: " << ip << std::endl;
    close(sockfd_);
    sockfd_ = -1;
    return;
  }
  
  // Start sender thread
  sender_thread_obj_ = std::thread(&async_udp_sender::sender_thread, this);
  std::cout << "[UDP_SENDER] Initialized: " << ip << ":" << port << std::endl;
}

async_udp_sender::~async_udp_sender() {
  running_ = false;
  queue_cv_.notify_all();
  
  if (sender_thread_obj_.joinable()) {
    sender_thread_obj_.join();
  }
  
  if (sockfd_ >= 0) {
    close(sockfd_);
  }
  
  std::cout << "[UDP_SENDER] Shutdown. Dropped packets: " << dropped_packets_.load() << std::endl;
}

bool async_udp_sender::send_async(const iq_udp_packet& packet) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  
  // Drop packet if queue is full
  if (packet_queue_.size() >= MAX_QUEUE_SIZE) {
    dropped_packets_++;
    return false;
  }
  
  packet_queue_.push(packet);
  queue_size_ = packet_queue_.size();
  lock.unlock();
  
  queue_cv_.notify_one();
  return true;
}

void async_udp_sender::sender_thread() {
  while (running_) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    
    // Wait for packets or shutdown
    queue_cv_.wait(lock, [this] { return !packet_queue_.empty() || !running_; });
    
    if (!running_ && packet_queue_.empty()) {
      break;
    }
    
    if (packet_queue_.empty()) {
      continue;
    }
    
    // Get packet from queue
    iq_udp_packet packet = packet_queue_.front();
    packet_queue_.pop();
    queue_size_ = packet_queue_.size();
    lock.unlock();
    
    // Send without blocking main thread
    if (sockfd_ >= 0) {
      // Calculate total size: header + correlation data + IQ data
      size_t total_size = sizeof(iq_udp_packet_header) + 
                         packet.correlation.size() * sizeof(float) +
                         packet.iq_samples.size() * sizeof(float);
      
      // Allocate buffer for complete packet
      std::vector<uint8_t> buffer(total_size);
      uint8_t* ptr = buffer.data();
      
      // Copy header
      std::memcpy(ptr, &packet.header, sizeof(iq_udp_packet_header));
      ptr += sizeof(iq_udp_packet_header);
      
      // Copy correlation data
      if (!packet.correlation.empty()) {
        std::memcpy(ptr, packet.correlation.data(), packet.correlation.size() * sizeof(float));
        ptr += packet.correlation.size() * sizeof(float);
      }
      
      // Copy IQ samples
      if (!packet.iq_samples.empty()) {
        std::memcpy(ptr, packet.iq_samples.data(), packet.iq_samples.size() * sizeof(float));
      }
      
      ssize_t sent = sendto(sockfd_, buffer.data(), total_size, 0,
                           (struct sockaddr*)&dest_addr_, sizeof(dest_addr_));
      if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Only log persistent errors
        static uint64_t error_count = 0;
        if (++error_count % 1000 == 0) {
          std::cerr << "[UDP_SENDER] Send error (count: " << error_count << ")" << std::endl;
        }
      }
    }
  }
}

/// \brief Estimates a fractional sample delay from samples around a maximum.
///
/// It uses a quadratic curve fitting of the input values to estimate the peak position. The result is expressed as a
/// fraction of the sampling time with respect to the center input sample.
///
/// When the peak estimation is not meaningful, the function returns zero, meaning that one should consider the
/// maximum input as the peak and no fractional refinement is possible. This happens if:
///   - the number of input samples is neither three nor five; or
///   - the estimation results in NaN or infinity.
///
/// \param[in] peak_center_correlation Odd number of samples containing a peak maximum in the center.
/// \return The fractional sample estimation where the maximum is located if the result is valid.
float fractional_sample_delay(span<const float> peak_center_correlation)
{
  // Calculation coefficients for solving the equations.
  static constexpr std::array<float, 5> num_weights_5 = {{-0.400000, -0.200000, 0.000000, 0.200000, 0.400000}};
  static constexpr std::array<float, 5> den_weights_5 = {{0.571429, -0.285714, -0.571429, -0.285714, 0.571429}};
  static constexpr std::array<float, 3> num_weights_3 = {{-0.5, 0.0, 0.5}};
  static constexpr std::array<float, 3> den_weights_3 = {{0.5, -1.0, 0.5}};

  float             correction = 1.0F;
  span<const float> num_weights;
  span<const float> den_weights;

  // Select weight depending on the number of samples.
  if (peak_center_correlation.size() == 5) {
    num_weights = num_weights_5;
    den_weights = den_weights_5;
  } else if (peak_center_correlation.size() == 3) {
    correction  = 0.5;
    num_weights = num_weights_3;
    den_weights = den_weights_3;
  } else {
    // The size is invalid.
    return 0.0F;
  }

  // Solve equation.
  float num    = srsvec::dot_prod(num_weights, peak_center_correlation, 0.0F);
  float den    = srsvec::dot_prod(den_weights, peak_center_correlation, 0.0F);
  float result = -correction * num / den;

  // Make sure the function does not return a result greater than one, lower than minus one, infinity or NaN.
  if (std::isnan(result) || std::isinf(result) || std::abs(result) > 1.0F) {
    return 0.0F;
  }

  return result;
}

const unsigned time_alignment_estimator_dft_impl::min_dft_size = pow2(log2_ceil(static_cast<unsigned>(
    1.0F / (15000 * phy_time_unit::from_timing_advance(1, subcarrier_spacing::kHz15).to_seconds()))));

const unsigned time_alignment_estimator_dft_impl::max_dft_size = pow2(log2_ceil(max_nof_re));

time_alignment_estimator_dft_impl::time_alignment_estimator_dft_impl(
    srsran::time_alignment_estimator_dft_impl::collection_dft_processors dft_processors_) :
  dft_processors(std::move(dft_processors_)), idft_abs2(max_dft_size)
{
  // Make sure all the possible powers of 2 between the minimum and the maximum DFT sizes are present and valid.
  for (unsigned dft_size     = time_alignment_estimator_dft_impl::min_dft_size,
                dft_size_max = time_alignment_estimator_dft_impl::max_dft_size;
       dft_size <= dft_size_max;
       dft_size *= 2) {
    // Check the IDFT size is present.
    srsran_assert(dft_processors.count(dft_size), "Missing DFT.");

    // Select IDFT and validate.
    const auto& idft = dft_processors.at(dft_size);
    srsran_assert(idft, "Invalid DFT processor.");
    srsran_assert(idft->get_size() == dft_size, "Invalid DFT size.");
    srsran_assert(idft->get_direction() == dft_processor::direction::INVERSE, "Invalid DFT direction.");
  }
}

time_alignment_measurement time_alignment_estimator_dft_impl::estimate(const re_buffer_reader<cf_t>&   symbols,
                                                                       bounded_bitset<max_nof_symbols> mask,
                                                                       subcarrier_spacing              scs,
                                                                       double                          max_ta)
{

  srsran_assert(mask.count() == symbols.get_slice(0).size(),
                "The number of complex symbols per port {} does not match the mask size {}.",
                symbols.get_slice(0).size(),
                mask.count());
  unsigned       mask_highest = mask.find_highest();
  unsigned       mask_lowest  = mask.find_lowest();
  dft_processor& idft         = get_idft(mask_highest - mask_lowest + 1);

  // Get IDFT input buffer.
  span<cf_t> channel_observed_freq = idft.get_input();

  // Zero input buffer.
  srsvec::zero(channel_observed_freq);

  // Prepare correlation temporary buffer.
  span<float> correlation = span<float>(idft_abs2).first(idft.get_size());

  // Correlate each of the symbol slices.
  for (unsigned i_in = 0, max_in = symbols.get_nof_slices(); i_in != max_in; ++i_in) {
    // Get view of the input symbols for the given slice.
    span<const cf_t> symbols_in = symbols.get_slice(i_in);

    // Write the symbols in their corresponding positions.
    mask.for_each(
        0, mask.size(), [&channel_observed_freq, &symbols_in, mask_lowest, i_lse = 0U](unsigned i_re) mutable {
          channel_observed_freq[i_re - mask_lowest] = symbols_in[i_lse++];
        });

    // Perform correlation in frequency domain.
    span<const cf_t> channel_observed_time = idft.run();

    // Calculate the absolute square of the correlation.
    if (i_in == 0) {
      srsvec::modulus_square(correlation, channel_observed_time);
    } else {
      // Accumulate the correlation.
      srsvec::modulus_square_and_add(correlation, channel_observed_time, correlation);
    }
  }

  // Estimate the time alignment from the correlation.
  return estimate_ta_correlation(correlation, /* stride = */ 1, scs, max_ta);
}

time_alignment_measurement time_alignment_estimator_dft_impl::estimate(span<const cf_t>                symbols,
                                                                       bounded_bitset<max_nof_symbols> mask,
                                                                       subcarrier_spacing              scs,
                                                                       double                          max_ta)
{

  modular_re_buffer_reader<cf_t, 1> symbols_view(1, symbols.size());
  symbols_view.set_slice(0, symbols);

  return estimate(symbols_view, mask, scs, max_ta);
}

time_alignment_measurement time_alignment_estimator_dft_impl::estimate_with_logfile(const re_buffer_reader<cf_t>& symbols,
                                                                       unsigned                      stride,
                                                                       srsran::subcarrier_spacing    scs,
                                                                       double                        max_ta,
                                                                      std::string                   filename,
                                                                      uint16_t                      rnti)
{ //ujjwal : this esitmate function is called for srs/pucch dmrs 
  // Extract timestamp from filename parameter (which contains just the timestamp)
  uint64_t timestamp_ns = 0;
  try {
    // filename is already just the timestamp string
    timestamp_ns = std::stoull(filename);
  } catch (...) {
    // If parsing fails, use current time
    timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }
  
  // Get IMEISV directly from C-RNTI (passed as parameter)
  std::string imeisv_str = "unknown";
  uint16_t c_rnti_val = rnti;
  int ta_value = 0;
  uint64_t ta_update_time_ns = 0;
  bool ta_is_rar = false;
  
  // Try to get IMEISV from C-RNTI
  if (c_rnti_val != 0) {
    if (!ue_identity_tracker::get_imeisv_by_crnti(c_rnti_val, imeisv_str)) {
      imeisv_str = "unknown_crnti_" + std::to_string(c_rnti_val);
    }

    long long unsigned int ta_timestamp_tmp = 0;
    int                    ta_value_tmp     = 0;
    bool                   is_rar_tmp       = false;
    uint16_t               ta_crnti         = 0;
    if (get_last_ta_with_crnti(ta_timestamp_tmp, ta_value_tmp, is_rar_tmp, ta_crnti) && ta_crnti == c_rnti_val) {
      ta_value = ta_value_tmp;
      ta_update_time_ns = ta_timestamp_tmp;
      ta_is_rar = is_rar_tmp;
    }
  }
  
  // Lazy initialize UDP sender (only once)
  static const char* UDP_IP = std::getenv("SRSRAN_IQ_UDP_IP");
  static const char* UDP_PORT = std::getenv("SRSRAN_IQ_UDP_PORT");
  if (UDP_IP && UDP_PORT && !udp_sender_) {
    try {
      udp_sender_ = std::make_unique<async_udp_sender>(UDP_IP, std::atoi(UDP_PORT));
    } catch (...) {
      std::cerr << "[UDP_SENDER] Failed to initialize" << std::endl;
    }
  }
  
  // filename = "iq_data_tdft_correlation_" + imeisv_str + "_" + filename + ".txt";
  // std::ofstream file(filename);
  unsigned       nof_symbols = symbols.get_nof_re();
  dft_processor& idft        = get_idft(nof_symbols);
  
  // Store all frequency-domain symbols from all antenna slices for UDP transmission
  std::vector<std::vector<cf_t>> all_symbols_freq;
  
  // Write IMEISV and TA info to file
  // file << "IMEISV=" << imeisv_str << std::endl;
  // try {
  //     long long unsigned int time_now;
  //     int new_ta;
  //     bool is_rar_ta;

  //     if (get_last_ta(time_now, new_ta, is_rar_ta)) {
  //         file << "Time when t_A was updated =" << time_now << " new_t_a=" << new_ta <<" is rar ta="<< is_rar_ta << std::endl;
  //       }
  //     }
  //    catch (...) {
  //     /* ignore */
  // }
  // Get IDFT input buffer.
  span<cf_t> channel_observed_freq = idft.get_input();

  // Zero input buffer.
  srsvec::zero(channel_observed_freq.last(channel_observed_freq.size() - nof_symbols));

  // Prepare correlation temporary buffer.
  span<float> correlation = span<float>(idft_abs2).first(idft.get_size());
  //ujjwal debug start
  // std::cout<<"correlation size "<<correlation.size()<<std::endl;
  // std::cout<<"idft size "<<channel_observed_freq.size()<<std::endl; 
  // std::cout<<"nof symbols "<<nof_symbols<<std::endl;
  // std::cout<<"zeros "<<channel_observed_freq.size() - nof_symbols<<std::endl;
  //ujjwal debug end
  // Correlate each of the symbol slices.
  for (unsigned i_in = 0, max_in = symbols.get_nof_slices(); i_in != max_in; ++i_in) {

    // Get view of the input symbols for the given slice.
    span<const cf_t> symbols_in = symbols.get_slice(i_in);
    
    //ujjwal debug start
    
    // std::cout<<"symbols"<<std::endl;
    // std::cout << "Slice " << i_in<< ": ";
    // if (symbols_in.empty()) {
    //     std::cout << "Empty slice" << std::endl;
    // } else {
    //     std::cout << "Elements: ";
    //     for (const auto& val : symbols_in) {
    //         std::cout << "(" << val.real() << ", " << val.imag() << ") ";
    //     }
    //     std::cout <<std::flush<< std::endl;
    // }
    
    //save into file

    // file << "symbols" << std::endl;
    // file << "Slice " << i_in<< ": ";

    // if (symbols_in.empty()) {
    //     file << "Empty slice" << std::endl;
    // } else {
    //     file << "Elements: ";
    // }
    // for (const auto& val : symbols_in) {

    //     file << "(" << val.real() << ", " << val.imag() << ") ";
    // }
    // file << std::endl;
    
    // Store frequency domain symbols from all slices for UDP transmission
    std::vector<cf_t> slice_symbols(symbols_in.begin(), symbols_in.end());
    all_symbols_freq.push_back(slice_symbols);
    
    //ujjwal debug end
    // Write the symbols in their corresponding positions.
    srsvec::copy(channel_observed_freq.first(nof_symbols), symbols_in);

    // Perform correlation in frequency domain.
    span<const cf_t> channel_observed_time = idft.run();
    // std::cout<<"channel_observed time size "<<channel_observed_time.size()<<std::endl;
    // std::cout<<"channel_observed time "<<channel_observed_time[1023]<<std::endl;
    //ujjwal debug start
    // file << "channel_observed time" << std::endl;
    // file << "Slice " << i_in << ": ";
    // if (channel_observed_time.empty()) {
    //     file << "Empty slice" << std::endl;
    // } else {
    //     file << "Elements: ";
    // }
    // for (const auto& val : channel_observed_time) {
    //     count++;
    //     file << "(" << val.real() << ", " << val.imag() << ") ";
    // }
    // file << std::endl;
    // std::cout<<"count "<<count<<std::endl;

    //ujjwal debug start
    // Calculate the absolute square of the correlation.
    if (i_in == 0) {
      srsvec::modulus_square(correlation, channel_observed_time);
    } else {
      // Accumulate the correlation.
      srsvec::modulus_square_and_add(correlation, channel_observed_time, correlation);
    }
  }
  //ujjwal debug start
  /*
  std::cout<<"correlation"<<std::endl;
        for (const auto& val : correlation) {
            std::cout << val << ", " ;
        }
        std::cout << std::endl;
    
  
  std::cout<<"stride"<<stride<<std::endl;
  std::cout<<"scs"<<scs_to_khz(scs)<<std::endl;
  std::cout<<"max_ta"<<max_ta<<std::endl;
  */
  
  // Send correlation and frequency-domain symbols from all slices via UDP (non-blocking, before file I/O)
  if (udp_sender_) {
    try {
      iq_udp_packet packet;
      packet.header.timestamp = timestamp_ns;
      std::strncpy(packet.header.imeisv, imeisv_str.c_str(), sizeof(packet.header.imeisv) - 1);
      packet.header.imeisv[sizeof(packet.header.imeisv) - 1] = '\0';
      packet.header.c_rnti = c_rnti_val;
      packet.header.ta_flags = ta_is_rar ? 1 : 0;
      packet.header.ta_value = ta_value;
      packet.header.ta_update_time = ta_update_time_ns;
      
      // Copy ALL correlation values (no truncation)
      packet.header.nof_correlation = correlation.size();
      packet.correlation.resize(correlation.size());
      for (unsigned i = 0; i < correlation.size(); ++i) {
        packet.correlation[i] = correlation[i];
      }
      
      // Flatten ALL antenna slices into single array (interleaved I/Q) - no truncation
      // Format: [slice0_symbol0_I, slice0_symbol0_Q, slice0_symbol1_I, slice0_symbol1_Q, ..., slice1_symbol0_I, ...]
      packet.header.nof_slices = all_symbols_freq.size();
      unsigned total_samples = 0;
      
      // Calculate total samples from all slices
      for (const auto& slice : all_symbols_freq) {
        total_samples += slice.size();
      }
      
      // Copy all samples from all slices
      packet.header.nof_iq_samples = total_samples;
      packet.iq_samples.resize(total_samples * 2);
      unsigned sample_idx = 0;
      for (const auto& slice : all_symbols_freq) {
        for (const auto& symbol : slice) {
          packet.iq_samples[sample_idx * 2] = symbol.real();     // I
          packet.iq_samples[sample_idx * 2 + 1] = symbol.imag(); // Q
          sample_idx++;
        }
      }
      
      udp_sender_->send_async(packet);
    } catch (...) {
      /* ignore UDP errors */
    }
  }
  
  //save into file
  // file << std::scientific << std::setprecision(15);
  // file << "correlation" << std::endl;
  // for (const auto& val : correlation) {
  //     file << val << ", " ;
  // }
  // file << std::endl;
  // file << "stride " << stride << std::endl;
  // file << "scs " << scs_to_khz(scs) << std::endl;
  // file << "max_ta " << max_ta << std::endl;
  
  // file.close();
  //ujjwal debug end

  // Estimate the time alignment from the correlation. 
  return estimate_ta_correlation(correlation, stride, scs, max_ta);
}

time_alignment_measurement time_alignment_estimator_dft_impl::estimate(const re_buffer_reader<cf_t>& symbols,
  unsigned                      stride,
  srsran::subcarrier_spacing    scs,
  double                        max_ta)
{ 
unsigned       nof_symbols = symbols.get_nof_re();
dft_processor& idft        = get_idft(nof_symbols);

// Get IDFT input buffer.
span<cf_t> channel_observed_freq = idft.get_input();

// Zero input buffer.
srsvec::zero(channel_observed_freq.last(channel_observed_freq.size() - nof_symbols));

// Prepare correlation temporary buffer.
span<float> correlation = span<float>(idft_abs2).first(idft.get_size());
//ujjwal debug start

//ujjwal debug end
// Correlate each of the symbol slices.
for (unsigned i_in = 0, max_in = symbols.get_nof_slices(); i_in != max_in; ++i_in) {

// Get view of the input symbols for the given slice.
span<const cf_t> symbols_in = symbols.get_slice(i_in);



// Write the symbols in their corresponding positions.
srsvec::copy(channel_observed_freq.first(nof_symbols), symbols_in);

// Perform correlation in frequency domain.
span<const cf_t> channel_observed_time = idft.run();





// Calculate the absolute square of the correlation.
if (i_in == 0) {
srsvec::modulus_square(correlation, channel_observed_time);
} else {
// Accumulate the correlation.
srsvec::modulus_square_and_add(correlation, channel_observed_time, correlation);
}
}


// Estimate the time alignment from the correlation.
return estimate_ta_correlation(correlation, stride, scs, max_ta);
}

time_alignment_measurement time_alignment_estimator_dft_impl::estimate(span<const cf_t>   symbols,
                                                                       unsigned           stride,
                                                                       subcarrier_spacing scs,
                                                                       double             max_ta)
{
  modular_re_buffer_reader<cf_t, 1> symbols_view(1, symbols.size());
  symbols_view.set_slice(0, symbols);

  return estimate(symbols_view, stride, scs, max_ta);
}

dft_processor& time_alignment_estimator_dft_impl::get_idft(unsigned nof_required_re)
{
  // Ensure the number of required RE is smaller than the maximum DFT size.
  srsran_assert(nof_required_re <= max_nof_re,
                "The number of required RE (i.e., {}) is larger than the maximum allowed number of RE (i.e., {}).",
                nof_required_re,
                max_nof_re);

  // Leave some guards to avoid circular interference.
  nof_required_re = (nof_required_re * max_dft_size) / max_nof_re;

  // Get the next power of 2 DFT size.
  unsigned dft_size = pow2(log2_ceil(nof_required_re));
  dft_size          = std::max(min_dft_size, dft_size);

  // Select the DFT processor.
  return *dft_processors[dft_size];
}

time_alignment_measurement time_alignment_estimator_dft_impl::estimate_ta_correlation(span<const float>  correlation,
                                                                                      unsigned           stride,
                                                                                      subcarrier_spacing scs,
                                                                                      double             max_ta)
{
  // Calculate half cyclic prefix duration.
  phy_time_unit half_cyclic_prefix_duration =
      phy_time_unit::from_units_of_kappa(144) / pow2(to_numerology_value(scs) + 1);

  // Deduce sampling rate.
  double sampling_rate_Hz = correlation.size() * scs_to_khz(scs) * 1000 * stride;

  // Maximum number of samples limited by half cyclic prefix.
  unsigned max_ta_samples = std::floor(half_cyclic_prefix_duration.to_seconds() * sampling_rate_Hz);

  // Select the most limiting number of samples.
  if (std::isnormal(max_ta)) {
    max_ta_samples = std::min(max_ta_samples, static_cast<unsigned>(std::floor(max_ta * sampling_rate_Hz)));
  }

  // Find the maximum of the delayed taps.
  std::pair<unsigned, float> observed_max_delay = srsvec::max_element(correlation.first(max_ta_samples));

  // Find the maximum of the advanced taps.
  std::pair<unsigned, float> observed_max_advance = srsvec::max_element(correlation.last(max_ta_samples));

  // Determine the number of taps the signal is advanced or delayed (negative).
  int idx = -(max_ta_samples - observed_max_advance.first);
  if (observed_max_delay.second >= observed_max_advance.second) { 
    idx = observed_max_delay.first;
    //std::cout<<"observed_max_delay is greater than observed_max_advance"<<std::endl;
  }

  

  // Calculate the fractional sample.
  double fractional_sample_index = 0.0F;
  if (correlation.size() != max_dft_size) {
    // Select the number of samples for the fractional sample calculation depending on the maximum number of samples.
    unsigned nof_taps = (max_ta_samples > 2) ? 5 : 3;

    // Extract samples around the peak.
    static_vector<float, 5> peak_center_correlation(nof_taps);
    for (unsigned i = 0; i != nof_taps; ++i) {
      peak_center_correlation[i] = correlation[(idx + i + correlation.size() - nof_taps / 2) % correlation.size()];
    }

    // Calculate the fractional sample.
    fractional_sample_index = fractional_sample_delay(peak_center_correlation);
  }
  double t_align_seconds = (static_cast<double>(idx) + fractional_sample_index) / sampling_rate_Hz;
    // ToA_idx is the index of the maximum correlation.
  // std::cout<<"--------------------------------------------------------------\n \n"<<std::endl;
  // std::cout<<"observed_max_delay idx :"<<observed_max_delay.first<<std::endl;
  // std::cout<<"observed_max_advance idx :"<<observed_max_advance.first<<std::endl;
  // std::cout<<"observed_max_delay value :"<<observed_max_delay.second<<std::endl;
  // std::cout<<"observed_max_advance value :"<<observed_max_advance.second<<std::endl;
  // int ToA_idx = srsvec::max_element(correlation).first;
  // std::cout<<"ToA_idx :"<<ToA_idx<<std::endl;
  // double ToA = static_cast<double>(ToA_idx) / sampling_rate_Hz;
  // std::cout<<"Max ta samples :"<<max_ta_samples<<std::endl;
  // std::cout<<"Correlation size :"<<correlation.size()<<std::endl;
  // std::cout<<"Fractional Sample Delay :"<<fractional_sample_index<<std::endl;
  // std::cout<<"Peak Index :"<<idx<<std::endl;
  // std::cout<<"t_alignment with out F sample delay :"<<static_cast<double>(idx)/sampling_rate_Hz<<std::endl;
  // Final calculation of the time alignment in seconds.
 
  // std::cout<<"t_alignment with F sample delay :"<<t_align_seconds<<std::endl;
  // std::cout<<"ToA :"<<ToA<<std::endl;
  // std::cout<<"Distance : "<<ToA*3e8<<std::endl;
  // std::cout<<"--------------------------------------------------------------\n \n"<<std::endl;
  // Produce results.
  return time_alignment_measurement{
      .time_alignment = t_align_seconds,
      .resolution     = 1.0 / sampling_rate_Hz,
      .min            = -static_cast<double>(max_ta_samples) / sampling_rate_Hz,
      .max            = static_cast<double>(max_ta_samples) / sampling_rate_Hz,
  };
}
