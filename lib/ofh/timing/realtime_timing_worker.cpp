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

#include "realtime_timing_worker.h"
#include "srsran/instrumentation/traces/ofh_traces.h"
#include "srsran/ofh/timing/ofh_ota_symbol_boundary_notifier.h"
#include <future>
#include <thread>
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <optional>
using namespace srsran;
using namespace ofh;

/// Difference between Unix seconds to GPS seconds.
/// GPS epoch: 1980.1.6 00:00:00 (UTC); Unix time epoch: 1970:1.1 00:00:00 UTC
/// Last leap second added in 31st Dec 2016 by IERS.
/// 1970:1.1 - 1980.1.6: 3657 days
/// 3657*24*3600=315 964 800 seconds (Unix seconds value at 1980.1.6 00:00:00 (UTC))
/// There are 18 leap seconds inserted after 1980.1.6 00:00:00 (UTC), which means GPS is 18 seconds larger.
static constexpr uint64_t UNIX_TO_GPS_SECONDS_OFFSET = 315964800ULL - 18ULL;

/// Offset for converting from UTC to GPS time including Alpha and Beta parameters.
static std::chrono::nanoseconds gps_offset;

namespace {

/// A GPS clock implementation.
struct gps_clock {
  using duration   = std::chrono::nanoseconds;
  using rep        = duration::rep;
  using period     = duration::period;
  using time_point = std::chrono::time_point<gps_clock>;
  // static constexpr bool is_steady = false;

  static time_point now()
  {
    ::timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);

    time_point now(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec));

    return now - gps_offset;
  }
};

} // namespace




void send_slot_info(const std::string& ip, int port, int info)
{
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) return;

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  inet_pton(AF_INET, ip.c_str(), &dest.sin_addr);

  sendto(sockfd, &info, sizeof(info), 0, (sockaddr*)&dest, sizeof(dest));
  close(sockfd);
}

struct start_message_payload {
  uint32_t magic;
  uint64_t start_time_ns;
};

std::optional<uint64_t> try_receive_start_message(int sockfd)
{
  start_message_payload payload{};
  sockaddr_in            sender_addr{};
  socklen_t              sender_len = sizeof(sender_addr);
  ssize_t                bytes =
      recvfrom(sockfd, &payload, sizeof(payload), 0, reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);

  if (bytes == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("recvfrom");
    }
    return std::nullopt;
  }

  if (bytes != static_cast<ssize_t>(sizeof(payload))) {
    std::cout << "Received unexpected control payload of size " << bytes << " bytes" << std::endl;
    return std::nullopt;
  }

  uint32_t magic = ntohl(payload.magic);
  if (magic != 0xABCD1234) {
    std::cout << "Received control payload with invalid magic " << std::hex << magic << std::dec << std::endl;
    return std::nullopt;
  }

  uint64_t start_time_ns = be64toh(payload.start_time_ns);
  std::cout << "Received synchronized start time: " << start_time_ns << " ns" << std::endl;
  return start_time_ns;
}

int create_nonblocking_udp_receiver(int port)
{
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) return -1;

  int flags = fcntl(sockfd, F_GETFL, 0);
  fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(sockfd);
    return -1;
  }

  return sockfd;
}

/// Calculates the fractional part inside a second from the given time point.
static std::chrono::nanoseconds calculate_ns_fraction_from(gps_clock::time_point tp)
{
  auto tp_sec = std::chrono::time_point_cast<std::chrono::seconds>(tp);
  return tp - tp_sec;
}

realtime_timing_worker::realtime_timing_worker(srslog::basic_logger&      logger_,
                                               task_executor&             executor_,
                                               const realtime_worker_cfg& cfg) :
  logger(logger_),
  executor(executor_),
  scs(cfg.scs),
  nof_symbols_per_slot(get_nsymb_per_slot(cfg.cp)),
  nof_symbols_per_sec(nof_symbols_per_slot * get_nof_slots_per_subframe(scs) * NOF_SUBFRAMES_PER_FRAME * 100),
  symbol_duration(1e9 / nof_symbols_per_sec),
  sleep_time(std::chrono::duration_cast<std::chrono::nanoseconds>(symbol_duration) / 15)
{
  // The GPS time epoch starts on 1980.1.6 so make sure that the system time is set after this date.
  // For simplicity reasons, only allow dates after 1981.
  ::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  ::tm     utc_time;
  ::gmtime_r(&tt, &utc_time);
  int current_year = utc_time.tm_year + 1900;
  report_error_if_not(current_year >= 1981,
                      "The Open FrontHaul standard uses GPS time for synchronization. Make sure that system time is "
                      "set after the year 1981 since the GPS time epoch starts on 1980.1.6");

  gps_offset = std::chrono::nanoseconds(static_cast<unsigned>(std::round(cfg.gps_Alpha / 1.2288))) +
               std::chrono::milliseconds(cfg.gps_Beta * 10) + std::chrono::seconds(UNIX_TO_GPS_SECONDS_OFFSET);
}

/// Returns the symbol index inside a second.
static unsigned get_symbol_index(std::chrono::nanoseconds                 fractional_ns,
                                 std::chrono::duration<double, std::nano> symbol_duration)
{
  // Perform operation with enough precision to avoid rounding errors when the amount of fractional nanoseconds is big.
  return fractional_ns / symbol_duration;
}

void realtime_timing_worker::start()
{
  logger.info("Starting the realtime timing worker");

  std::promise<void> p;
  std::future<void>  fut = p.get_future();

  if (!executor.defer([this, &p]() {
        // Signal start() caller thread that the operation is complete.
        p.set_value();

        auto ns_fraction    = calculate_ns_fraction_from(gps_clock::now());
        previous_symb_index = get_symbol_index(ns_fraction, symbol_duration);
        timing_loop();
      })) {
    report_fatal_error("Unable to start the realtime timing worker");
  }

  // Block waiting for timing executor to start.
  fut.wait();

  logger.info("Started the realtime timing worker");
}

void realtime_timing_worker::stop()
{
  logger.info("Requesting stop of the realtime timing worker");
  status.store(worker_status::stop_requested, std::memory_order_relaxed);

  // Wait for the timing thread to stop - this line also introduces a happens-before relationship with the clear on
  // ota_notifier.
  while (status.load(std::memory_order_acquire) != worker_status::stopped) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Clear the subscribed notifiers.
  ota_notifiers.clear();

  logger.info("Stopped the realtime timing worker");
}

void realtime_timing_worker::timing_loop()
{
  while (SRSRAN_LIKELY(status.load(std::memory_order_relaxed) == worker_status::running)) {
    poll();
  }

  // Acquire/Release semantics - ota_notifiers is cleared and destructed by a different thread.
  status.store(worker_status::stopped, std::memory_order_release);
}

/// Returns the difference between cur and prev taking into account a potential wrap around of the values.
static unsigned circular_distance(unsigned cur, unsigned prev, unsigned size)
{
  return (cur >= prev) ? (cur - prev) : (size + cur - prev);
}

/// Calculates the current slot point given the GPS time.
static slot_point
calculate_slot_point(subcarrier_spacing scs, uint64_t gps_seconds, uint32_t fractional_us, uint32_t slot_duration_us)
{
  unsigned subframe_index      = (fractional_us / (SUBFRAME_DURATION_MSEC * 1000)) % NOF_SUBFRAMES_PER_FRAME;
  unsigned slot_subframe_index = (fractional_us / slot_duration_us) % get_nof_slots_per_subframe(scs);

  static constexpr unsigned NUM_FRAMES_PER_SEC = 100;
  static constexpr unsigned FRAME_DURATION_US  = SUBFRAME_DURATION_MSEC * NOF_SUBFRAMES_PER_FRAME * 1000;
  unsigned                  sfn = (gps_seconds * NUM_FRAMES_PER_SEC + (fractional_us / FRAME_DURATION_US)) % NOF_SFNS;

  return {to_numerology_value(scs), sfn, subframe_index, slot_subframe_index};
}

void realtime_timing_worker::poll()
{

  static thread_local int          udp_recv_fd          = create_nonblocking_udp_receiver(9000);
  static thread_local std::string  controller_ip        = "10.0.0.1";
  static thread_local int          controller_port      = 9001;
  static thread_local bool         ready_sent           = false;
  static thread_local bool         start_time_received  = false;
  static thread_local uint64_t     start_time_unix_ns   = 0;
  static thread_local bool         slot_origin_set      = false;
  static thread_local uint32_t     slot_origin_slot_cnt = 0;
  static thread_local bool         log_slot_sequence    = false;
  static thread_local unsigned     remaining_slots      = 0;
  static thread_local int          log_counter          = 0;

  if (!ready_sent) {
    send_slot_info(controller_ip, controller_port, 1);
    ready_sent = true;
    auto now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(gps_clock::now().time_since_epoch()).count();
    logger.info("[GNB] Sent ready signal. t_now={} ns", now_ns);
    std::cout << "[GNB] Sent ready signal. t_now=" << now_ns << " ns" << std::endl;
  }

  if (!start_time_received) {
    auto start_time = try_receive_start_message(udp_recv_fd);
    if (!start_time.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return;
    }
    start_time_unix_ns  = start_time.value();
    start_time_received = true;
    logger.info("[GNB] Received synchronous start request at unix={} ns", start_time_unix_ns);
    std::cout << "[GNB] Received synchronous start request at unix=" << start_time_unix_ns << " ns" << std::endl;
  }

  auto      now_rt_tp = std::chrono::system_clock::now();
  uint64_t  now_rt_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now_rt_tp.time_since_epoch()).count());
  if (start_time_received && now_rt_ns + 1000ULL < start_time_unix_ns) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    return;
  }
  if (start_time_received && now_rt_ns < start_time_unix_ns) {
    return;
  }

  auto now         = gps_clock::now();
  auto ns_fraction = calculate_ns_fraction_from(now);

  unsigned current_symbol_index = get_symbol_index(ns_fraction, symbol_duration);
  unsigned delta                = circular_distance(current_symbol_index, previous_symb_index, nof_symbols_per_sec);
  previous_symb_index           = current_symbol_index;

  // Are we still in the same symbol as before?
  if (delta == 0) {
    std::this_thread::sleep_for(sleep_time);
    return;
  }

  // Check if we have missed more than one symbol.
  if (SRSRAN_UNLIKELY(delta > 1)) {
    logger.info("Real-time timing worker woke up late, skipped '{}' symbols", delta);
  }
  if (SRSRAN_UNLIKELY(delta >= nof_symbols_per_slot)) {
    logger.warning("Real-time timing worker woke up late, sleep time has been '{}us', or equivalently, '{}' symbols",
                   std::chrono::duration_cast<std::chrono::microseconds>(delta * symbol_duration).count(),
                   delta);
  }

  slot_symbol_point symbol_point(
      calculate_slot_point(scs,
                           std::chrono::time_point_cast<std::chrono::seconds>(now).time_since_epoch().count(),
                           std::chrono::duration_cast<std::chrono::microseconds>(ns_fraction).count(),
                           1000 / get_nof_slots_per_subframe(scs)),
      current_symbol_index % nof_symbols_per_slot,
      nof_symbols_per_slot);

  if (start_time_received && !slot_origin_set) {
    if (symbol_point.get_symbol_index() != 0) {
      return;
    }
    slot_point first_slot = symbol_point.get_slot();
    slot_origin_slot_cnt  = first_slot.to_uint();
    slot_origin_set       = true;
    log_slot_sequence     = true;
    remaining_slots       = 10;
    log_counter           = 0;
    logger.info("[GNB] Aligning slot counter to SFN={} slot={} at first symbol boundary",
                first_slot.sfn(),
                first_slot.slot_index());
    std::cout << "[GNB] Aligning slot counter to SFN=" << first_slot.sfn() << " slot=" << first_slot.slot_index()
              << std::endl;
  }

  if (!slot_origin_set) {
    return;
  }

  const uint32_t total_slots = slot_point(scs, 0).nof_slots_per_system_frame();
  slot_point      raw_slot   = symbol_point.get_slot();
  uint32_t normalized_slot_count = (raw_slot.to_uint() + total_slots - slot_origin_slot_cnt) % total_slots;
  slot_point normalized_slot(scs, normalized_slot_count);
  symbol_point = slot_symbol_point(normalized_slot, symbol_point.get_symbol_index(), nof_symbols_per_slot);

  if (SRSRAN_UNLIKELY(log_slot_sequence && remaining_slots > 0 && symbol_point.get_symbol_index() == 0)) {
    slot_point slot = symbol_point.get_slot();
    auto slot_time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    logger.info("[GNB] Slot sync sample {}: SFN={} slot={} time={} ns",
                ++log_counter,
                slot.sfn(),
                slot.slot_index(),
                slot_time_ns);
    std::cout << "[GNB] Slot sync sample " << log_counter << ": SFN=" << slot.sfn()
              << " slot=" << slot.slot_index() << " time=" << slot_time_ns << " ns" << std::endl;
    --remaining_slots;
    if (remaining_slots == 0) {
      log_slot_sequence = false;
    }
  }

  for (unsigned i = 0; i != delta; ++i) {
    unsigned skipped_symbol_id = delta - 1 - i;
    // Notify pending symbols from oldest to newest.
    notify_slot_symbol_point({symbol_point - skipped_symbol_id, std::chrono::system_clock::now()});
  }
}

void realtime_timing_worker::notify_slot_symbol_point(const slot_symbol_point_context& slot_context)
{
  ofh_tracer << instant_trace_event("ofh_timing_notify_symbol", instant_trace_event::cpu_scope::global);

  for (auto* notifier : ota_notifiers) {
    notifier->on_new_symbol(slot_context);
  }
}

void realtime_timing_worker::subscribe(span<ota_symbol_boundary_notifier*> notifiers)
{
  // The defer() call in start() synchronizes the contents of ota_notifiers with the worker thread.
  ota_notifiers.assign(notifiers.begin(), notifiers.end());
}
