# Adapter design

[English](./architecture.md) | [Русский](./architecture.ru.md)

A separate Python process maps the explicitly enabled native Orca agent API to
universal MCP stdio. The official MCP SDK owns initialization, framing, tool
schemas and lifecycle. The wire protocol remains v1; API revision 3 extends device selection, AMS inventory, slice previews and print reports. The original ten specialized tools remain
compatible. Fifteen additional native tool groups expose fixed action allowlists
for editing, files, previews, history, batches, presets and printers. Three recipe
tools manage local storage and guarded native application. There is no arbitrary
native method or code execution tool.

```text
MCP stdio client
  -> server.py: 28 tools, selectors, image conversion, outcome semantics
  -> client.py: private discovery, explicit target, HTTP RPC, bounded observation
  -> authenticated loopback native queue
  -> GUI controller and native scene/settings/geometry/printing domains
```

The adapter does not launch or implicitly enable Orca. A remote HTTP MCP transport
is outside this local adapter. Native parameter schemas and validation remain
authoritative, including settings inheritance and per-build capability limits.

## Identity and files

Every operation selects the full session/window UUID pair; labels, focus, PID and
list order cannot select a target. Native replies must echo the same pair. Tokens
remain in the client and are redacted from results. Descriptors are validated on
the already opened file: owner/mode on POSIX, owner/protected DACL through pywin32
on Windows. Failed or unavailable checks reject the file. Proxies and redirects
are disabled. Descriptor and payload limits apply before submission.

File roots belong to the native owner's launch environment. The native endpoint
checks actual-open input files and makes private snapshots; exports are staged
and published atomically. MCP supplies explicit local paths but cannot expand the
roots. General settings and preset edits exclude scripts and credentials.

## Mutations and jobs

All edits require an original state token and explicit request ID. The adapter
checks selector combinations, but does not duplicate native settings/geometry
validation. Structural changes require refreshing scene handles and mesh
revisions. Mixed edit groups conservatively advertise destructive capabilities;
printer groups advertise external-world effects.

Observation is bounded and never resubmits an operation. After admission-response
loss, a mutation gets conservative advice to repeat only the exact original
arguments and ID. Unknown native methods are treated as mutations for recovery.
Once a job ID is known, observe it. Native result retention is bounded; the adapter
has no permanent exactly-once execution cache. Cancellation has its own receipt
and does not imply a physical printer stopped.

Native PNG results become MCP image content without a second base64 payload in
structured JSON. Invalid/oversized preview responses fail closed.

## Printing outcomes

Preparation freezes a private artifact, state, endpoint, options and expiry; it
has no upload side effects. Submit requires native owner opt-in and warning IDs.
The native journal durably consumes the operation before network transmission.
Unknown outcomes are reconciled, never automatically resubmitted. Native success
can mean upload accepted or submission accepted; it does not prove physical
printing. Device controls explicitly affect the whole endpoint.

The adapter inspects only known durable receipt envelopes. Missing, malformed or
unknown outcomes preserve the original receipt, add `outcome_uncertain=true`,
set `isError=true`, and tell the client to reconcile the same operation. An error
flag never authorizes a new submission ID or retry. A known consumed receipt is
an observation of the previous attempt, not proof of a second transmission.

## Verification boundaries

Unit tests and official SDK stdio fixtures cover identity, validation, recovery,
image handling, annotations and durable outcome semantics. Native macOS arm64
runs cover actual editor behavior and uploads to a local receiver, including loss
of the reply after file acceptance. Physical BBL printing and Windows/Linux
runtime require their own validation.

## Printer selection and review data

The read-only printer group includes AMS inventory. A separate mutating
`orca_printer_selection` group selects a backend/device and synchronizes a
supported single nozzle. GUI device selection is shared within the Orca process;
prepared submissions still use explicit device IDs and revalidated snapshots.

Slice results expose native estimates and used project filament IDs. Preparation
returns a report bound to the frozen slice, with saved-preset differences and
explicitly labeled override baselines. PNG previews distinguish model placement
from completed G-code toolpaths (`scope: slice`). The workflow skill presents the
report before approval or after an already authorized/virtual destination send;
it never treats a transfer receipt as proof of physical printing.

## Local recipes

`recipes_contract.py` owns versioned validation, canonical RFC 8785 identities and
portable JSON. `recipes_store.py` owns immutable SQLite revisions, CAS heads,
import transactions, plans and durable application receipts. `recipes_native.py`
owns live compatibility capture, composition and one native patch per plan.
The store is shared across clients; a lifetime OS lock identifies abandoned
dispatches and operation version checks prevent stale observers overwriting
newer receipts. No observer resends native writes. No native fork changes are
required by the recipe library.

## Repository and installation

The native API, adapter and skill live in one OrcaSlicer fork. Python sources and
package tests are in `mcp/`; the sole workflow-skill source is
`skills/orca-mcp-control/` at the repository root. The skill is distributed
separately from the Python wheel and does not become an MCP runtime dependency.
Normal CMake builds do not install Python or depend on the MCP package.

`scripts/agent/install.sh` installs a source package or release wheel into a
user-local uv tool environment. Selected client entries use the installed
absolute executable path; the checkout and development `.venv` are not runtime
dependencies. Configuration changes preserve other entries and create backups.
The installer neither launches Orca nor enables agent access or printing.
Recipe storage and native print receipts remain outside the installation.
