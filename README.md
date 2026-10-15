# BetterFace for OBS

[简体中文](README.zh-CN.md)

A native OBS Studio video filter for Apple Silicon Macs, with multi-face skin smoothing,
face slimming, whitening, rosy tone, and under-eye / nasolabial-fold softening.

## Download

[Download v2.0.5](https://github.com/vanawaker/BetterFaceOBS/releases/tag/v2.0.5) — choose
`BetterFace-for-OBS-2.0.5.pkg`. Source code, installation guides, licenses, and SHA-256 checksums
are available on the same release page.

1. Quit OBS completely and run the installer. The package is not notarized; macOS may require
   approval under **System Settings → Privacy & Security**.
2. Restart OBS, open a camera source's **Filters**, and add **BetterFace for OBS**.
3. Adjust smoothing, slimming, whitening, rosy tone, and lift. Start with modest settings and
   test with your intended camera and stream configuration.

See the [installation and removal guide](dist_template/Installation.txt) for details.

## Current version

Version 2.0.5 is the first public release. Background protection is disabled.
All detected faces currently share the same effect sliders;
per-person settings are not implemented. This project is not affiliated with Facebetter or OBS Studio.

## Requirements

- An Apple Silicon Mac running macOS 13 or newer.
- The Apple Silicon build of OBS Studio 31 or newer; development verification used OBS 32.2.2.
- To build: Xcode Command Line Tools, Git, Python 3, and an OBS installation.

Windows, Linux, and Intel Macs are not supported by this release. Compatibility with every OBS
or macOS version in the stated range has not been independently tested.

## Build and package

```sh
git clone https://github.com/vanawaker/BetterFaceOBS.git
cd BetterFaceOBS
./tools/fetch_obs_headers.sh
./tools/build_plugin.sh 2.0.5
./tools/package.sh 2.0.5 build/BetterFace.plugin
```

The build is isolated inside this checkout and does not install or restart OBS.
The outputs include `dist/BetterFace-for-OBS-2.0.5.pkg` and Chinese / English installation guides.
The installer is not notarized. Review the installation guide before installing it.

`OBS_APP` can point to a different OBS application bundle. `OBS_INCLUDE_DIR` can point to
an existing checkout's `libobs` headers. The default header revision is pinned by the fetch script.
The release binary removes debug symbol records and remaps local source paths before compilation.

## Using the filter

After installation and restarting OBS, open a source's Filters panel and add **BetterFace for OBS**.
Adjust smoothing, slimming, whitening, rosy tone, and lift independently. A separate option extends
skin smoothing to the neck and ears. Up to six faces can be tracked.

For chroma-keyed sources, test the filter after keying as described in the installation guide.
Check the complete filter chain with the intended camera, lighting, and streaming load.

## Failure behavior and limitations

Detected conversion, GPU allocation, and rendering errors disable beauty processing for that
filter instance and pass the original source through. Changing settings or toggling the filter
does not clear this latch; recreate the filter or restart OBS after the stream.
This is not process isolation and cannot contain every framework, driver, or native-code failure.
Face tracking, occlusion handling, and performance still depend on the scene and hardware.
Slimming may distort nearby background edges while background protection is disabled.

## Privacy

Face detection and processing use Apple's on-device Vision framework inside OBS. The plugin
implements no network client or analytics. Optional development diagnostics can contain face
landmarks; keep them private unless you have reviewed and intentionally shared them.

The public source and installer exclude local credentials, private footage, workstation paths,
and personal developer signing certificates. See the [release privacy review](docs/PRIVACY_REVIEW.md)
for its scope and limits.

## Contributing and roadmap

See [development notes](docs/DEVELOPMENT.md). Test media, recordings, screenshots, local credentials,
workstation notes, and build caches are intentionally excluded from the source distribution.

Contributions are welcome; read [CONTRIBUTING.md](CONTRIBUTING.md) before sending changes or reports.
Planned areas include stable per-person settings and background compensation. These features are
not included in v2.0.5 and do not have a promised release date.

## License

Copyright (C) 2026 vanawaker and BetterFace contributors.

BetterFace is free software: you may redistribute and modify it under the GNU General Public
License as published by the Free Software Foundation, either version 2 of the License, or
(at your option) any later version. It is distributed without any warranty. See [LICENSE](LICENSE)
and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

SPDX license identifier: `GPL-2.0-or-later`.
