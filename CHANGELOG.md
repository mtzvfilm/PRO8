# Changelog

## 1.0.2 — 2026-11-26

Windows version (VST3 + CLAP) and engine fixes.

- Core and plugin code split into portable C++17 and a small OS layer (macOS / Windows); no change to the sound
- Windows: VST3 + CLAP build with the panel in WebView2, presets in %APPDATA%\PRO8
- Fix: notes held by the sustain pedal (CC 64) are now released when the pedal goes up (they hung until the same
  key was played again); keys still down keep sounding, mono returns to the last held key
- Fix: switching the arpeggiator on with keys held left those notes sounding forever (also on a preset change)
- Fix: moving VINTAGE, OSC SPREAD or TUNE (or any settings update) cut all sounding notes
- Fix: MIDI CCs that are not panel knobs (sustain, mod wheel, all notes off) were applied later on the main thread,
  out of order with the notes and wrong in offline bounces; they now act sample-accurately
- Fix: the panel's output volume slider showed 0 dB after reopening a project (the level itself was restored)
- Fix: preset names from imported .syx files are shown as text (a name with < > broke the preset list)
- UNISON: last-note priority like MONO; releasing the sounding key returns to the previous held key (also after the
  sustain pedal); arpeggiator notes no longer count as held keys in MONO / UNISON
- Third-party licenses: Microsoft WebView2 SDK loader (BSD 3-Clause, Windows build only) added
- 10 new sound demos on the project page
- macOS: otherwise the same plugin as 1.0.1 (file dialogs on the current API); projects and presets load unchanged

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
