# Third-party notices

BetterFace source is distributed under `GPL-2.0-or-later`. Copyright (C) 2026 vanawaker
and BetterFace contributors. The full license is in [LICENSE](LICENSE).

## OBS Studio / libobs

The plugin builds against OBS Studio's libobs headers and links to libobs supplied by the user's
OBS installation. OBS Studio is copyright its respective authors and contributors and is licensed
under GNU GPL version 2 or, at your option, any later version.

- Project: https://github.com/obsproject/obs-studio
- Header revision: `31.0.0` / `0b2c85845e20f41c612cfe2583e380bd436b6b3c`
- License: https://github.com/obsproject/obs-studio/blob/31.0.0/COPYING

The dependency checkout retains the upstream copyright notices and COPYING file. No OBS application
or standalone libobs binary is bundled in this release.

## Apple system frameworks

The plugin uses Foundation, CoreVideo, CoreGraphics, and Vision from the user's macOS installation.
These system components and their models are not redistributed by this project. macOS and Apple
frameworks remain subject to Apple's terms.

## Optional development tools

Python packages and FFmpeg used by the test utilities are installed separately and are not included
in the plugin installer or source archive. Refer to each upstream package for its license.
