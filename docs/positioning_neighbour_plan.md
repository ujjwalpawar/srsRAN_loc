# Neighbour Positioning – Implementation Plan

Status key: `[done]` completed, `[wip]` in progress, `[todo]` not started.

## Milestone 1 – Accept neighbour positioning requests and export schedules
- [done] Allow positioning requests using C-RNTI (neighbour path no longer asserts).  
  - Files: `lib/scheduler/srs/srs_scheduler_impl.cpp` (neighbour branch permits C-RNTI).
- [done] Export start/stop with IMEISV and SFN/slot debug logs.  
  - Files: `lib/scheduler/srs/srs_schedule_file_exporter.cpp` (START/STOP/SKIP prints).
- [done] Trace scheduled SRS SFN/slot for serving/neighbour alignment (printf).  
  - Files: `lib/scheduler/srs/srs_scheduler_impl.cpp` (printf per SRS allocation).
- [todo] Harden IMEISV propagation for stops when teardown races with identity tracker.

## Milestone 2 – Neighbour PHY SRS demap/estimation
- [wip] Add positioning-aware SRS RX trace to confirm demap for foreign RNTIs.  
  - Files: `lib/fapi_adaptor/phy/phy_to_fapi_results_event_translator.cpp` (printf on positioning SRS RX with SFN/slot/rnti/TA).
- [todo] Allow demap for unknown UEs explicitly if blocked elsewhere; ensure resource config is applied as-is.
- [todo] Validate demap uses schedule periodicity/offset (SFN/slot alignment) and logs SFN/slot/resource.

## Milestone 3 – Measurement export
- [todo] Define exporter interface for positioning SRS measurements (schedule_id, IMEISV, RNTI, SFN/slot, UE resource id, CSI/correlation).  
  - New: `include/srsran/scheduler/positioning_srs_measurement_exporter.h`, impl under `lib/scheduler/srs/`.
- [todo] Wire exporter into PHY SRS RX to publish estimates; add remote publisher (e.g., remote_control service) to push JSON/CBOR to controller.

## Milestone 4 – Validation & tooling
- [todo] Add debug/health checks: per SRS RX log SFN/slot/resource/IMEISV; optional ring buffer query via remote command.  
  - Files: remote_control command (new) to fetch recent schedules/measurements.
- [todo] Integration test plan: serve + neighbour gNB with shared SRS config, PTP sync, verify aligned START/STOP logs and received measurements.
