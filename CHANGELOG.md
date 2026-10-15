# Changelog

## 2.0.5 — first public release

- Native OBS filter for Apple Silicon Macs: multi-face smoothing, face slimming, whitening,
  rosy tone, and under-eye / nasolabial-fold softening.
- Optional smoothing of the neck and ears.
- Up to six faces with shared controls.
- Detectable conversion, graphics-allocation, and rendering failures disable beauty processing
  for the affected filter instance and pass through the original source.

Per-person settings and background protection are unavailable in this release.
Requires macOS 13+ and Apple Silicon OBS Studio 31+. The installer is not notarized.
