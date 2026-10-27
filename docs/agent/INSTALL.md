# Install OrcaSlicer for agents

[English](INSTALL.md) | [Русский](INSTALL.ru.md)

The bundle contains a separate OrcaSlicer build with AgentControl, an MCP server, and a skill. The native build is required to work with an open project; official OrcaSlicer does not provide the API this MCP uses. The current installer supports macOS only, and the verified bundle platform is macOS arm64. This documentation does not confirm a published release, Developer ID signing, or notarization: check the specific file in the [fork's Releases](https://github.com/outsid3rx/OrcaSlicer-Agent-control/releases).

## OrcaSlicer

Install the application from the fork's DMG into `/Applications`, or use your own build with `SLIC3R_AGENT_CONTROL=ON`. Save projects and close Orca before replacing the application.

Agent access is disabled on a normal launch. To enable it, launch the binary directly with **existing absolute** model and export directories:

```sh
ORCA_AGENT_CONTROL=1 \
ORCA_AGENT_INPUT_ROOTS='["/Users/your-name/Models"]' \
ORCA_AGENT_OUTPUT_ROOTS='["/Users/your-name/Exports"]' \
/Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer
```

Replace the paths with your own; MCP cannot expand these permitted directories. Without them, file operations are rejected. If Orca is already running without AgentControl, close it and relaunch with the required environment: connecting MCP cannot change an existing process's environment variables.

In current builds, `ORCA_AGENT_CONTROL=1` also enables native job submission and device control; older builds may additionally require `ORCA_AGENT_PRINTING=1` at launch. Check `printers.list` for `transmission_enabled`; it does not replace the authorization rules for a particular job. For multiple processes, enable Orca's multiple-instance setting. The agent must select the full window UUID pair corresponding to the project.

## MCP and skill

Install [uv](https://docs.astral.sh/uv/getting-started/installation/). The script uses your installed `uv` for Python and the MCP environment; it does not download a third-party installer through a hidden `curl` command.

From the checkout root, install MCP and obtain a universal stdio configuration:

```sh
./scripts/agent/install.sh install
```

Add the emitted configuration to your MCP client. Codex, Claude Desktop, Cursor, Windsurf, and ZCode are examples of clients that can launch stdio MCP servers; their exact configuration formats and skill support vary. For automatic Codex and/or ZCode setup, use the optional flags:

```sh
./scripts/agent/install.sh install --client codex --client zcode
```

You may pass `--client` once or several times. The installer:

- Installs MCP using `uv tool install` and records the absolute command path from `uv tool dir --bin`, usually `~/.local/bin/orca-slicer-mcp`.
- Changes only the selected client's `orca-slicer` entry, preserves other settings, and creates an adjacent `*.orca-backup-<timestamp>` backup before changing a file.
- When the corresponding `--client` is selected, installs the skill into `~/.codex/skills/orca-mcp-control` and/or `~/.zcode/skills/orca-mcp-control`.
- Preserves recipes, operation receipts, print journals, Orca profiles, and user certificates.

The release installation archive already includes a wheel next to the scripts: after unpacking, the same `./scripts/agent/install.sh install` command finds it automatically. First check the archive against `SHA256SUMS` from a trusted release.

To install a separately downloaded wheel:

```sh
./scripts/agent/install.sh install \
  --wheel /absolute/path/orca_slicer_mcp-0.4.0-py3-none-any.whl \
  --client codex --client zcode
```

Add `--dry-run` to preview without writing files, installing packages, or making network requests. This requires Python 3.12 to be available already: a missing runtime is deliberately not downloaded. MCP can also be installed without `--client`: the script emits a universal stdio JSON configuration for manual addition to another client.

```sh
./scripts/agent/install.sh install --dry-run --client codex
./scripts/agent/install.sh config --client zcode
./scripts/agent/install.sh doctor
```

`config` points the selected clients at the command already installed. `doctor` checks MCP startup, protocol initialization, and the tool list; it does not invoke native operations or send data to a printer. Starting the server may initialize the local recipe library. A successful `doctor` does not prove that an Orca window is enabled. After installation or an update, reconnect MCP/restart the client: a running server process will not replace itself.

Without `--client`, installation sets up only the MCP server and prints its stdio configuration; it does not install the skill. For other clients, copy the bundled `skills/orca-mcp-control/` following the client’s skill-installation documentation. If the client has no native skill support, apply the instructions in `SKILL.md` as project guidance.

The skill is optional for the MCP protocol but contains the agent's workflow rules, including slice review and submission authorization. Its source lives in [`skills/orca-mcp-control`](../../skills/orca-mcp-control), separately from the Python package.

## Other MCP clients

Any stdio client can launch the installed command. Use the absolute path reported by the installer; the surrounding configuration field names depend on your client:

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

An endpoint URL and auth token are not needed in the client configuration or agent messages. They are read from Orca's private descriptors.

For Orca with a custom `--datadir`, add the **MCP arguments** `--directory /absolute/datadir/cache/agent-control`. To use a different shared recipe database, add `--recipes-db /absolute/private/directory/recipes.sqlite3`; its directory must be private. These are MCP options, whereas `ORCA_AGENT_CONTROL` and permitted file directories belong to the Orca process.

## Workflow and print submission

After connecting, the agent calls `orca_list_instances`, selects a window by the full `session_id` / `window_id`, then reads capabilities and the scene. If the choice is ambiguous, it asks the user. It must not select by focus or list order.

The skill requires the final report to be assembled before submission: printer and action, duration, filament consumption, a real slice screenshot, and differences from the saved preset. If the user has not already authorized printing and the printer name does not contain `virtual`, the agent shows the report, asks for permission, and waits. With prior authorization or `virtual`, the same report is shown after submission.

An owner typically uses a device with `virtual` in its name for a proxy/queue, such as Bambuddy. This skill rule removes only the additional permission request; it does not bypass compatibility checks, authorize unsolicited printing, or prove the absence of a physical start. A lost receipt requires monitoring/reconciling the original operation, not automatically resubmitting it.

## Updates and local data

Update Orca with a build **from this fork**. The official upstream build lacks AgentControl, so replacing the application with it removes the API even if MCP remains installed. In the current source, a build with `SLIC3R_AGENT_CONTROL=ON` disables automatic upstream application update checks, and the manual check opens this fork's Releases. This change requires updating the application itself; installing a new MCP does not change an older installed Orca's updater. Do not treat an older build's normal update offer as proof of compatibility with this fork.

Update MCP from the selected checkout or a verified wheel:

```sh
./scripts/agent/install.sh update --client codex --client zcode
```

Clients do not need the checkout after installation. Before starting work, MCP checks Orca's protocol and capabilities; the MCP version need not match the application version. Bundle releases list compatible versions and file checksums.

On macOS, the default recipe library is `~/Library/Application Support/orca-slicer-mcp/recipes.sqlite3`. Updating/removing MCP does not erase this database or the completed-operation journal. A JSON dump is for exchanging recipes; it contains no operation receipts and does not replace a SQLite backup. Do not replace the live database with a dump or an old copy while an operation is unfinished.

The public DMG does not contain a personal Bambuddy CA. Keep the original custom certificate outside `.app`. If it was manually added to application resources, replacing `.app` removes that change; this guide does not promise automatic migration. Adding trust must remain an explicit local action, and disabling TLS verification does not replace a certificate.

To remove the installed MCP and its selected client entries:

```sh
./scripts/agent/install.sh uninstall --client codex --client zcode
```

This removes the shared MCP environment: clients omitted from `--client` retain their entries but cannot launch it until reinstallation. User data and backups remain. Removing MCP does not stop a job already accepted by the slicer or printer.

## Source and verification

For MCP development, from the repository root:

```sh
cd mcp
uv sync --locked
uv run --locked python -m unittest discover -s tests -v
uv run --locked python docs/contracts/recipes-v1/verify_contract.py
```

Normal client installations should not point at this `.venv`. See [AgentControl](../HLSD/agent-control.md) for native architecture, opt-in, and access boundaries, and the [MCP verification record](../../mcp/docs/verification.md) for checked scenarios and limitations. Adapter tests and successful slicing do not prove the physical quality or safety of a particular print.
