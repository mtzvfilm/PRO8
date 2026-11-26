# Changelog

## 1.0.1 — 2026-11-26

Licensing cleanup.

- The PRO-800 factory bank is no longer shipped: the PRO8 bank is now bank A and loads by default
  (your own PRO-800 banks still import as .syx)
- License texts (GPL-3.0 and third-party: CLAP, clap-wrapper, VST3 SDK, AudioUnitSDK, DSEG font) shipped in every
  plugin bundle and shown in the installer
- SPDX license headers in the sources, trademark disclaimer extended (README, project page)

## 1.0.0 — 2026-11-26

First public release.

- 8-voice (up to 64) analog engine calibrated on a hardware PRO-800
- CLAP, VST3 and AU for macOS (universal)
- Effects, preset library (26 banks, .syx / .fxp / .fxb), arpeggiator, DAW tempo sync, poly unison, VINTAGE / OSC SPREAD, master tune
- Factory content: PRO-800 factory bank (A) and the PRO8 bank (B)
