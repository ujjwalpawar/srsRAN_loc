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

#include "srs_schedule_remote_exporter.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "srsran/srslog/srslog.h"
#include "srsran/support/io/unique_fd.h"
#include <array>
#include <chrono>
#include <netdb.h>
#include <netinet/in.h>
#include <random>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace srsran;

namespace {

srslog::basic_logger& logger = srslog::fetch_basic_logger("SRS_EXPORT");
constexpr const char* ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::chrono::seconds ws_timeout{2};

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

std::string to_string_resource_type(srs_resource_type value)
{
  switch (value) {
    case srs_resource_type::aperiodic:
      return "aperiodic";
    case srs_resource_type::semi_persistent:
      return "semi_persistent";
    case srs_resource_type::periodic:
      return "periodic";
    default:
      return "invalid";
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
  j["resource_type"]             = to_string_resource_type(res.res_type);

  if (res.periodicity_and_offset) {
    nlohmann::json periodicity;
    periodicity["t_srs"]  = static_cast<unsigned>(res.periodicity_and_offset->period);
    periodicity["offset"] = res.periodicity_and_offset->offset;
    j["periodicity"]      = std::move(periodicity);
  }

  return j;
}

std::string build_resource_key(const srs_config::srs_resource&      res,
                               const nr_cell_global_id_t&           cell_id,
                               rnti_t                               rnti,
                               bool                                 positioning_requested,
                               const std::optional<std::string>&    imeisv)
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

std::vector<srs_config::srs_resource> resolve_all_resources(const srs_schedule_descriptor& descriptor)
{
  std::vector<srs_config::srs_resource> all_resources = descriptor.all_resources;
  if (descriptor.positioning_requested && all_resources.empty()) {
    all_resources.push_back(descriptor.resource);
  }
  return all_resources;
}

nlohmann::json build_schedule_payload(const srs_schedule_descriptor& descriptor)
{
  const auto all_resources = resolve_all_resources(descriptor);

  nlohmann::json payload;
  payload["cmd"] = "positioning_request";

  nlohmann::json cell;
  cell["plmn"] = descriptor.cell_id.plmn_id.to_string();
  cell["nci"]  = descriptor.cell_id.nci.value();

  nlohmann::json schedule;
  if (descriptor.imeisv) {
    schedule["imeisv"] = *descriptor.imeisv;
  }
  if (descriptor.rar_ta) {
    schedule["rar_ta"] = *descriptor.rar_ta;
  }
  schedule["sfn"]         = descriptor.slot.sfn();
  schedule["slot"]        = descriptor.slot.slot_index();
  schedule["schedule_id"] = descriptor.schedule_id;
  schedule["rnti"]        = fmt::format("{:#x}", to_value(descriptor.rnti));
  schedule["resource"]    = build_resource_json(descriptor.resource);
  if (descriptor.positioning_requested) {
    schedule["positioning_requested"] = true;
  }
  if (!all_resources.empty()) {
    nlohmann::json all_res = nlohmann::json::array();
    for (const auto& res : all_resources) {
      all_res.push_back(build_resource_json(res));
    }
    schedule["all_resources"] = std::move(all_res);
  }

  cell["schedule"] = std::move(schedule);
  payload["cells"] = nlohmann::json::array({cell});

  return payload;
}

nlohmann::json build_stop_payload(const srs_schedule_stop_descriptor& descriptor)
{
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
  if (descriptor.rar_ta) {
    schedule["rar_ta"] = *descriptor.rar_ta;
  }
  if (descriptor.positioning_requested) {
    schedule["positioning_requested"] = true;
  }

  cell["schedule"] = std::move(schedule);
  payload["cells"] = nlohmann::json::array({cell});

  return payload;
}

std::string normalize_path(const std::string& path)
{
  if (path.empty()) {
    return "/";
  }
  if (path.front() == '/') {
    return path;
  }
  return "/" + path;
}

bool send_all(int fd, const uint8_t* data, size_t len)
{
  while (len > 0) {
    const ssize_t sent = ::send(fd, data, len, 0);
    if (sent <= 0) {
      return false;
    }
    data += sent;
    len -= sent;
  }
  return true;
}

bool send_all(int fd, const std::string& data)
{
  return send_all(fd, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

bool set_socket_timeout(int fd, std::chrono::seconds timeout)
{
  struct timeval tv;
  tv.tv_sec  = timeout.count();
  tv.tv_usec = 0;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool read_http_response(int fd, std::string& response)
{
  std::array<char, 1024> buf{};
  while (response.find("\r\n\r\n") == std::string::npos) {
    const ssize_t received = ::recv(fd, buf.data(), buf.size(), 0);
    if (received <= 0) {
      return false;
    }
    response.append(buf.data(), static_cast<size_t>(received));
    if (response.size() > 8192) {
      break;
    }
  }
  return true;
}

bool is_switching_protocols(const std::string& response)
{
  if (response.rfind("HTTP/1.1 101", 0) == 0) {
    return true;
  }
  return response.rfind("HTTP/1.0 101", 0) == 0;
}

std::array<uint8_t, 4> make_mask()
{
  std::array<uint8_t, 4> mask{};
  std::random_device     rd;
  for (auto& byte : mask) {
    byte = static_cast<uint8_t>(rd());
  }
  return mask;
}

std::vector<uint8_t> build_ws_frame(std::string_view payload)
{
  const size_t payload_len = payload.size();
  std::vector<uint8_t> frame;
  frame.reserve(2 + 8 + 4 + payload_len);
  frame.push_back(0x81);

  if (payload_len <= 125) {
    frame.push_back(static_cast<uint8_t>(0x80 | payload_len));
  } else if (payload_len <= 0xffff) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(payload_len & 0xff));
  } else {
    frame.push_back(0x80 | 127);
    const uint64_t len64 = payload_len;
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<uint8_t>((len64 >> shift) & 0xff));
    }
  }

  const auto mask = make_mask();
  frame.insert(frame.end(), mask.begin(), mask.end());

  for (size_t i = 0; i < payload_len; ++i) {
    frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % mask.size()]);
  }

  return frame;
}

unique_fd connect_to_endpoint(const std::string& host, unsigned port)
{
  struct addrinfo hints {
  };
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family   = AF_UNSPEC;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* result = nullptr;
  std::string      port_str = fmt::format("{}", port);
  const int        rc       = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
  if (rc != 0) {
    logger.warning("SRS export: failed to resolve {}:{} ({})", host, port, ::gai_strerror(rc));
    return {};
  }

  unique_fd fd;
  for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
    const int sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock < 0) {
      continue;
    }
    if (::connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
      fd = unique_fd(sock);
      break;
    }
    ::close(sock);
  }
  ::freeaddrinfo(result);

  if (!fd.is_open()) {
    logger.warning("SRS export: failed to connect to {}:{}", host, port);
  }
  return fd;
}

} // namespace

srs_schedule_remote_exporter::srs_schedule_remote_exporter(const scheduler_positioning_export_config& config)
{
  endpoints.reserve(config.neighbours.size());
  for (const auto& neighbour : config.neighbours) {
    if (neighbour.address.empty()) {
      continue;
    }
    endpoint ep;
    ep.address = neighbour.address;
    ep.port    = neighbour.port;
    ep.path    = neighbour.path;
    if (ep.path.empty()) {
      ep.path = "/";
    }
    endpoints.push_back(std::move(ep));
  }

  if (!endpoints.empty()) {
    worker = std::thread([this]() { worker_loop(); });
  }
}

srs_schedule_remote_exporter::~srs_schedule_remote_exporter()
{
  if (worker.joinable()) {
    stopping.store(true);
    cv.notify_all();
    worker.join();
  }
}

void srs_schedule_remote_exporter::handle_schedule(const srs_schedule_descriptor& descriptor)
{
  if (endpoints.empty()) {
    return;
  }

  const std::string key = build_resource_key(
      descriptor.resource, descriptor.cell_id, descriptor.rnti, descriptor.positioning_requested, descriptor.imeisv);
  const nlohmann::json payload = build_schedule_payload(descriptor);

  {
    std::lock_guard<std::mutex> lock(mtx);
    if (!active_keys.insert(key).second) {
      return;
    }
    pending.push_back(payload.dump());
  }
  cv.notify_one();

  fmt::print(
      "[SRS_EXPORT] START plmn={} nci={} rnti={} sfn={} slot={} imeisv={} positioning={} ue_res_id={}\n",
      descriptor.cell_id.plmn_id.to_string(),
      descriptor.cell_id.nci.value(),
      fmt::format("{:#x}", to_value(descriptor.rnti)),
      descriptor.slot.sfn(),
      descriptor.slot.slot_index(),
      descriptor.imeisv.value_or("n/a"),
      descriptor.positioning_requested,
      fmt::underlying(descriptor.resource.id.ue_res_id));
}

void srs_schedule_remote_exporter::handle_stop(const srs_schedule_stop_descriptor& descriptor)
{
  if (endpoints.empty()) {
    return;
  }

  const std::string key = build_resource_key(
      descriptor.resource, descriptor.cell_id, descriptor.rnti, descriptor.positioning_requested, descriptor.imeisv);
  const nlohmann::json payload = build_stop_payload(descriptor);

  {
    std::lock_guard<std::mutex> lock(mtx);
    active_keys.erase(key);
    pending.push_back(payload.dump());
  }
  cv.notify_one();

  fmt::print("[SRS_EXPORT] STOP plmn={} nci={} rnti={} ue_res_id={} imeisv={} positioning={}\n",
             descriptor.cell_id.plmn_id.to_string(),
             descriptor.cell_id.nci.value(),
             fmt::format("{:#x}", to_value(descriptor.rnti)),
             fmt::underlying(descriptor.resource.id.ue_res_id),
             descriptor.imeisv.value_or("n/a"),
             descriptor.positioning_requested);
}

void srs_schedule_remote_exporter::worker_loop()
{
  while (true) {
    std::string payload;
    {
      std::unique_lock<std::mutex> lock(mtx);
      cv.wait(lock, [this]() { return stopping.load() || !pending.empty(); });
      if (stopping.load() && pending.empty()) {
        return;
      }
      payload = std::move(pending.front());
      pending.pop_front();
    }

    for (const auto& peer : endpoints) {
      if (!send_payload_to_endpoint(peer, payload)) {
        logger.warning("SRS export: failed to deliver payload to {}:{}", peer.address, peer.port);
      }
    }
  }
}

bool srs_schedule_remote_exporter::send_payload_to_endpoint(const endpoint& peer, const std::string& payload)
{
  unique_fd fd = connect_to_endpoint(peer.address, peer.port);
  if (!fd.is_open()) {
    return false;
  }
  if (!set_socket_timeout(fd.value(), ws_timeout)) {
    logger.warning("SRS export: failed to set receive timeout for {}:{}", peer.address, peer.port);
  }

  const std::string path = normalize_path(peer.path);
  const std::string request = fmt::format(
      "GET {} HTTP/1.1\r\nHost: {}:{}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
      "{}\r\nSec-WebSocket-Version: 13\r\n\r\n",
      path,
      peer.address,
      peer.port,
      ws_key);

  if (!send_all(fd.value(), request)) {
    return false;
  }

  std::string response;
  if (!read_http_response(fd.value(), response)) {
    return false;
  }
  if (!is_switching_protocols(response)) {
    logger.warning("SRS export: websocket upgrade failed for {}:{} (response: {})",
                   peer.address,
                   peer.port,
                   response);
    return false;
  }

  const std::vector<uint8_t> frame = build_ws_frame(payload);
  return send_all(fd.value(), frame.data(), frame.size());
}
