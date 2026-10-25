# Verification record

[English](./verification.md) | [Русский](./verification.ru.md)

Verified 2026-10-25 on macOS arm64, Python 3.12.13 and MCP SDK 1.30.0.
Adapter 0.4.0; OrcaSlicer 2.5.0-dev, wire v1 / API revision 3.

- 87 adapter tests pass, including official MCP stdio against independently
  authenticated windows, retry/conflict/cancel rules, image conversion, uncertain
  print outcomes and printer-selection routing/annotations.
- Five native AgentControl CTest suites pass: commands, loopback transport,
  durable print journal, actual-open file access and print material mapping.
  The material tests contain 28 assertions covering sparse project IDs, zero
  consumption map keys, exact mapping coverage, unused entries, external-spool
  sentinels and native mapping payload arrays.
- Release application builds. Changed/new module files pass the repository's
  clang-tidy checks without the PCH. Diff whitespace checks pass.
- Isolated real GUI/MCP checks verify pre-slice preview rejection, model preview,
  placement on the plate polygon with a 10 mm grid, actual G-code toolpath PNGs,
  state preservation, native time/grams/metres, and frozen-plan settings reports.
  Reports verify saved-process and object-override differences. No file is sent.
- The signed standalone bundle repeats the report/preview scenario. Its 70 Mach-O
  files have no external non-system library dependencies. Its only runtime
  symlink points within the bundle. Certificate resources match upstream and
  exclude the personal Bambuddy CA; deep strict code-sign verification passes.
- An isolated local Moonraker simulator verifies explicit device selection and
  inventory identity. Missing nozzle telemetry produces a bounded not-ready
  result and retains the selected device, without a modal dialog or upload.
- Configured Codex and ZCode commands each initialize and expose 28 MCP tools.
  The workflow skill validates and is installed for both clients. New sessions
  are required for existing clients to reload a running server/skill catalog.

Earlier revision-2 checks also covered broad editing/geometry/preset operations,
real local test-receiver uploads, duplicate submission prevention and loss of the
reply after acceptance. These are regression context, not new physical evidence.

No real printer transmission or physical print/control was performed. Automatic
nozzle sync with actual printer telemetry, real AMS multicolor submission, cloud
start, Windows/Linux builds/runtime and notarization are not established by these
checks. Start supports one installed nozzle without a rack and explicit AMS
mapping; dual/rack start and manual multicolor spool changes remain unavailable.
Native model/nozzle/material rules and durable submission revalidation remain in
force. Successful file transfer is not physical-print confirmation.

Recipe 0.4.0 verification: 34 portable contract checks pass. SQLite tests cover
immutable revisions, CAS heads, atomic import, private storage, concurrent claims,
restart recovery and backup preservation. Official MCP stdio tests cover shared
clients, 106-key pagination/composition, conflicts, import/export and lost native
admission replies without resubmission. Numeric readback tests cover strict scalar
normalization without accepting rounding or parser prefixes.

An isolated signed Orca GUI applies two recipes (wall loops and two support
settings) to one object via one patch, verifies completion and replays the plan
under a new request ID without changing the state token. No printer is contacted.
A built wheel initializes in a separate environment with its bundled schema;
configured Codex/ZCode commands expose all 28 tools and share the same library.
Windows ACL/locking code and Linux runtime were not executed; semantic migration
rules for future setting changes do not exist in v1.

## Single-repository distribution

The adapter source now lives under `mcp/` and the skill has a single source under
`skills/orca-mcp-control/` at the fork root. User-local installation uses the uv
tool executable rather than a checkout `.venv`. This layout change does not
extend the physical/runtime claims above.

The macOS setup helper has offline tests for configuration preservation, backups,
skill installation, dry-run behavior, data-preserving removal and MCP protocol
probing against a fixture. Doctor probes stdio initialization and tool schemas
only; starting MCP may initialize its local library. It does not test Orca/native
reachability, printer telemetry or physical printing. Public release publication
and notarization require separate evidence.

Distribution checks on macOS arm64: all 87 MCP tests and 22 installer/release
boundary tests pass, plus 34 portable contract checks and shell syntax validation.
The modified native application rebuilds successfully. Its staged DMG verifies,
contains the repository CA bundle, has 70 Mach-O files without external runtime
library dependencies, and passes deep strict ad-hoc signature verification.
Developer ID notarization is not established. The installed application and its
user-added CA were not replaced.

The installer was executed against the real Codex and ZCode configurations:
unrelated parsed settings are identical, backup files exist, both use the stable
user-local command and both initialize with 28 tools. The installed wheel also
controls an isolated newly built Orca window, applies three recipe keys, and
reuses the same plan without changing state twice. No printer was contacted.
Workflow YAML parses locally; GitHub Actions/draft creation have not been run.
