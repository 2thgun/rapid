# Validation status

The native C++ runtime and Windows companion are implemented. The Qt touchscreen
provides Drive, Timing, Vehicle, Tyres and stacked Graphs pages.

## Automated checks

- Runtime, v4 authentication, recording, archive and network tests run under CTest.
- Qt model checks cover source-sample deduplication, missing channels, menu gaps,
  HTTP failures and session changes.
- Browser graph tests cover freshness, history aging and duplicate polling.
- CI builds the Qt target together with the native runtime in Debug and Release.

## Hardware acceptance

The Qt repair was built on the Raspberry Pi and all five pages plus the Wi-Fi
selector were checked using screenshots and X input events. The Wi-Fi selector
offers Home, AP and Off; the network worker selects Home at startup.

A real AC1/Content Manager driving rehearsal, physical finger calibration check,
and MoTeC inspection remain separate acceptance steps. Synthetic telemetry and
screenshots do not establish simulator or physical-wheel correctness.
