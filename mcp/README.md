# OrcaSlicer MCP

[English](./README.md) | [Русский](./README.ru.md)

Universal local stdio adapter for the custom OrcaSlicer agent-control API. Uses
the official Python MCP SDK, Python 3.11+, and no runtime dependency on the fork
checkout. It opens no HTTP listener. Any MCP client supporting stdio can use it.

## Install and connect

This package lives in the [agent-control OrcaSlicer fork](../README.md), alongside
its native API and the workflow skill. A compatible custom Orca build is required;
the official application does not contain this API.

From the repository root, install to a stable user-local uv tool environment:

```sh
./scripts/agent/install.sh install
./scripts/agent/install.sh doctor
```

An existing [uv installation](https://docs.astral.sh/uv/getting-started/installation/)
is required. The default command installs the MCP server and prints generic
stdio configuration for any compatible MCP client. It does not install the
workflow skill into a client catalog. To configure supported clients and install
the skill automatically, add `--client codex`, `--client zcode`, or both. For other
clients, transfer the bundled skill or workflow instructions according to that
client's documentation. The English `SKILL.md` remains the single authoritative
workflow-skill source. Use `--wheel /absolute/path/package.whl` to install a
release wheel.
Installation does not require the checkout at runtime. See
[installation and update instructions](../docs/agent/INSTALL.md).

Generic MCP configuration (use the absolute installed path reported by setup):

```json
{
  "mcpServers": {
    "orca-slicer": {
      "command": "/Users/your-name/.local/bin/orca-slicer-mcp",
      "args": []
    }
  }
}
```

For development only, run `cd mcp` from the repository root, then `uv sync --locked`
and `.venv/bin/orca-slicer-mcp --help`. Client installation uses a stable tool
path rather than this checkout's `.venv`.

No endpoint URLs or auth tokens belong in the client configuration or prompts.
For a custom Orca `--datadir`, add `--directory` followed by the absolute path to
its `cache/agent-control` directory in `args`.

## Enable Orca

Launch the custom build explicitly. Normal launches leave the API disabled.

```sh
ORCA_AGENT_CONTROL=1 \
ORCA_AGENT_INPUT_ROOTS='["/absolute/path/models"]' \
ORCA_AGENT_OUTPUT_ROOTS='["/absolute/path/exports"]' \
/Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer
```

File roots are owner-controlled JSON arrays of existing absolute directories.
They are configured in Orca's environment, not supplied as tool arguments. File
operations fail closed without them. For multiple processes, enable Orca's
multiple-instance preference. Discovery uses private descriptors in the native
data directory. Closed/stale endpoints are ignored; there is no target fallback.

In current builds, `ORCA_AGENT_CONTROL=1` also enables print submissions and device controls. Older builds may additionally require `ORCA_AGENT_PRINTING=1` at launch; inspect `printers.list` for `transmission_enabled`. Preparing a plan has no upload side effects.

## Tools and workflow

The original ten tools remain available: `orca_list_instances`,
`orca_get_capabilities`, `orca_get_scene`, `orca_get_settings_schema`,
`orca_get_settings`, `orca_patch_settings`, `orca_start_slicing`,
`orca_get_slice_result`, `orca_get_job`, `orca_cancel_job`.

Expanded tools group fixed allowlists of native actions:

| Tool | Operations |
|---|---|
| `orca_files` | Import, 3MF open/save, STL/3MF/G-code export |
| `orca_plates` | Create/select/rename/duplicate/delete/reorder/lock |
| `orca_instances` | Transform, move to plate, center/drop, clone/delete, orient, printable |
| `orca_objects` | Clone, make independent, rename, printable, delete, split, multipart merge |
| `orca_parts` | Part transforms/split, modifier primitives and types |
| `orca_arrange` | Native arrangement of explicit instances |
| `orca_geometry` | Validate, cut/connectors, boolean, simplify, repair |
| `orca_painting` | Support/seam/material facets, regions, raycast and brush |
| `orca_presets` | List/describe/select/patch/save as new user preset |
| `orca_preview` | Native PNG as MCP image content |
| `orca_history` | Native history inspection, Undo/Redo |
| `orca_slice_plates` | Explicit plate set with sequential child jobs |
| `orca_printers` | Printer/backend status, AMS slots, external spools and nozzle inventory |
| `orca_printer_selection` | Explicit Device-tab selection with default nozzle synchronization |
| `orca_printing` | Frozen plans, submit, reconcile, device controls |
| `orca_recipes` | Local recipe discovery, checks, export and operation observation |
| `orca_recipe_library` | Immutable revisions, forks, archive and planned import |
| `orca_recipe_apply` | Composition plans and single native application |

1. Choose the complete session/window UUID pair according to the user's project
   intent. Ask if ambiguous; focus, port, PID and list order never select a target.
2. Read capabilities and scene. `method_params_schemas` from the running build is
   authoritative for domain `parameters`. Unsupported methods fail without a
   UI automation or another-device fallback.
3. Edits require a current `state_token` and unique `request_id`. Grouped tools put
   the token inside `parameters`. Settings use Orca strings (`"4"`, `"30%"`).
   Process, plate, object, volume and height-range scopes follow native key
   availability. Object overrides affect every instance. Scripts, credentials and
   network settings are excluded from general settings/preset edits.
4. Refresh scene after structural edits or Undo. Plate handles are scoped to the
   document epoch. Geometry/painting require mesh revisions. Native linked-instance
   rules apply; make a copy independent when needed. Geometry supports a computed
   `preview:true` result before committing.
5. Observe async jobs with `orca_get_job`. Timeouts or MCP cancellation stop
   observation, not the native operation. Use `orca_cancel_job` explicitly.
   Native dialogs require the user's response.
6. Slice one current plate or an explicit set. Batches finish on the last requested
   plate and return child receipts; failures retain partial receipts in error
   details. Estimates are predictions.

A conflict requires rereading and reconsidering the edit. Never refresh the token
and retry automatically. A lost admission response may mean execution occurred:
retry only the **exact original arguments and request ID**. Once a job ID is
known, observe it. Request history is bounded, not a permanent exactly-once
execution guarantee. Unknown methods get conservative mutation retry advice.

## Device and material selection

Use `orca_printer_selection` with `printers.select`, an explicit backend/device
and a scene state token. It waits for telemetry and synchronizes a supported
single installed nozzle by default. Inspect `nozzle_sync.state`; `blocked` reports
a profile/telemetry/layout problem even when device selection succeeded.
Selection is shared by windows in the same Orca process. Refresh state and slice
after nozzle synchronization.

Read `printers.filaments` through `orca_printers` for AMS/external material slots.
Use reported `tray_index` values. A print's `options.ams_mapping` is an array of
`{"filament_id": 2, "tray_index": 0}` entries, with 1-based project material IDs
covering exactly the materials used by the completed slice. Unused project
materials do not block printing. Single-nozzle AMS multicolor is supported;
dual-nozzle/rack starts and manual spool-change multicolor are not verified.

The optional [orca-mcp-control skill](../skills/orca-mcp-control/SKILL.md) includes
launch commands and the owner's confirmation rule: ask immediately before sending,
except for an explicitly approved job or a selected printer name containing
`virtual` (case-insensitive). The exception is not a compatibility-check bypass
or proof that no downstream hardware can start. Native environment opt-in alone
is not per-job approval.

## Printing

Name the backend and device explicitly. `printing.prepare` freezes the validated
slice, endpoint, options, artifact hash, input state and expiry. Echo warning IDs
according to the submit schema. Use owner-declared `endpoint_mode` (queue, direct,
unknown), never infer it from a name. An upload-only operation to a queue/virtual
endpoint may cause downstream printing.

Submit consumes a durable cross-process claim before network I/O. A lost response
or crash means reconcile the original operation; do not resubmit with a new ID.
Native success can mean upload accepted or submission accepted; it does not prove
physical printing. Controls explicitly state device-wide scope, affecting whatever
job the endpoint currently runs. Cancelling a local worker does not stop a printer.

The native catalog reports real backend limits. Generic PrintHost upload can be
available while start remains unavailable without verified model/nozzle/material
telemetry. Do not bypass native preflight failures.

## Verification

The client validates private descriptors, UUID/file identity, exact loopback
endpoints, target echoes, response limits and deadlines. Proxies and redirects are
disabled; credentials are redacted. Windows ownership/protected ACL checks use
`pywin32` and reject unavailable or failed checks.

```sh
uv run --locked python -m unittest discover -s tests -v
```

Tests include official stdio sessions against independent authenticated local
fixtures. Native GUI, real printers and Windows/Linux runtime require separate
validation; adapter tests do not establish them.

## Local recipe library (0.4.0)

Recipes bundle up to 256 serialized settings for one scope. Combine up to 16
pinned revisions in one plan and apply them with one native settings patch.
The library lives in private SQLite storage shared by MCP clients; no backend
or separate application is needed.

- `orca_recipes`: describe action schemas, list/get/check revisions, export a
  portable JSON dump, observe a saved application operation.
- `orca_recipe_library`: create/revise/fork/archive recipes; prepare and commit
  an import. Pass the dump as raw JSON text so duplicate keys can be rejected.
- `orca_recipe_apply`: plan a composition, inspect its diff/warnings, apply once.

Start with `orca_recipes(action="describe", parameters={"tool":
"orca_recipe_apply", "action":"plan"})` to obtain exact parameter schemas.
Capture the full window UUID pair, owner IDs and current state token. Create
recipes from explicit values, review `check`, then pin recipe ID/revision/hash
when planning. Conflicting values require explicit resolution. Apply the exact
plan hash and acknowledge its warnings; poll `operation.get` using its receipt.
Never resend an uncertain application: a consumed plan returns its original
operation, including across MCP restarts and different request IDs.

On macOS the default database is
`~/Library/Application Support/orca-slicer-mcp/recipes.sqlite3`.
Override it with **MCP server** option `--recipes-db /absolute/private/path/library.sqlite3`;
use the same path in clients that should share recipes. This is not an Orca
launch argument. Storage is initialized automatically; newer unsupported storage
versions fail closed. Semantic recipe migration requires explicit supported
rules; v1 does not invent replacements for changed Orca settings.

See [contract v1](docs/contracts/recipes-v1/CONTRACT.md) for limits, portability,
compatibility and durability guarantees. Declared material/model compatibility
cannot currently be established from the native API and blocks application;
nozzle diameter and scalar layer height can be inspected. Empty constraints do
not certify physical print quality. Multi-command workflows are outside v1.
