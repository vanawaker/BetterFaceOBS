# Privacy

Face detection and beauty processing run locally inside OBS using Apple's Vision framework.
The plugin implements no network client or analytics. Optional development diagnostics can
contain face landmarks; review them before sharing.

## Release checks

The v2.0.5 source, reachable Git history, and expanded installer were checked for private
identifiers and credential patterns. No matches were found within the reviewed deliverables.
Test footage, recordings, workstation logs, local credentials, and personal signing certificates
are excluded. Builds remap local source paths and omit debug information. The installer uses
an ad-hoc signature and contains no developer certificate or Team ID.

The project is associated with the maintainer's public GitHub account, `vanawaker`.
These checks cannot detect every possible form of personal information; future contributions
and release assets require their own review.
