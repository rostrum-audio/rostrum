# Stream readiness

On the OBS page, select **Check stream readiness**. This reads the current engine controls,
selected/resolved devices, PipeWire channel links and their processing states, and a live
obs-websocket snapshot. It does not create links, unmute anything, change OBS settings, play
sounds, record audio, enable fallback, or upload data. OBS setup/undo remain separate actions.

The summary counts individual checks, rather than claiming overall stream readiness. Controls,
Devices, Routing and OBS capture are separate expandable groups. All Needs attention results
appear directly below the summary, with concise explanations and navigation to existing
settings or OBS setup. These links only navigate; they do not apply a correction. Remaining
unverified groups open automatically; healthy or intentionally excluded/idle groups start
collapsed. Select any row to expand its full observed evidence; keyboard activation works too.
Open groups and evidence survive live
refreshes. The permanent recording/audience limitation is a separate scope note, excluded
from the summary's unresolved-check count. Colours follow the desktop theme, with icons and
text labels so that status does not depend on colour alone.

## Manual readiness observations (2026-10-04)

The user confirmed these manual readiness-panel results:

- Normal OBS capture: Mic/Aux and Rostrum Stream Mix were Verified; unsupported
  Alerts/Reminders remained Not verified. Browser Audio and Spotify were excluded/idle.
- Desktop Headphones Only: Intentionally excluded/idle. The user also confirmed the earlier
  recording-isolation test passed; that recording evidence remains scoped to its earlier build.
- Rostrum mic mute: Effective stream mic changed to Needs attention.
- Required OBS capture muted: Rostrum Stream Mix changed to Needs attention.
- OBS closed: capture configuration changed to Not verified and previous verified capture
  rows disappeared.

These confirm displayed results and invalidation, not a new end-to-end recording or audience
audio test of this readiness artifact. The user has not manually tested keyboard navigation,
narrow layouts, scaling or a screen reader. Automated QML checks and rendering cover keyboard,
accessible metadata, narrow layouts and 200% Qt scaling; they do not replace a screen-reader test.

## Inspection evidence and check scope

Read-only live inspection on 2026-10-04 found Spotify node 299 linked both to Rostrum Music
(node 122, links 358/129) and directly to the Flatpak OBS-owned capture adapter (node 131,
links 97/328, FL/FR). Adapter monitor links 148/149 fed OBS capture stream 137. The direct
Spotify links were paused at inspection; they establish an alternate capture path around Music's
exclusions, not proof of audible recording at that instant. These were OBS capture links, not
Rostrum peak-meter links. No classification change was made to dismiss this warning. IDs are
specific to that snapshot and may change. For recording entirely through Rostrum, disable/remove
the separate Spotify app capture in OBS and use Rostrum Stream Mix; the check does not do this
automatically. OBS mute, scene/track selection and recorded audio still require confirmation.

Each result has a specific scope:

- **Verified:** the stated control/property or complete active channel path was observed.
  This is not proof that audible sound reaches a recording or audience.
- **Needs attention:** observed evidence establishes a missing required path/device, an error,
  a forbidden destination send, effective silence, or an unwanted capture target.
- **Intentionally excluded/idle:** the selected destination excludes that output, a bus is
  muted/at zero/removed by solo, an app or complete path is idle, or an extra OBS capture is
  disabled. An idle app or silent meter alone is never a routing failure.
- **Not verified:** observations are disconnected, incomplete, unsupported, stale, ambiguous,
  or still negotiating. A node's existence alone does not establish routing or delivery.

The effective mic and Stream Mix rows include session controls: panic and held push-to-talk/
push-to-mute. A held push-to-talk overrides a saved mic mute; push-to-mute and panic take
precedence. Gain positions, destination and solo are evaluated too. Separate observed-node
rows report available PipeWire mute/gain properties. A live scene change can briefly leave
controls/properties unsettled; re-check after routing settles.

Device rows show the saved selection (or system default), the actually resolved node, and
whether microphone fallback was explicitly enabled. A missing saved mic is reported even
when an enabled fallback supplies another mic, so that replacement is visible. Headphones
may fall back without changing the saved selection. The check never changes these choices.

Both headphone and Stream Mix paths are inspected for every playback bus. Headphones Only
requires no Desktop/bus sends to Stream Mix; Stream Only requires no sends to headphones,
including links that are currently inactive. Expected paths require complete channel ports
and matching links. Paused links are labelled idle; errors are not. Unmapped source channels
remain unverified, even if the other channel links are active. The resolved microphone
path includes its filter stage when active. Rostrum Mic is a separate OBS capture from Stream
Mix, rather than an assumed mic link into the playback mix.

OBS checks compare supported configured capture targets with uniquely identified live OBS
capture streams and their channel links. They read mute, gain, track assignments and directly
enabled program-scene items, including global audio sources. Headphones/default monitors,
direct app capture and other captures outside Rostrum's intended outputs can bypass its
exclusions. Muted extra captures are excluded; a muted required Rostrum capture needs attention.

Groups/nested scenes, unsupported source kinds and failed fields remain unverified. An
incomplete OBS observation does not become "unmuted" or "missing capture" by default. The
check uses read requests from the [OBS WebSocket protocol](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md).
Read requests time out after five seconds each. Replies from an invalidated check are discarded.
OBS changes/disconnects and closing the page invalidate the snapshot; after an explicit check,
observations refresh while the page is open. Snapshots older than ten seconds are not accepted.

Output track selection, audio contributed by unsupported sources/processors, physical mic
switches, transient leakage at every sample, recording contents and audience delivery remain
unverified. The check does not take over an OBS output or perform an end-to-end sound test.
A short manual recording is still the final confirmation.

Existing go-live warnings use the same evaluator for effective controls and preserve their
start-only banner behavior and observed-capture criteria. Only fixed warnings disappear;
muting later intentionally does not create a new banner, and a readiness check does not
restore dismissed warnings. The explicit check adds configuration/scope/freshness evidence.

Validation includes pure evaluator fixtures, mock loopback OBS read/failure/timeout/disconnect
checks, the QML panel and a private PipeWire/WirePlumber test that verifies Rostrum's actual
link-state mirror with deterministic signals. Run `ctest --test-dir build --output-on-failure`
in a build configured with `ROSTRUM_AUDIO_INTEGRATION_TESTS=ON`. The integration test
`audio_readiness` uses the same fail-closed private session and supervised cleanup as the
[audio safety suite](../tests/integration/README.md); it never uses the desktop audio session.
