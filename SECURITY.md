# Security policy

## Reporting a vulnerability

Email **security@getrostrum.dev**. Please do not open a public issue for a security problem.

Include what you found, the Rostrum version (Settings → About), your
distribution, and the steps to reproduce it. You will get a reply from a person, and you will be
credited in the fix unless you ask not to be.

## Supported versions

Only the latest release and the `main` branch get security fixes.

## What is in scope

Rostrum runs as your user and never asks for root. The areas that matter most:

- Files Rostrum writes outside its own config: the PipeWire rule fragments in
  `~/.config/pipewire/`, the autostart entry, and OBS's scene collection when you press Set Up OBS
  with OBS closed.
- The OBS integration: Rostrum reads the obs-websocket password from OBS's own config and only
  connects to `127.0.0.1`.
- Parsing of scene files, imported scene bundles, and OBS scene collections.

General questions go to **hello@getrostrum.dev**.
