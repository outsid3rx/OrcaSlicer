# Local agent control

[English](./agent-control.md) | [Русский](./agent-control.ru.md)

The agent-control module exposes the running FFF editor to local clients through
HTTP JSON-RPC 2.0. A separate MCP adapter can use the same API as a regular Python,
JavaScript or C++ client. The module lives in `src/slic3r/AgentControl/`; it does not
depend on the Design/CAD tab or the embedded Python plugin runtime.

## Ownership and integration

`RpcServer` owns a loopback listener and its network worker. `Commands` owns a
bounded, synchronized registry of copied requests and job results. `Controller`,
owned by `MainFrame`, drains that queue on a wx timer. Transport callbacks never
wait for the UI thread or retain live model/config pointers.

`SettingsDomain`, `SceneDomain`, `GeometryDomain`, `DevicesDomain` and `PrintingDomain` own their
command schemas and handlers. `Controller` owns target/state validation, admission,
operation guards, asynchronous completion and cancellation. Public operations
remain explicitly named; there is no arbitrary Plater, Python or G-code evaluator.

Settings edits strict-parse a detached candidate and validate affected effective
configs with native `DynamicPrintConfig::validate(true)` before writing. Process
and edited-preset commits enter `Tab::apply_agent_config` as a complete copy;
model/plate overrides use their native owners and refresh the settings UI without
changing UI selection. An edit has one native Undo snapshot. Changes remain
unsaved unless the client explicitly asks to save a project or preset.

The public `Plater::slice_current_plate` delegates to the same current-plate action
as the UI, including pre-slice setup and warnings. `BackgroundSlicingProcess`
assigns each successful start a monotonically increasing run ID. Progress and
completion events carry that ID, so a queued event from another run cannot finish
an agent job. Estimates come from the native `GCodeProcessorResult`.

Small `Hooks.hpp` calls connect document/config changes, slicing and serialization
to the module. The hooks are no-ops when the feature is compiled out. Operation
guards cover project import/reset/export, snapshot capture, undo/redo and config
propagation; the timer does not enter those critical sections from a nested event
loop. Close and GUI recreation requests are deferred until an executing command or
serialization has left its stack. Shutdown stops admission, abandons queued work,
closes sockets, joins the network worker and removes the window's discovery file
before destroying Plater.

## Activation and discovery

`SLIC3R_AGENT_CONTROL` controls compilation and defaults to ON. The listener remains
off unless the process is launched with `ORCA_AGENT_CONTROL=1`. For example, launch
a custom macOS build directly:

```sh
ORCA_AGENT_CONTROL=1 /path/to/OrcaSlicer.app/Contents/MacOS/OrcaSlicer
```

Use Orca's existing preference for allowing multiple application instances when
opening several windows. Each enabled MainFrame gets a different endpoint and a
private descriptor at:

```text
<Orca data_dir>/cache/agent-control/<session_id>-<window_id>.json
```

The descriptor contains protocol version, endpoint, PID, session/window IDs and a
secret auth token. POSIX files/directories are owner-only; Windows files have a
protected current-user ACL. Publication is atomic and refuses to overwrite an
existing descriptor. A stopped owner removes only its own file. Clients ignore
stale descriptors after failed authenticated probes; they never select by PID,
port or project name, and do not delete files on a probe timeout.

| Identity | Lifetime |
|---|---|
| `session_id` | Random UUID v4 for the process; preserved during GUI recreation |
| `window_id` | Random UUID v4 for a MainFrame; replaced during GUI recreation |
| `document_epoch` | Changes when the document is reset or files are imported |
| `auth_token` | Random 256-bit secret for the listener; independent of IDs |

The existing executable-path instance hash and persistent updater/machine UUID
serve different lifetimes and are not used as these runtime IDs.

## Wire protocol

Send `POST /api/v1/rpc` to the descriptor's `http://127.0.0.1:<port>` address with
`Authorization: Bearer <auth_token>` and `Content-Type: application/json`. Host must
match the exact loopback endpoint. Browser Origins, redirects, public/LAN bindings
and arbitrary code execution are not part of this API. Printer transmission and
device control require separate owner opt-in, described below.
Header/body deadlines, connection limits, request limits, duplicate-key rejection
and JSON depth limits are enforced before command admission. HTTP bodies are
limited to 1 MiB; command admission applies the smaller serialized-JSON limit below.

An initial `system.describe` can omit target. Every other request must include
the exact pair returned by discovery:

```json
{
  "jsonrpc": "2.0",
  "id": "client-unique-request-id",
  "method": "scene.describe",
  "params": {
    "target": {
      "session_id": "<full-session-uuid>",
      "window_id": "<full-window-uuid>"
    }
  }
}
```

All admitted commands except `jobs.get` immediately return `result.job_id` and
`result.target`. Poll `jobs.get` with the same target and `job_id`; it returns the
job's `state` and, when terminal, `result` or `error`. A timeout in the client is
not a failed command: retain the job ID and poll again. If the admission response
was lost, retry the exact envelope with the same request ID. Reusing a retained
ID for different content is rejected.

The registry retains at most 256 jobs, evicting only terminal entries. Admission
limits a serialized request to 64 KiB and the retained request copies to 4 MiB
(the parsed JSON representation has additional allocator overhead). The complete
serialized status of one job is limited to 3 MiB, and retained statuses/results
share a 16 MiB budget. Status pressure evicts only terminal jobs and releases their
request-ID entries and request copies as well. Live work is preserved. A result
that exceeds the per-job bound leaves a small `result_too_large` failure receipt
with `operation_may_have_completed: true`; this does not roll back an executed
operation or authorize a new mutation. Progress text is limited to 4 KiB without
splitting UTF-8 code points. A queued
command expires after 15 seconds and is never executed afterwards. Replayed old
mutations must still satisfy their original state token, even after job-history
eviction. Cancellation requests have priority over queued targets. A running short
edit cannot be rolled back by cancelling its request; cancellation of an active
slice is allowed only while its native run ID is still owned by this module.

Job states are `queued`, `running`, `succeeded`, `failed`, `cancelled`. A native modal
decision leaves the job running with `waiting_for_user: true`; the API never chooses
Discard, Transfer or a warning-dialog answer for the user. Rejected operations
report structured error kinds such as `busy`, `conflict`, `stale_target`,
`invalid_scope`, `invalid_value`, `invalid_config`, `slice_not_started` and
`superseded`. Completion is based on the real native event rather than a timeout.

| Method | Parameters besides target | Result inside the completed job |
|---|---|---|
| `system.describe` | None | API/app versions, target IDs, capabilities, parameter JSON schemas |
| `scene.describe` | Optional `offset`, `limit` | Project/window label, document identity, plates, objects, instances, parts and state token |
| `settings.describe` | Optional `scope`, `filter`, `keys`, `offset`, `limit` | Native writable keys, types/ranges/enums and permitted scopes |
| `settings.get` | `scope` and explicit model/plate owner, optional page/filter | Effective/own values, inheritance, preset name and state token |
| `settings.patch` | Explicit `scope`, `state_token`, `values` and/or `unset`, owner identifiers | Actual values, changed keys and new state token |
| `slicing.start` | `state_token`, explicit current `plate_index` | Estimates and diagnostics when the native run finishes |
| `slicing.result` | Completed slice `job_id` | The result if the current inputs still match it |
| `slicing.start_many` | `state_token`, explicit `plate_ids` | Parent batch receipt and real child slice job IDs; partial failure retains child receipts |
| `jobs.cancel` | Target job ID | Cancellation acknowledgement |
| `jobs.get` | Job ID | Immediate job status; no extra job is created |

`scope` is `process`, `plate`, `object`, `volume` or `height_range`. Process writes
affect the active edited process preset. Model writes require an object with an
instance on the explicit plate. Legacy process/object calls that omit a plate use
the current plate. A canonical `plate_id` is preferred; an optional `plate_index`
must agree with it. Object, part and height-range changes affect all linked
instances, including those on other plates. Each affected inheritance chain is
validated before committing. `unset` removes overrides; process values cannot be
unset. `own_value` may be null, while `value`, `source` and `inheritance` describe the
effective value. Values use Orca serialization, for example:

```json
{
  "scope": "object",
  "object_id": "<ID-from-scene>",
  "state_token": "<token-from-settings-or-scene>",
  "values": {"wall_loops": "4", "sparse_infill_density": "30%"}
}
```

The writable process surface is derived from `Preset::print_options`, covering
ordinary supports, bridges, overhangs, speeds, accelerations, widths, arrays,
booleans and enums. Executable code, G-code templates, network credentials,
plugins, backend identity, external bed asset paths and internal preset/tool
bookkeeping are excluded. `settings.describe` is authoritative. Schema/value pages
accept a case-sensitive name/category/label filter, exact keys, offset and a limit
of 1–100 (default 80). The original fifteen keys are also included on an unfiltered
first process/object read for existing v1 clients.

Plate keys come from the native plate tab; arbitrary support parameters are not
plate overrides. Object keys combine native PrintObjectConfig and PrintRegionConfig.
Parts/modifiers accept only PrintRegionConfig. Height ranges use `[lower, upper)`
in millimetres and accept layer height plus region settings. A patch can create a
nonoverlapping range with native `extruder=0` inheritance and effective layer
height; neither required range field can be unset. Instances do not
own independent print settings. Model-material overrides, when present, remain in
the native inheritance chain and are reported on part reads. Material assignment
indices must identify existing 1-based filament slots, or zero for inheritance.

`presets.list/describe/select/patch/save_as` support process, filament and machine
collections. Selection retains native Transfer/Discard/Save decisions and checks
visibility/compatibility. Patch edits the managed copy, not its disk JSON, and
keeps vector cardinalities. Filament patch currently requires one filament slot.
Save-as validates a simple new name and uses native create-only persistence:
serialize first, atomically publish without replacement, then register/select the
new preset. Existing registry names, filesystem aliases and sidecar collisions
are rejected; failed publication creates no phantom registered preset. POSIX uses
a new hard link and Windows a move without replace. The ordinary GUI save path
keeps its prior overwrite semantics. Sync sidecar writing retains native
best-effort behavior, with create-only publication for this API path.

The input token hashes current configs, preset identity, plate input configs, model-material configs and
volume identities, mesh data, transforms, layer overrides/profiles and painted
facet timestamps, together with owner epochs. It catches manual edits and undo,
rather than counting only remote requests. Automatically resolved plate filament,
nozzle and volume mappings are derived outputs in their respective automatic
modes; manual assignments remain hashed inputs. The native slicing worker writes
these results through its cancellation-aware GUI-task bridge, so readers cannot
race option-map mutation. Hashing large meshes can cost UI time;
clients should poll jobs rather than repeatedly request full scenes.

Changing inputs invalidates an active job's claim to the result; the module does
not cancel a replacement slice started by the user. Results include native normal
mode time in seconds, filament volume in mm³, plate index, input token and native
diagnostics. Estimates are slicer predictions, not measured physical print results.

## Scene, files and model editing

`scene.describe` supplies explicit document, plate, object, instance and volume
identities, transforms, bounds, memberships and mesh revisions. A plate handle
includes the document epoch and native print identity; it is not a persistent
3MF identifier or a mutable list index. Scene mutations and native history can
invalidate handles. Read the scene again after a mutation that asks for a refresh.
`mesh_revision` hashes volume identity, source vertices/triangles and its local
transform. Facet IDs address original mesh triangles, not generated subdivisions.

The scene domain provides plate creation/selection/rename/duplicate/reorder/lock,
deletion of empty unlocked plates, explicit instance transformations, center/drop,
mirror, orient-to-face, printable flags, clone/delete/move-to-plate and native
arrangement. Distances use millimetres, rotations use degrees and scales use
positive factors. Each transformation specifies its supported coordinate frame;
the advertised schema rejects unsupported absolute/relative combinations. The
native linked-instance constraints remain: shared geometry, XY tilt, scale and
mirror cannot be made independently editable merely by addressing an instance.
`objects.make_independent` creates the independent model owner when necessary.

Object/part commands rename, clone, delete, split disconnected shells into objects
or parts, group compatible objects as multipart, and add/delete/change the type of
primitive negative volumes, parameter modifiers and support enforcers/blockers.
Grouping is not boolean union. Multipart merge requires same-plate,
single-instance objects with matching object overrides and no height ranges.
Results identify created/removed/replaced entities rather than retaining stale IDs.

Files require owner-configured JSON-array environment variables
`ORCA_AGENT_INPUT_ROOTS` and `ORCA_AGENT_OUTPUT_ROOTS`. Checks happen in the native
endpoint, not merely in MCP. Paths are absolute local paths; the API does not fetch
URLs. `InputSnapshot` checks the opened regular file, its actual resolved path,
size and change metadata, then hashes a private snapshot before native parsing.
`OutputArtifact` stages a complete result and publishes it atomically through the
validated output directory; existing output requires explicit `overwrite`.
Destination symlinks are rejected. External file writes are not undone by native
document Undo.

`model.import` accepts STL, OBJ, geometry-only 3MF, AMF and STEP/STP. A batch has at
most 16 sources and 256 MiB total, and every source is prepared before the model
commit. It excludes imported print/network presets and overrides. Native parsing
currently runs on the GUI thread, so large imports can pause the UI.
`project.open` explicitly requires `load_config: true` and retains native
unsaved-project/preset dialogs; use model import when only geometry is desired.
`project.save` writes a full 3MF copy without rebinding the current project filename
or marking its editor state saved. `model.export` writes binary STL or geometry-only
3MF. `gcode.export` copies the already valid plate toolpath and does not run
post-processing scripts. A project can contain configured executable content;
ordinary settings APIs do not add scripts and print preparation rejects configured
post-processing scripts.

## Geometry, painting and visual inspection

`geometry.validate`, `geometry.cut`, `geometry.boolean`, `geometry.simplify` and
`geometry.repair` use native algorithms on detached model data. The worker commits
on the GUI thread only if the original state, object/instance identities and mesh
revisions still match. A cancelled/conflicting computation does not replace the
live model. A successful logical edit uses one native Undo snapshot. Existing cut
correspondence/connectors and linked instances on locked plates impose native
editing restrictions; unsupported cases are rejected instead of partially edited.

Cut takes an explicit instance and world-space plane, selected halves and optional
native plug/dowel/snap connectors; connectors require two separate retained halves.
Boolean operates on two positive parts within one object. Simplification uses a
target triangle count and native quadric error metric, not an asserted millimetre
tolerance. The `preview` option computes geometry metrics without committing;
it is not a rendered image of the proposed result. Repair and replacement have an
explicit paint-retention policy. Native mesh validity, topology, open edges,
bounds and signed volume are available for inspection; they do not prove physical
fit or print strength.

`preview.render` produces a native OpenGL PNG for a plate or the whole scene,
bound to the returned state token. Supported orthographic views are isometric,
top, front, left and right; dimensions are 64–1024 pixels (default 512). The
compressed image is capped at 2 MiB and returned as `mime_type`, base64 `data`,
width and height. A plate thumbnail may omit outside, nonprintable and modifier
volumes; inspect scene placement metadata as well. Failure to obtain a native GL
context is reported, not replaced with an invented view. MCP can present the PNG
as image content.

`painting.describe/clear/set/raycast/brush` address one positive model volume and
support, seam or material annotations. Mutations and raycasts require the source
`mesh_revision`. Support/seam states are automatic/enforce/block; material states
are unpainted or an existing 1-based filament slot. Explicit facet edits replace
the whole source facet, including prior subdivisions. Region selection uses
original triangle centroids inside an inclusive AABB in volume-mesh coordinates.
Raycasts and native sphere/circle brushes use explicitly supplied mesh-space rays,
not GUI mouse input. Brush strokes currently do not subdivide triangles.
The volume limit is 500,000 source triangles; explicit facet/region sets are capped
at 10,000 triangles per edit. Annotation updates preserve model placement and use
native scene/background invalidation and Undo. Described native annotation strings
can retain subdivision information even though whole-facet edits replace it.

`history.describe/undo/redo` use native document history. Writes require an
available Prepare editor history and the original state token. A restore must
actually move the native main-stack timestamp; success returns `refresh_scene`
and invalidates document handles. History does not undo exported files, uploaded
files or physical printer effects.

## Printing and device controls

Printer operations are explicitly addressed by `backend_id` and `device_id`.
Names, UI focus and the active printer selection do not choose the destination.
`printers.list/describe/status` report available native backends and capabilities
without exposing access codes or private endpoints. BBL LAN submission currently
starts the verified single-nozzle, single-filament path, with native loaded
material/nozzle/AMS and storage checks. Storage-only BBL uploads may target a
virtual queue without that telemetry; unverified or incompatible telemetry becomes
an explicit warning requiring acknowledgement, while transport and slice checks
remain mandatory. Start options are not applied to upload-only operations. A configured generic PrintHost supports
explicit upload only; it does not claim verified model/nozzle/material telemetry
or expose its upload-and-start action through this contract. Cloud submission and
unverified multi-extruder/material mappings are not advertised as supported.

`printing.prepare` freezes a valid native slice into a private artifact and a
ten-minute plan without contacting the printer or uploading a probe file. It
requires an unchanged input token and binds window/document/plate/slice identity,
artifact hash and size, backend/device/transport, material/nozzle/options snapshot,
action and warning IDs. Up to 64 plans are retained. Post-processing scripts are
rejected rather than implicitly executed by packaging.

`printing.submit` additionally requires the owner to launch Orca with
`ORCA_AGENT_PRINTING=1`. The exact plan warning IDs must be resolved. Native code
rechecks the plan, source slice, frozen artifact, target credentials/options and
backend/library generation. The backend/library is retained for the owned worker;
UI selection cannot redirect an accepted plan. A durable claim consumes the plan
before one transmission. There is no automatic LAN-to-cloud fallback or retry of
a possibly submitted command. A negative transport code or cancellation after
claim leaves an uncertain outcome, not a reusable print plan.

`PrintJournal` is private to the current user and independent of bounded job
history/discovery files. Atomic create-only claims prevent concurrent processes
from consuming one operation twice, including after restart. The default journal
and artifact directories are under the platform application user-data cache;
`ORCA_AGENT_PRINT_JOURNAL` and `ORCA_AGENT_PRINT_ARTIFACTS` override them. All clients
controlling one user's intent history must use the same journal directory, even
when Orca processes use different project/discovery datadirs. Do not delete durable
claims to retry an unknown outcome.

`printing.status/reconcile` distinguish upload, submission acceptance and unknown
outcome. Submission completion is not proof of physical printing. Where a remote
receipt exists, reconciliation can compare its exact ID to endpoint telemetry;
the API does not infer a match from filenames or another busy task. Physical start
remains unconfirmed when the transport supplies no adequate receipt.

Bambuddy endpoints can route or queue downstream work, including after an
upload-only operation. `endpoint_mode` records explicit direct/queue/unknown context;
it does not prove the ultimate physical destination. A name such as “P1S Virtual”
is not endpoint verification. Unknown routing and native warnings remain explicit
in the plan and must not be turned into a promise that upload has no physical effect.

`printers.control` currently exposes explicitly device-wide BBL LAN pause/resume/stop
with a durable operation UUID. It can affect whichever task the endpoint currently
runs; atomic job-scoped control is not advertised. Cancelling the local submission
worker or waiting client does not stop an already started physical print. Native
warning/save dialogs require the user's decision; a model-supplied confirmation
boolean is not an authorization mechanism.

## Command-line client

`scripts/agent_control.py` uses Python's standard library. It lists authenticated
enabled windows without printing tokens, requires explicit full session/window
IDs for calls and verifies that responses match that target. It disables proxies
and redirects. Use `--directory` for a nonstandard Orca data directory.

```sh
python3 scripts/agent_control.py list
python3 scripts/agent_control.py call --session <uuid> --window <uuid> scene.describe
python3 scripts/agent_control.py call --session <uuid> --window <uuid> settings.patch --params patch.json
python3 scripts/agent_control.py wait --session <uuid> --window <uuid> <job-id>
```

`patch.json` contains the parameters shown above; the client adds `target`. Calls
wait up to 30 seconds by default and print the job ID/status if it is still active.
Use `wait` to resume observation rather than submitting a new mutation. The CLI
does not launch or modify a slicer until explicitly asked to call a method.

## Verification

The isolated Catch2 tests in `tests/agent_control` exercise real loopback sockets,
authentication, per-window credentials/descriptors, bounded admission, client
timeouts, token rotation, shutdown, queued cancellation, request deduplication,
retained-result byte limits and structured failure receipts. Native libslic3r
tests additionally cover create-only preset publication, unchanged existing files
and symlinks, and collection state after failed publication.
They are included in the normal test build and can also run without GUI libraries:

```sh
cmake -S tests/agent_control -B build/agent-control -DCMAKE_BUILD_TYPE=Release
cmake --build build/agent-control --config Release
ctest --test-dir build/agent-control -C Release --output-on-failure
```

The standalone build needs Boost headers, OpenSSL development files and a C++17
compiler; Catch2 and nlohmann JSON are vendored. Native GUI smoke checks must also
exercise two windows, two objects, read/patch/undo, manual-edit conflicts, slice
completion/cancellation, export contention, window closure and language/GUI
recreation. Compiler checks and transport tests do not substitute for those runs.

## Device selection and materials

`printers.select` selects an explicit active-backend device through the Device
tab's native selection path. Selection is shared across windows of the process;
print plans still bind an explicit backend/device rather than UI selection.
Nozzle sync is on by default and waits asynchronously for connected telemetry.
`printers.sync_nozzle` retries synchronization for that same selected device.
The automatic path supports one installed nozzle without a rack, requires a
matching printer profile, and uses native sidebar synchronization. It refuses
unknown layouts and profile switches that would discard edited presets. A
successful selection with blocked synchronization returns `nozzle_sync.state`
`blocked` and the reason; clients must check it before slicing. Waiting jobs
observe document/backend/selection changes and cancel without undoing selection.

`printers.filaments` reads reported AMS units, slots, external spools and nozzle
telemetry without replacing project materials. Global tray indices come from
`DevFilaSystem::GetTrayIndexMap`, including nonstandard AMS layouts.

The LAN start preflight checks actual completed-slice filament consumption,
including supports, painting and tower consumption, rather than the configured
project material count. Each used material is verified against nozzle hardness,
loaded material type and native compatibility rules. An explicit `ams_mapping`
contains one entry per used 1-based project filament ID and its native tray index.
`ams_tray_index` remains valid for exactly one used material. Payload arrays keep
unused project positions, preserving sparse tool IDs in the artifact. Single-nozzle
AMS multicolor is supported; dual/rack starts and manual multicolor spool changes
remain unavailable. Plans revalidate the material mapping and device snapshot at
submission. Successful transfer is not physical-print confirmation.

## Review images and reports

`preview.render` retains model plate/scene thumbnails and adds `scope: slice`
for the selected current plate's valid completed toolpaths. `scope: bed` uses
the native top plate camera and composites transparent model pixels over the
actual plate polygon with a 10 mm grid for placement review. A separate libvgcode
viewer and camera render to an offscreen framebuffer, preserving the editor's
camera, filters and GL state. Output includes slice/state identity and never
falls back to a model image for an unavailable slice. The bounded path supports
up to two million moves and a 1024-pixel, 2-MiB PNG; it uses machine coordinates.

Completed slice receipts include native time and per-material consumption,
with mass/length derived from the result's density/diameter metadata when known.
`printing.prepare` includes the same estimates plus differences from saved
selected presets and explicit plate/object/volume/height-range overrides.
Project controls use a separately labeled native option default baseline.
Reports omit executable/credential fields, mark truncation, and bind the data
to the frozen slice. A saved user preset is the baseline, not an invented
universal factory default.
