# Bundle contents and releases

[English](DISTRIBUTION.md) | [Русский](DISTRIBUTION.ru.md)

The slicer, MCP, and skill live in one fork, [outsid3rx/OrcaSlicer](https://github.com/outsid3rx/OrcaSlicer). The repository does not use a separate MCP checkout/submodule and requires no recipe-library backend.

| Directory | Responsibility |
|---|---|
| `src/slic3r/AgentControl/` | Native operations on the current GUI project, parameter validation, jobs, and print control |
| `mcp/` | Python stdio MCP, window selection, response adaptation, local recipes |
| `skills/orca-mcp-control/` | Single source of agent workflow instructions |
| `scripts/agent/` | Installation, client configuration, and bundle packaging |
| `.github/workflows/agent_mcp.yml` | MCP and installer checks, separate from the CMake build |
| `.github/workflows/agent_release.yml` | Release file assembly from a verified native artifact |

The MCP and skill directories are separate from upstream sources. The native module uses limited integration points in Orca. A normal CMake build does not install Python or start MCP. Upstream updates require checking these integration points and native tests; separate directories do not eliminate merge conflicts automatically.

## Versions and artifacts

The OrcaSlicer application version, wire protocol/API revision, MCP Python package, and recipe format are independent. A bundle tag such as `agent-v0.4.0` pins selected compatible versions rather than making every version number equal. The running Orca reports its supported methods and schemas; a published release number does not replace that check.

A release contains a native DMG, MCP wheel/sdist, skill ZIP, and installation archive. `release-manifest.json` links the bundle to its source and versions; `SHA256SUMS` checks file integrity. A checksum does not replace verification of provenance, application signing, or notarization status.

`agent_release.yml` is run manually. Supply the ID of a successful `Build all` run from the same repository at the same commit, and the exact artifact name containing one DMG. The workflow checks provenance, validates the DMG, bundled standard CA, and dependencies, runs tests, and builds archives. By default, the result remains an Actions artifact.

To create a **draft**, run the workflow on an existing `agent-v<MCP version>` tag with `create_draft=true`. The workflow does not perform a separate publication step. Dirty sources are allowed only for a local test bundle, marked `publishable=false`, and rejected by `--require-clean`. A clean bundle includes an archive of the corresponding source.

Local verification and packaging on macOS, from the checkout root:

```sh
uv run --no-project --python 3.12 scripts/agent/verify_dmg.py --dmg /absolute/path/OrcaSlicer.dmg \
  --output /absolute/path/native-verification.json --source-commit "$(git rev-parse HEAD)"
uv run --no-project --python 3.12 scripts/agent/package_release.py --dmg /absolute/path/OrcaSlicer.dmg \
  --native-report /absolute/path/native-verification.json \
  --output /absolute/path/new-empty-release-directory --require-clean
```

DMG verification records integrity, platform, AgentControl presence, dependencies, CA, and signing status. It does not launch the application or prove API, AMS, or physical printing behavior; `runtime_tested=false` preserves that boundary. It does not confirm notarization.

When explicitly requested, the GitHub workflow prepares a **draft**. Publication, Developer ID signing, and notarization do not follow from the existence of the workflow: the specific release states its published status and supported platforms. The first locally verified platform is macOS arm64; source support for other platforms is not evidence of successful installation or operation there.

## Installation and updates

The MCP installer uses an isolated `uv tool install` environment and a stable absolute command path. Clients do not depend on the checkout after installation. When Codex/ZCode are selected through `--client`, the skill is copied from its single source into their directories. Configuration backups are kept next to modified files; unrelated MCP entries are preserved.

The stdio server can be used with clients such as Codex, Claude Desktop, Cursor, Windsurf, and ZCode, depending on their capabilities. Running the installer without `--client` emits generic configuration. Optional `--client codex` and `--client zcode` flags automate those clients' configuration and skill installation. The current installer supports macOS only.

Without `--client`, installation sets up only the MCP server and prints its stdio configuration; it does not install the skill. For other clients, copy the bundled `skills/orca-mcp-control/` following the client’s skill-installation documentation. If the client has no native skill support, apply the instructions in `SKILL.md` as project guidance.

The application is installed/replaced separately from the DMG. The MCP installer does not modify `/Applications`, Keychain, Orca profiles, or TLS trust. The owner enables control and printing through Orca's environment; configuring a client does not silently enable them.

Builds with `SLIC3R_AGENT_CONTROL=ON` use a manual update channel through the fork's Releases: automatic upstream checks are disabled, and the manual command opens the fork's page. Installing the official upstream build replaces the application with one lacking AgentControl. The normal updater behavior of a particular installed application depends on its build: updating MCP alone does not switch an older build to the fork's channel.

The recipe SQLite database, operation receipts, and native print journals live outside the installation. Removing the Python package does not delete them. A recipe dump contains no receipts and cannot restore an unfinished operation's state.

## Public and local contents

Artifacts exclude `.venv`, auth descriptors/tokens, the SQLite database, user models, personal Bambuddy CA certificates, and local working builds. Public Orca uses the standard upstream certificate bundle. A certificate manually added to `.app` resources does not become part of the public release and may be lost when replacing the application; automatic migration of this trust is not promised.

The AGPL license and corresponding source are available with the bundle. Original attribution to OrcaSlicer and its predecessors is preserved in the [upstream README](UPSTREAM_README.md).
