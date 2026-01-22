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

#include "dmrs_schedule_remote_exporter.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "srsran/ran/bwp/bwp_configuration.h"
#include "srsran/ran/dmrs.h"
#include "srsran/ran/rnti.h"
#include "srsran/ran/subcarrier_spacing.h"
#include "srsran/scheduler/ue_identity_tracker.h"
#include "srsran/scheduler/result/resource_block_group.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/io/unique_fd.h"
#include <array>
#include <chrono>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <random>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <utility>
#include <unistd.h>

using namespace srsran;

namespace {

srslog::basic_logger& logger = srslog::fetch_basic_logger("DMRS_EXPORT");
constexpr const char* ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::chrono::seconds ws_timeout{2};

std::string to_string_dmrs_config_type(dmrs_config_type type)
{
  switch (type) {
    case dmrs_config_type::type1:
      return "type1";
    case dmrs_config_type::type2:
      return "type2";
    default:
      return "unknown";
  }
}

using dmrs_hopping_mode_t = decltype(std::declval<pusch_information>().dmrs_hopping_mode);

std::string to_string_dmrs_hopping_mode(dmrs_hopping_mode_t mode)
{
  switch (mode) {
    case dmrs_hopping_mode_t::group_hopping:
      return "group_hopping";
    case dmrs_hopping_mode_t::sequence_hopping:
      return "sequence_hopping";
    default:
      return "no_hopping";
  }
}

nlohmann::json build_rb_allocation_json(const pusch_information& pusch_cfg, const bwp_configuration& bwp_cfg)
{
  nlohmann::json rb_alloc;
  if (pusch_cfg.rbs.is_type1()) {
    const auto& vrbs = pusch_cfg.rbs.type1();
    rb_alloc["type"]   = "type1";
    rb_alloc["start"]  = vrbs.start();
    rb_alloc["length"] = vrbs.length();
  } else {
    rb_alloc["type"]      = "type0";
    rb_alloc["rbg_mask"]  = fmt::format("{:x}", pusch_cfg.rbs.type0());
    rb_alloc["rbg_count"] = pusch_cfg.rbs.type0().count();
    rb_alloc["rbg_size"]  = to_nominal_rbg_size_value(get_nominal_rbg_size(bwp_cfg.crbs.length(), true));
  }
  return rb_alloc;
}

nlohmann::json build_dmrs_json(const pusch_information& pusch_cfg)
{
  const dmrs_information& dmrs = pusch_cfg.dmrs;
  nlohmann::json          j;

  j["symbol_mask"]                = dmrs.dmrs_symb_pos.to_uint64();
  j["ports_mask"]                 = dmrs.dmrs_ports.to_uint64();
  j["config_type"]                = to_string_dmrs_config_type(dmrs.config_type);
  j["scrambling_id"]              = dmrs.dmrs_scrambling_id;
  j["scrambling_id_complement"]   = dmrs.dmrs_scrambling_id_complement;
  j["low_papr"]                   = dmrs.low_papr_dmrs;
  j["n_scid"]                     = dmrs.n_scid;
  j["num_cdm_groups_no_data"]     = dmrs.num_dmrs_cdm_grps_no_data;
  j["transform_precoding"]        = pusch_cfg.transform_precoding;
  j["dmrs_id"]                    = pusch_cfg.pusch_dmrs_id;
  j["hopping_mode"]               = to_string_dmrs_hopping_mode(pusch_cfg.dmrs_hopping_mode);

  return j;
}

nlohmann::json build_schedule_payload(slot_point                  slot,
                                      const nr_cell_global_id_t&  cell_id,
                                      const pusch_information&    pusch_cfg,
                                      const std::string&          imeisv,
                                      const std::optional<int>&   rar_ta)
{
  nlohmann::json payload;
  payload["cmd"] = "dmrs_schedule";

  nlohmann::json cell;
  cell["plmn"] = cell_id.plmn_id.to_string();
  cell["nci"]  = cell_id.nci.value();

  nlohmann::json schedule;
  schedule["imeisv"] = imeisv;
  if (rar_ta) {
    schedule["rar_ta"] = *rar_ta;
  }
  schedule["sfn"]  = slot.sfn();
  schedule["slot"] = slot.slot_index();
  schedule["rnti"] = fmt::format("{:#x}", to_value(pusch_cfg.rnti));
  schedule["positioning_requested"] = true;

  if (pusch_cfg.bwp_cfg != nullptr) {
    const bwp_configuration& bwp_cfg = *pusch_cfg.bwp_cfg;
    nlohmann::json           bwp;
    bwp["crb_start"] = bwp_cfg.crbs.start();
    bwp["nof_prbs"]  = bwp_cfg.crbs.length();
    bwp["scs_khz"]   = scs_to_khz(bwp_cfg.scs);
    schedule["bwp"]  = std::move(bwp);

    schedule["rb_allocation"] = build_rb_allocation_json(pusch_cfg, bwp_cfg);
  }

  nlohmann::json symbols;
  symbols["start"]  = pusch_cfg.symbols.start();
  symbols["length"] = pusch_cfg.symbols.length();
  schedule["symbols"] = std::move(symbols);

  schedule["dmrs"] = build_dmrs_json(pusch_cfg);

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
    logger.warning("DMRS export: failed to resolve {}:{} ({})", host, port, ::gai_strerror(rc));
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
    logger.warning("DMRS export: failed to connect to {}:{}", host, port);
  }
  return fd;
}

} // namespace

dmrs_schedule_remote_exporter::dmrs_schedule_remote_exporter(const scheduler_positioning_export_config& config)
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

dmrs_schedule_remote_exporter::~dmrs_schedule_remote_exporter()
{
  if (worker.joinable()) {
    stopping.store(true);
    cv.notify_all();
    worker.join();
  }
}

void dmrs_schedule_remote_exporter::handle_slot(slot_point slot,
                                                const nr_cell_global_id_t& cell_id,
                                                span<const ul_sched_info> puschs)
{
  if (endpoints.empty() || puschs.empty()) {
    return;
  }

  std::vector<std::string> payloads;
  payloads.reserve(puschs.size());

  for (const auto& grant : puschs) {
    const pusch_information& pusch_cfg = grant.pusch_cfg;
    if (!is_crnti(pusch_cfg.rnti)) {
      continue;
    }

    std::string imeisv;
    if (!ue_identity_tracker::get_imeisv_by_crnti(to_value(pusch_cfg.rnti), imeisv)) {
      continue;
    }
    std::optional<int> rar_ta;
    auto ta_opt = ue_identity_tracker::get_latest_ta_by_rnti(to_value(pusch_cfg.rnti));
    if (ta_opt) {
      rar_ta = *ta_opt;
    }

    nlohmann::json payload = build_schedule_payload(slot, cell_id, pusch_cfg, imeisv, rar_ta);
    payloads.push_back(payload.dump());

    logger.debug("DMRS sched: plmn={} nci={} rnti={} sfn={} slot={} imeisv={}",
                 cell_id.plmn_id.to_string(),
                 cell_id.nci.value(),
                 fmt::format("{:#x}", to_value(pusch_cfg.rnti)),
                 slot.sfn(),
                 slot.slot_index(),
                 imeisv);
  }

  if (payloads.empty()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& payload : payloads) {
      pending.push_back(std::move(payload));
    }
  }
  cv.notify_one();
}

void dmrs_schedule_remote_exporter::worker_loop()
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
        logger.warning("DMRS export: failed to deliver payload to {}:{}", peer.address, peer.port);
      }
    }
  }
}

bool dmrs_schedule_remote_exporter::send_payload_to_endpoint(const endpoint& peer, const std::string& payload)
{
  unique_fd fd = connect_to_endpoint(peer.address, peer.port);
  if (!fd.is_open()) {
    return false;
  }
  if (!set_socket_timeout(fd.value(), ws_timeout)) {
    logger.warning("DMRS export: failed to set receive timeout for {}:{}", peer.address, peer.port);
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
    logger.warning("DMRS export: websocket upgrade failed for {}:{} (response: {})",
                   peer.address,
                   peer.port,
                   response);
    return false;
  }

  const std::vector<uint8_t> frame = build_ws_frame(payload);
  return send_all(fd.value(), frame.data(), frame.size());
}
