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

#include "fmt/format.h"
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace srsran {

/// \remark See TS 38.331 - RNTI-Value and TS 38.321, Table 7.1-1: RNTI Values. Values: (0..65535)
enum class rnti_t : uint16_t {
  INVALID_RNTI = 0x0,
  MIN_CRNTI    = 0x1,
  // ...
  MAX_CRNTI         = 0xffef,
  MIN_RESERVED_RNTI = 0xfff0,
  // ...
  MAX_RESERVED_RNTI = 0xfffd,
  P_RNTI            = 0xfffe,
  SI_RNTI           = 0xffff
};

/// Checks whether RNTI value corresponds to a C-RNTI value.
constexpr bool is_crnti(rnti_t rnti)
{
  return rnti <= rnti_t::MAX_CRNTI and rnti >= rnti_t::MIN_CRNTI;
}

/// Converts integer to RNTI value.
constexpr rnti_t to_rnti(std::underlying_type_t<rnti_t> number)
{
  return static_cast<rnti_t>(number);
}

/// Converts RNTI value to integer.
constexpr inline uint16_t to_value(rnti_t rnti)
{
  return static_cast<uint16_t>(rnti);
}

/// Creates a deterministic positioning RNTI from an IMEISV string and optional salt.
inline rnti_t make_positioning_rnti(std::string_view imeisv, uint32_t salt = 0)
{
  // 32-bit FNV-1a hash with optional salt.
  uint32_t hash = 0x811C9DC5u ^ salt;
  constexpr uint32_t fnv_prime = 0x01000193u;
  for (char c : imeisv) {
    hash ^= static_cast<uint8_t>(c);
    hash *= fnv_prime;
  }

  // Keep 15 entropy bits and set the MSB to mark this as positioning-specific.
  uint16_t rnti_value = static_cast<uint16_t>((hash & 0x7FFFu) | 0x8000u);
  if (rnti_value == to_value(rnti_t::SI_RNTI)) {
    // Avoid colliding with SI-RNTI.
    rnti_value ^= 0x0100u;
  }
  if (rnti_value == to_value(rnti_t::INVALID_RNTI)) {
    rnti_value = 0x8001u;
  }
  return to_rnti(rnti_value);
}

} // namespace srsran

// Formatters
namespace fmt {
template <>
struct formatter<srsran::rnti_t> {
  template <typename ParseContext>
  auto parse(ParseContext& ctx)
  {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(srsran::rnti_t rnti, FormatContext& ctx) const
  {
    return format_to(ctx.out(), "{:#x}", to_value(rnti));
  }
};

} // namespace fmt
