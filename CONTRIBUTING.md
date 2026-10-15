# Contributing

Use GitHub Issues for reproducible bugs and feature requests, and pull requests for proposed changes.
Include OBS version, macOS version, chip model, input resolution / frame rate, output frame rate,
filter order, and concise reproduction steps when relevant.

Keep passwords, API keys, local home paths, company information, face-landmark dumps, and private
footage out of reports. Share only the relevant redacted log lines. Do not attach identifiable
people or workplace material without permission. Public issues and Git history are public records.

Build instructions and test-tool cautions are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
Use a dedicated OBS test profile and scene collection. Never run tests that restart OBS, mutate
sources, or start streams during a live show.

For tracking or rendering changes, compare the previous version with your changes on identical
material. Cover multiple faces, head turns, occlusion, tracking loss, and recovery. Report the
hardware, frame rates, test duration, and any frame drops. Do not infer multi-hour stability from
a short smoke test. Keep background protection disabled until a replacement has been validated.

Use a public handle and a GitHub-provided noreply email if you want to keep your real email private.
Do not add agent session notes, local credentials, build directories, or test recordings to commits.

Contributions must be compatible with `GPL-2.0-or-later`. By submitting your own contribution, you
agree to license it under that license. Preserve the license and attribution of third-party work.
