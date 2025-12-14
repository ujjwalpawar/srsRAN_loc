/*
 * SPDX-FileCopyrightText: 2025 Software Radio Systems Limited
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

# Multi-RU SRS Distribution – Design & Milestones

This document tracks the changes required to let the serving gNB export Sounding Reference Signal (SRS) schedules to
non-serving RUs, collect their correlation results, and keep all schedulers consistent. Each milestone lists the
implementation goals plus the concrete files to touch.

## Milestone 1 – Scheduler instrumentation & data model

**Goal:** introduce a reusable representation for SRS schedule exports, including globally unique positioning identities,
and emit it whenever the MAC allocates an SRS PDU (for both connected and non-connected UEs).

- `include/srsran/ran/rnti.h`: add helpers to derive a positioning RNTI from IMEISV + gNB ID (`make_positioning_rnti`)
  so different cells never collide on neighbour measurements.
- `lib/cu_cp/...` (F1 positioning handlers): fetch the UE IMEISV, derive the positioning RNTI via the helper above, and
  populate both the IMEISV and derived RNTI inside `positioningMeasurementRequest`.
- `include/srsran/scheduler/srs_schedule_exporter.h` *(new)*: abstract notifier that receives
  `srs_schedule_descriptor` structs (slot timing, resource config, TA hints, IMEISV, schedule ID, etc.).
- `lib/scheduler/srs/srs_scheduler_impl.{h,cpp}`: store an optional exporter reference, fill the descriptor inside
  `allocate_srs_opportunity`, and call the exporter before enqueuing the PDU.
- `lib/scheduler/ue_scheduling/ue_event_manager.cpp`: wire the exporter into the constructor so the MAC layer can
  configure it.
- `lib/mac/mac_sched/srsran_scheduler_adapter.{h,cpp}`: plumb configuration from DU to scheduler.
- Testing: extend `tests/unittests/scheduler/srs_scheduling/srs_scheduler_test.cpp` with a stub exporter to validate
  that descriptors are produced for both connected and positioning UEs.

**Exit criteria:** the positioning request carries an IMEISV-derived RNTI; a build flag or CLI option can register a
dummy exporter and see log output per scheduled SRS PDU (including IMEISV + schedule ID).

## Milestone 2 – Distribution path & remote interface

**Goal:** forward SRS descriptors to the central controller and allow non-serving gNBs to consume them.

- `apps/gnb/gnb_appconfig.*`: add configuration knobs for the SRS exporter (enable flag, remote endpoint, queue sizes).
- `apps/services/remote_control` or a new service: implement `srs_schedule_remote_exporter` that serializes descriptors
  (JSON/CBOR) and publishes them via the existing remote server.
- `apps/services/remote_control/remote_command*`: add commands so a central controller can push positioning requests
  (`positioning_request`) or fetch active schedules (useful for bootstrap or debugging).
- `lib/fapi_adaptor/phy/phy_to_fapi_results_event_translator.cpp`: augment SRS indication PDUs with the exported
  `schedule_id` so the server can correlate measurements from multiple cells.

**Exit criteria:** enabling the exporter in `gnb.conf` produces remote messages per allocation; the CLI can retrieve
the latest schedule buffer.

## Milestone 3 – Conflict handling & capture guidance

**Goal:** ensure neighbouring-cell UEs can be scheduled alongside serving UEs without silent drops.

- `lib/scheduler/srs/srs_scheduler_impl.cpp`: extend `allocate_srs_opportunity` to detect overlapping RBs/symbols; apply
  a deterministic policy (e.g., prioritise connected UE, then positioning UE) and emit a `srs_schedule_reject` via the
  exporter when a conflict occurs.
- `include/srsran/scheduler/srs_schedule_exporter.h`: add result types for `schedule` and `reject`, including reasons
  (`max_pdus`, `rb_conflict`, etc.).
- `lib/scheduler/cell/resource_grid.*`: expose helper APIs to probe UL RB availability so the SRS scheduler can search
  for the next feasible slot when conflicts occur.
- `tests/unittests/scheduler/srs_scheduling/srs_scheduler_test.cpp`: cover the new conflict policy; ensure neighbour
  UE resources are either rescheduled or rejected with the correct reason.
- Documentation: extend this file with the final behaviour and add a short user-facing note (e.g.,
  `docs/gnb_configuration.md`) on how to configure multi-RU SRS.

**Exit criteria:** when two SRS resources target the same slot/RBs, the exporter receives an explicit reject (or a
rescheduled descriptor), and the MAC/PHY remain consistent with the decision.

---

Progress tracking: update each milestone section with completion notes (date, commit hash) as the implementation lands.
