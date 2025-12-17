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

#include "srsran/ran/rnti.h"
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace srsran {

/// \brief UE Identity Tracker - Tracks persistent UE identity across RACH procedures
/// This tracker maps IMEISV (persistent UE identifier) to RA-RNTI and C-RNTI allocations,
/// maintaining a history of TA adjustments over time for each physical UE.
class ue_identity_tracker {
public:
  /// Timing Advance event type
  enum class ta_event_type {
    RAR_TA,    ///< Initial TA from Random Access Response
    CE_ACK_TA  ///< TA adjustment from MAC CE ACK
  };

  /// Timing Advance history entry
  struct ta_history_entry {
    long long unsigned int timestamp_ms; ///< Timestamp in milliseconds
    int                    ta_value;     ///< TA value (absolute for RAR, delta for CE)
    ta_event_type          event_type;   ///< Type of TA event
    uint16_t               rnti;         ///< Associated RNTI (RA-RNTI or C-RNTI)
  };

  /// UE record containing RNTI history and TA timeline
  struct ue_record {
    std::vector<uint16_t>         ra_rntis;   ///< History of RA-RNTIs (one per RACH)
    std::vector<uint16_t>         c_rntis;    ///< History of C-RNTIs/TC-RNTIs
    std::vector<ta_history_entry> ta_history; ///< Timeline of TA events
  };

  /// Register IMEISV from NGAP Initial Context Setup Request (no C-RNTI available yet)
  /// \param imeisv_str IMEISV string (derived from fixed_bitstring<64>)
  /// \param ue_index UE index from NGAP layer (IGNORED - unreliable cross-layer)
  static void register_imeisv_from_ngap(const std::string& imeisv_str, uint64_t ue_index)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto&                       records = get_records();

    if (records.find(imeisv_str) == records.end()) {
      // New UE
      records[imeisv_str] = ue_record{};
      std::cout << "[UE_ID_TRACKER] NEW_UE: IMEISV=" << imeisv_str << " ue_index=" << ue_index << std::endl;
    }

    // Check ALL pending RACH records to see if any match this UE
    // First, clean up stale RACHs (older than 1 second without IMEISV - likely failed attaches)
    auto& pending = get_pending_rach();
    const long long unsigned int current_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const long long unsigned int stale_threshold_ns = 1000000000ULL; // 1 second in nanoseconds
    
    std::vector<uint16_t> stale_tc_rntis;
    std::vector<uint16_t> recent_tc_rntis;
    
    for (auto& kv : pending) {
      if (kv.second.imeisv_str.empty()) {
        long long unsigned int age_ns = current_time - kv.second.timestamp_ms;
        if (age_ns > stale_threshold_ns) {
          // Old RACH without IMEISV - probably failed attach, discard it
          stale_tc_rntis.push_back(kv.first);
        } else {
          // Recent RACH - could be this UE or another concurrent UE
          recent_tc_rntis.push_back(kv.first);
        }
      }
    }
    
    // Discard stale pending RACHs
    for (uint16_t stale_rnti : stale_tc_rntis) {
      long long unsigned int age_ms = (current_time - pending[stale_rnti].timestamp_ms) / 1000000ULL;
      std::cout << "[UE_ID_TRACKER] DISCARD_STALE_RACH: TC_RNTI=" << std::hex << stale_rnti << std::dec 
                << " age_ms=" << age_ms << " (older than 1s, failed attach)" << std::endl;
      pending.erase(stale_rnti);
    }
    
    // If exactly one recent RACH without IMEISV, link it to this UE
    // If multiple, wait (could be concurrent UEs) - will resolve when scheduler creates C-RNTI
    if (recent_tc_rntis.size() == 1) {
      uint16_t matched_tc_rnti = recent_tc_rntis[0];
      auto& pend_info = pending[matched_tc_rnti];
      pend_info.imeisv_str = imeisv_str; // Store IMEISV in pending RACH
      
      // IMPORTANT: Also create reverse C-RNTI → IMEISV mapping immediately!
      // Scheduler may have already called link_crnti_to_imeisv() before NGAP arrived
      get_crnti_to_imeisv()[matched_tc_rnti] = imeisv_str;
      
      // Add C-RNTI to UE record
      auto& rec = records[imeisv_str];
      if (std::find(rec.c_rntis.begin(), rec.c_rntis.end(), matched_tc_rnti) == rec.c_rntis.end()) {
        rec.c_rntis.push_back(matched_tc_rnti);
      }
      
      // Add RA-RNTI and TA event
      if (std::find(rec.ra_rntis.begin(), rec.ra_rntis.end(), pend_info.ra_rnti) == rec.ra_rntis.end()) {
        rec.ra_rntis.push_back(pend_info.ra_rnti);
      }
      rec.ta_history.push_back({pend_info.timestamp_ms, pend_info.ta_value, ta_event_type::RAR_TA, matched_tc_rnti});
      
      std::cout << "[UE_ID_TRACKER] NGAP_RESOLVED_PENDING_RACH: IMEISV=" << imeisv_str 
                << " C_RNTI=" << std::hex << matched_tc_rnti << std::dec 
                << " RA_RNTI=" << std::hex << pend_info.ra_rnti << std::dec
                << " RAR_TA=" << pend_info.ta_value << std::endl;
      
      // Remove from pending since it's now resolved
      pending.erase(matched_tc_rnti);
    } else {
      std::cout << "[UE_ID_TRACKER] NGAP_MULTIPLE_PENDING: IMEISV=" << imeisv_str 
                << " ue_index=" << ue_index << " (found " << recent_tc_rntis.size() 
                << " recent RACHs - may be concurrent UEs, will use ue_index fallback)" << std::endl;
    }
    
    // Store ue_index mapping for backward compatibility (but it's not reliable!)
    get_ue_index_to_imeisv()[ue_index] = imeisv_str;
    
    // Check old pending_crnti_by_ue_index in case scheduler already created C-RNTI
    auto& pending_crnti_map = get_pending_crnti_by_ue_index();
    if (pending_crnti_map.find(ue_index) != pending_crnti_map.end()) {
      uint16_t c_rnti = pending_crnti_map[ue_index];
      auto& rec = records[imeisv_str];
      
      // Add C-RNTI if not already present
      if (std::find(rec.c_rntis.begin(), rec.c_rntis.end(), c_rnti) == rec.c_rntis.end()) {
        rec.c_rntis.push_back(c_rnti);
      }
      
      // Reverse mapping: C-RNTI -> IMEISV
      get_crnti_to_imeisv()[c_rnti] = imeisv_str;
      
      // Resolve any pending RACH records for this C-RNTI
      if (pending.find(c_rnti) != pending.end()) {
        auto& pend_info = pending[c_rnti];
        // Add RA-RNTI
        if (std::find(rec.ra_rntis.begin(), rec.ra_rntis.end(), pend_info.ra_rnti) == rec.ra_rntis.end()) {
          rec.ra_rntis.push_back(pend_info.ra_rnti);
        }
        // Add TA event
        rec.ta_history.push_back({pend_info.timestamp_ms, pend_info.ta_value, ta_event_type::RAR_TA, c_rnti});
        std::cout << "[UE_ID_TRACKER] RESOLVE_PENDING: IMEISV=" << imeisv_str << " RA_RNTI=" << std::hex
                  << pend_info.ra_rnti << " TC_RNTI=" << c_rnti << std::dec << " RAR_TA=" << pend_info.ta_value
                  << std::endl;
        pending.erase(c_rnti);
      }
      
      pending_crnti_map.erase(ue_index);
    }
  }

  /// Link C-RNTI to IMEISV (called when C-RNTI is assigned)
  /// \param c_rnti C-RNTI assigned to this UE
  /// \param ue_index UE index (IGNORED - unreliable cross-layer)
  static void link_crnti_to_imeisv(uint16_t c_rnti, uint64_t ue_index)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    
    // CRITICAL: When scheduler assigns C-RNTI, we DON'T know the IMEISV yet!
    // We can only resolve the mapping when NGAP provides IMEISV.
    // DO NOT use old stale mappings from disconnected UEs - C-RNTIs get recycled!
    
    // Check if there's a pending RACH for this C-RNTI with IMEISV already stored from NGAP
    auto& pending = get_pending_rach();
    if (pending.find(c_rnti) != pending.end() && !pending[c_rnti].imeisv_str.empty()) {
      // IMEISV was already stored in pending RACH from NGAP!
      const std::string& imeisv_str = pending[c_rnti].imeisv_str;
      auto& records = get_records();
      auto& rec = records[imeisv_str];
      auto& pend_info = pending[c_rnti];
      
      // Add C-RNTI (could be handover with same IMEISV but new C-RNTI)
      if (std::find(rec.c_rntis.begin(), rec.c_rntis.end(), c_rnti) == rec.c_rntis.end()) {
        rec.c_rntis.push_back(c_rnti);
      }
      
      // Add RA-RNTI
      if (std::find(rec.ra_rntis.begin(), rec.ra_rntis.end(), pend_info.ra_rnti) == rec.ra_rntis.end()) {
        rec.ra_rntis.push_back(pend_info.ra_rnti);
      }
      
      // Add TA event
      rec.ta_history.push_back({pend_info.timestamp_ms, pend_info.ta_value, ta_event_type::RAR_TA, c_rnti});
      
      // Reverse mapping: C-RNTI -> IMEISV
      get_crnti_to_imeisv()[c_rnti] = imeisv_str;
      
      // Update ue_index mapping (may overwrite stale mapping)
      get_ue_index_to_imeisv()[ue_index] = imeisv_str;
      
      std::cout << "[UE_ID_TRACKER] LINK_CRNTI_FROM_PENDING: IMEISV=" << imeisv_str 
                << " C_RNTI=" << std::hex << c_rnti << std::dec
                << " RA_RNTI=" << std::hex << pend_info.ra_rnti << std::dec
                << " RAR_TA=" << pend_info.ta_value << " ue_index=" << ue_index << std::endl;
      
      pending.erase(c_rnti);
      return;
    }
    
    // IMPORTANT: DO NOT use get_crnti_to_imeisv() here!
    // Old mappings are from disconnected UEs - C-RNTIs get recycled.
    // We must wait for NGAP to provide fresh IMEISV for this new attach.
    
    // Check ue_index mapping as fallback (only if IMEISV arrived before C-RNTI assignment)
    auto& ue_idx_map = get_ue_index_to_imeisv();
    if (ue_idx_map.find(ue_index) != ue_idx_map.end()) {
      const std::string& imeisv_str = ue_idx_map[ue_index];
      auto& records = get_records();
      auto& rec = records[imeisv_str];

      // Add C-RNTI if not already present (could be handover)
      if (std::find(rec.c_rntis.begin(), rec.c_rntis.end(), c_rnti) == rec.c_rntis.end()) {
        rec.c_rntis.push_back(c_rnti);
      }

      // Reverse mapping: C-RNTI -> IMEISV
      get_crnti_to_imeisv()[c_rnti] = imeisv_str;

      // Resolve any pending RACH records
      if (pending.find(c_rnti) != pending.end()) {
        auto& pend_info = pending[c_rnti];
        if (std::find(rec.ra_rntis.begin(), rec.ra_rntis.end(), pend_info.ra_rnti) == rec.ra_rntis.end()) {
          rec.ra_rntis.push_back(pend_info.ra_rnti);
        }
        rec.ta_history.push_back({pend_info.timestamp_ms, pend_info.ta_value, ta_event_type::RAR_TA, c_rnti});
        std::cout << "[UE_ID_TRACKER] LINK_CRNTI: IMEISV=" << imeisv_str << " C_RNTI=" << std::hex << c_rnti
                  << std::dec << " RA_RNTI=" << std::hex << pend_info.ra_rnti << std::dec 
                  << " RAR_TA=" << pend_info.ta_value << " ue_index=" << ue_index << std::endl;
        pending.erase(c_rnti);
      } else {
        std::cout << "[UE_ID_TRACKER] LINK_CRNTI: IMEISV=" << imeisv_str << " C_RNTI=" << std::hex << c_rnti
                  << std::dec << " ue_index=" << ue_index << " (no pending RACH)" << std::endl;
      }
      return;
    }
    
    // No IMEISV available yet - this is normal, NGAP will arrive later
    // Store pending so NGAP can resolve it
    get_pending_crnti_by_ue_index()[ue_index] = c_rnti;
    std::cout << "[UE_ID_TRACKER] LINK_CRNTI_PENDING: C_RNTI=" << std::hex << c_rnti << std::dec
              << " ue_index=" << ue_index << " (waiting for IMEISV from NGAP)" << std::endl;
  }

  /// Register RA-RNTI and TC-RNTI during RACH procedure
  /// \param ra_rnti RA-RNTI computed from PRACH occasion
  /// \param tc_rnti Temporary C-RNTI assigned for this RACH
  /// \param ta_value Initial TA value from RAR
  /// \param timestamp_ms Timestamp in milliseconds
  static void register_rach(uint16_t ra_rnti, uint16_t tc_rnti, int ta_value, long long unsigned int timestamp_ms)
  {
    std::lock_guard<std::mutex> lock(get_mutex());

    // Check if TC-RNTI is already associated with an IMEISV (from NGAP)
    auto& crnti_map = get_crnti_to_imeisv();
    if (crnti_map.find(tc_rnti) != crnti_map.end()) {
      // Known UE, update records
      const std::string& imeisv_str = crnti_map[tc_rnti];
      auto&              records    = get_records();
      auto&              rec        = records[imeisv_str];

      // Add RA-RNTI if not already present
      if (std::find(rec.ra_rntis.begin(), rec.ra_rntis.end(), ra_rnti) == rec.ra_rntis.end()) {
        rec.ra_rntis.push_back(ra_rnti);
      }

      // Add TA event
      rec.ta_history.push_back({timestamp_ms, ta_value, ta_event_type::RAR_TA, tc_rnti});

      std::cout << "[UE_ID_TRACKER] RACH: IMEISV=" << imeisv_str << " RA_RNTI=" << std::hex << ra_rnti
                << " TC_RNTI=" << tc_rnti << std::dec << " RAR_TA=" << ta_value << " TIME=" << timestamp_ms
                << std::endl;
    } else {
      // Unknown UE (IMEISV not yet received from NGAP), store in pending
      auto& pending = get_pending_rach();
      pending[tc_rnti] = {ra_rnti, ta_value, timestamp_ms, ""}; // empty imeisv_str
      std::cout << "[UE_ID_TRACKER] RACH_PENDING: RA_RNTI=" << std::hex << ra_rnti << " TC_RNTI=" << tc_rnti
                << std::dec << " RAR_TA=" << ta_value << " (waiting for IMEISV)" << std::endl;
    }
  }

  /// Register TA CE ACK event
  /// \param c_rnti C-RNTI of the UE
  /// \param ta_delta TA adjustment delta
  /// \param accumulated_ta Current accumulated TA value after applying delta
  /// \param timestamp_ms Timestamp in milliseconds
  static void register_ta_ce_ack(uint16_t c_rnti, int ta_delta, int accumulated_ta, long long unsigned int timestamp_ms)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto&                       crnti_map = get_crnti_to_imeisv();

    if (crnti_map.find(c_rnti) != crnti_map.end()) {
      const std::string& imeisv_str = crnti_map[c_rnti];
      auto&              records    = get_records();
      auto&              rec        = records[imeisv_str];

      rec.ta_history.push_back({timestamp_ms, ta_delta, ta_event_type::CE_ACK_TA, c_rnti});

      std::cout << "[UE_ID_TRACKER] TA_CE_ACK: IMEISV=" << imeisv_str << " C_RNTI=" << std::hex << c_rnti << std::dec
                << " TA_DELTA=" << ta_delta << " ACCUMULATED_TA=" << accumulated_ta << " TIME=" << timestamp_ms << std::endl;
    } else {
      std::cout << "[UE_ID_TRACKER] TA_CE_ACK_UNKNOWN: C_RNTI=" << std::hex << c_rnti << std::dec
                << " TA_DELTA=" << ta_delta << " ACCUMULATED_TA=" << accumulated_ta << " (no IMEISV found)" << std::endl;
    }
  }

  /// Get UE record by IMEISV
  static bool get_ue_record(const std::string& imeisv_str, ue_record& out_record)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto&                       records = get_records();

    if (records.find(imeisv_str) != records.end()) {
      out_record = records[imeisv_str];
      return true;
    }
    return false;
  }

  /// Get IMEISV by C-RNTI
  static bool get_imeisv_by_crnti(uint16_t c_rnti, std::string& out_imeisv)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto&                       crnti_map = get_crnti_to_imeisv();

    if (crnti_map.find(c_rnti) != crnti_map.end()) {
      out_imeisv = crnti_map[c_rnti];
      return true;
    }
    return false;
  }

  /// Get latest TA (RAR or CE) for a given RNTI, if known.
  static std::optional<int> get_latest_ta_by_rnti(uint16_t c_rnti)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto&                       crnti_map = get_crnti_to_imeisv();
    auto                        it        = crnti_map.find(c_rnti);
    if (it == crnti_map.end()) {
      return std::nullopt;
    }

    const std::string& imeisv_str = it->second;
    auto&              records    = get_records();
    auto               rec_it     = records.find(imeisv_str);
    if (rec_it == records.end()) {
      return std::nullopt;
    }

    const auto& history = rec_it->second.ta_history;
    for (auto hist_it = history.rbegin(); hist_it != history.rend(); ++hist_it) {
      if (hist_it->rnti == c_rnti) {
        return hist_it->ta_value;
      }
    }
    return std::nullopt;
  }

  /// Remove C-RNTI mapping when UE disconnects (called by scheduler on UE removal)
  static void remove_crnti(uint16_t c_rnti)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto& crnti_map = get_crnti_to_imeisv();
    
    if (crnti_map.find(c_rnti) != crnti_map.end()) {
      std::string imeisv_str = crnti_map[c_rnti];
      crnti_map.erase(c_rnti);
      std::cout << "[UE_ID_TRACKER] REMOVE_CRNTI: C_RNTI=" << std::hex << c_rnti << std::dec 
                << " IMEISV=" << imeisv_str << " (UE disconnected, C-RNTI can be reused)" << std::endl;
    }
  }

  /// Resolve pending RACH records when IMEISV is received (assumes mutex already locked)
  static void resolve_pending_rach_internal(const std::string& imeisv_str, uint16_t c_rnti)
  {
    auto& pending = get_pending_rach();

    if (pending.find(c_rnti) != pending.end()) {
      auto& pend_info = pending[c_rnti];
      auto& records   = get_records();
      auto& rec       = records[imeisv_str];

      // Add RA-RNTI
      if (std::find(rec.ra_rntis.begin(), rec.ra_rntis.end(), pend_info.ra_rnti) == rec.ra_rntis.end()) {
        rec.ra_rntis.push_back(pend_info.ra_rnti);
      }

      // Add TA event
      rec.ta_history.push_back(
          {pend_info.timestamp_ms, pend_info.ta_value, ta_event_type::RAR_TA, c_rnti});

      std::cout << "[UE_ID_TRACKER] RESOLVE_PENDING: IMEISV=" << imeisv_str << " RA_RNTI=" << std::hex
                << pend_info.ra_rnti << " TC_RNTI=" << c_rnti << std::dec << " RAR_TA=" << pend_info.ta_value
                << std::endl;

      pending.erase(c_rnti);
    } else {
      std::cout << "[UE_ID_TRACKER] RESOLVE_PENDING_NOT_FOUND: C_RNTI=" << std::hex << c_rnti << std::dec 
                << " (no pending RACH for this C-RNTI)" << std::endl;
    }
  }

  /// Resolve pending RACH records when IMEISV is received (public version with lock)
  static void resolve_pending_rach(const std::string& imeisv_str, uint16_t c_rnti)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    resolve_pending_rach_internal(imeisv_str, c_rnti);
  }

  /// Print TA history for a UE (for debugging)
  static void print_ta_history(const std::string& imeisv_str)
  {
    std::lock_guard<std::mutex> lock(get_mutex());
    auto&                       records = get_records();

    if (records.find(imeisv_str) == records.end()) {
      std::cout << "[UE_ID_TRACKER] No record found for IMEISV=" << imeisv_str << std::endl;
      return;
    }

    const auto& rec = records[imeisv_str];
    std::cout << "[UE_ID_TRACKER] ===== TA HISTORY for IMEISV=" << imeisv_str << " =====" << std::endl;
    std::cout << "[UE_ID_TRACKER] RA-RNTIs: ";
    for (auto ra_rnti : rec.ra_rntis) {
      std::cout << std::hex << ra_rnti << " ";
    }
    std::cout << std::dec << std::endl;

    std::cout << "[UE_ID_TRACKER] C-RNTIs: ";
    for (auto c_rnti : rec.c_rntis) {
      std::cout << std::hex << c_rnti << " ";
    }
    std::cout << std::dec << std::endl;

    std::cout << "[UE_ID_TRACKER] TA Events:" << std::endl;
    for (const auto& entry : rec.ta_history) {
      std::cout << "[UE_ID_TRACKER]   TIME=" << entry.timestamp_ms << " TYPE="
                << (entry.event_type == ta_event_type::RAR_TA ? "RAR" : "CE_ACK") << " VALUE=" << entry.ta_value
                << " RNTI=" << std::hex << entry.rnti << std::dec << std::endl;
    }
    std::cout << "[UE_ID_TRACKER] ========================================" << std::endl;
  }

private:
  struct pending_rach_info {
    uint16_t               ra_rnti;
    int                    ta_value;
    long long unsigned int timestamp_ms;
    std::string            imeisv_str; // IMEISV if known (from NGAP arriving before C-RNTI)
  };

  static std::mutex& get_mutex()
  {
    static std::mutex mtx;
    return mtx;
  }

  static std::map<std::string, ue_record>& get_records()
  {
    static std::map<std::string, ue_record> records;
    return records;
  }

  static std::map<uint16_t, std::string>& get_crnti_to_imeisv()
  {
    static std::map<uint16_t, std::string> mapping;
    return mapping;
  }

  static std::map<uint64_t, std::string>& get_ue_index_to_imeisv()
  {
    static std::map<uint64_t, std::string> mapping;
    return mapping;
  }

  static std::map<uint16_t, pending_rach_info>& get_pending_rach()
  {
    static std::map<uint16_t, pending_rach_info> pending;
    return pending;
  }

  static std::map<uint64_t, uint16_t>& get_pending_crnti_by_ue_index()
  {
    static std::map<uint64_t, uint16_t> pending_crnti;
    return pending_crnti;
  }
};

} // namespace srsran
