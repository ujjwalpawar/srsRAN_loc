/* Lightweight tracker to correlate MAC CE LCIDs (e.g. TA CMD) with HARQ IDs.
 * This header is intentionally header-only and thread safe (mutex guarded). It is
 * purposely small and best-effort: it helps the debugging instrumentation print
 * only HARQ events that are relevant to previously queued/allocated CEs.
 */
#pragma once

#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace srsran {
namespace ta_ce_tracker {

inline std::mutex& _mutex()
{
  static std::mutex m;
  return m;
}

inline std::unordered_set<int>& _pending_lcids()
{
  static std::unordered_set<int> s;
  return s;
}

inline std::unordered_set<unsigned>& _tracked_harqs()
{
  static std::unordered_set<unsigned> s;
  return s;
}

inline std::unordered_map<unsigned, std::vector<int>>& _harq_to_lcids()
{
  static std::unordered_map<unsigned, std::vector<int>> m;
  return m;
}

inline std::unordered_map<int, std::pair<int,int>>& _pending_ce_info()
{
  static std::unordered_map<int, std::pair<int,int>> m; // lcid -> (tag_id, ta_cmd)
  return m;
}

inline std::unordered_map<unsigned, std::vector<std::pair<int,std::pair<int,int>>>>& _harq_to_ce_infos()
{
  // h_id -> vector of (lcid, (tag_id, ta_cmd))
  static std::unordered_map<unsigned, std::vector<std::pair<int,std::pair<int,int>>>> m;
  return m;
}

// Record that a CE (identified by LCID) was queued. tag_id/ta_cmd kept for future use if needed.
inline void track_ce_queued(int lcid, int tag_id = -1, int ta_cmd = -1)
{
  std::lock_guard<std::mutex> lk(_mutex());
  _pending_lcids().insert(lcid);
  if (tag_id != -1) {
    // ALWAYS update with the MOST RECENT ta_cmd value (not the first one)
    _pending_ce_info()[lcid] = std::make_pair(tag_id, ta_cmd);
  }
}

// Record that a CE (identified by LCID) was allocated to the TB. This is useful to
// ensure the CE actually entered the transport block.
inline void track_ce_allocated(int lcid, int tag_id = -1, int ta_cmd = -1)
{
  std::lock_guard<std::mutex> lk(_mutex());
  _pending_lcids().insert(lcid);
  if (tag_id != -1) {
    // ALWAYS update with the MOST RECENT ta_cmd value (not the first one)
    _pending_ce_info()[lcid] = std::make_pair(tag_id, ta_cmd);
  }
}

// Link a HARQ id with the LCIDs included in the grant. If any of the LCIDs were pending
// as CE LCIDs, the HARQ is marked as tracked.
template <typename Container>
inline void link_harq_with_lcs(unsigned h_id, const Container& lcs)
{
  std::lock_guard<std::mutex> lk(_mutex());
  for (const auto& lc : lcs) {
    try {
      int lcid = static_cast<int>(lc.lcid.value());
      if (_pending_lcids().erase(lcid) > 0) {
        _tracked_harqs().insert(h_id);
        // Avoid pushing duplicate LCIDs for the same HARQ id.
        auto &vec = _harq_to_lcids()[h_id];
        if (std::find(vec.begin(), vec.end(), lcid) == vec.end()) {
          vec.push_back(lcid);
        }
        // If we have payload info for this LCID, copy it to harq info map for later reporting.
        // NOTE: We do NOT erase from _pending_ce_info here! Leave it until reset_tracker() is called.
        // This prevents race conditions where a second CE with same LCID is queued before the first
        // HARQ is ACKed, which would create a new _pending_ce_info entry that could be wrongly linked
        // to a different HARQ.
        auto it = _pending_ce_info().find(lcid);
        if (it != _pending_ce_info().end()) {
          auto &infos = _harq_to_ce_infos()[h_id];
          // Avoid duplicate ce infos (same lcid and same payload triple).
          bool found = false;
          for (const auto &e : infos) {
            if (e.first == lcid && e.second.first == it->second.first && e.second.second == it->second.second) {
              found = true;
              break;
            }
          }
          if (!found) {
            infos.push_back({lcid, it->second});
          }
          // DO NOT ERASE: _pending_ce_info().erase(it);
        }
      }
    } catch (...) {
      // Best-effort: ignore errors in debug tracking.
    }
  }
}

inline bool is_tracked_harq(unsigned h_id)
{
  std::lock_guard<std::mutex> lk(_mutex());
  return _tracked_harqs().find(h_id) != _tracked_harqs().end();
}

inline void untrack_harq(unsigned h_id)
{
  std::lock_guard<std::mutex> lk(_mutex());
  _tracked_harqs().erase(h_id);
  _harq_to_lcids().erase(h_id);
}

inline std::vector<int> get_tracked_lcids(unsigned h_id)
{
  std::lock_guard<std::mutex> lk(_mutex());
  auto it = _harq_to_lcids().find(h_id);
  if (it == _harq_to_lcids().end()) {
    return {};
  }
  return it->second;
}

inline std::vector<std::pair<int,std::pair<int,int>>> get_tracked_ce_infos(unsigned h_id)
{
  std::lock_guard<std::mutex> lk(_mutex());
  auto it = _harq_to_ce_infos().find(h_id);
  if (it == _harq_to_ce_infos().end()) {
    return {};
  }
  return it->second;
}

// Reset all tracker state. Call this when a RAR is sent (RACH completion) to start fresh tracking
// for the new connection and avoid stale CE entries from previous RACH sequences.
inline void reset_tracker()
{
  std::lock_guard<std::mutex> lk(_mutex());
  _pending_lcids().clear();
  _tracked_harqs().clear();
  _harq_to_lcids().clear();
  _pending_ce_info().clear();
  _harq_to_ce_infos().clear();
}

} // namespace ta_ce_tracker
} // namespace srsran
