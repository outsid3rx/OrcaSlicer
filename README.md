# OrcaSlicer for AI agents

[English](README.md) | [Русский](README.ru.md)

This is an **unofficial fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer)** for autonomous agent work with the slicer: from loading a model and configuring supports to slicing, checking the result, and submitting a job to the selected printer. The fork lives at [outsid3rx/OrcaSlicer](https://github.com/outsid3rx/OrcaSlicer). It is not an official OrcaSlicer release and is not maintained by the upstream team.

One repository contains the modified slicer, a universal MCP server, and an agent skill. No self-hosted server, recipe-library account, or separate settings application is required.

## Why a separate build is needed

MCP needs to work with the currently open project: inspect models and plates, change settings, retrieve slicing results, and select a device. The standard OrcaSlicer CLI does not provide this access to a running GUI. The official build does not include the AgentControl API used here; installing the MCP server alone does not add that API to an installed application.

The fork adds [`src/slic3r/AgentControl`](src/slic3r/AgentControl), which validates operations and performs them through Orca's own mechanisms. It is compiled with the slicer but **is enabled only when explicitly launched with `ORCA_AGENT_CONTROL=1`**. A normal launch leaves the API disabled. See [AgentControl](docs/HLSD/agent-control.md) for its architecture and access boundaries.

## What the agent can do

- Load and save models and projects, create plates, arrange and transform objects.
- Change process, plate, object, part, and height-range settings, including supports, bridges, and ironing.
- Use geometry and painting operations available in the current build, slice, and obtain real PNG previews of models and toolpaths.
- Select a printer, retrieve AMS/external spools, map used filaments to slots, and synchronize a supported single nozzle.
- Maintain a local recipe library: settings with immutable revisions, compatibility checks, composition, and JSON import/export.
- Prepare and submit print jobs under the owner's authorization rules, retaining an operation receipt.

Specific actions depend on the running build's capabilities, printer type, and telemetry. Confirmed file transfer does not prove that physical printing has started or succeeded.

## MCP server

[`mcp/`](mcp) is the `orca-slicer-mcp` Python package, using the official MCP SDK and **stdio** transport. It works with any client supporting that transport, including Codex, Claude Desktop, Cursor, Windsurf, and ZCode. Configuration methods and skill support depend on the client. No separate HTTP MCP server is started: MCP connects to Orca's private authenticated loopback API.

The server provides **28 tools**. The agent can inspect native schemas and the current build's capabilities; there is no arbitrary code execution tool. With multiple instances, the agent selects the full `session_id` / `window_id` pair, rather than the focused window or the first list item. Changes are tied to project state, and long operations return jobs that can be monitored.

The recipe library is stored outside the application and checkout, at `~/Library/Application Support/orca-slicer-mcp/recipes.sqlite3` on macOS. Multiple MCP clients can use the same database. [MCP documentation](mcp/README.md) · [Recipe contract](mcp/docs/contracts/recipes-v1/CONTRACT.md).

## Agent skill

[`skills/orca-mcp-control/SKILL.md`](skills/orca-mcp-control/SKILL.md) describes the workflow: selecting a window, checking capabilities, preparing models, mapping materials, reviewing the slice, and handling uncertain outcomes. The skill adds instructions to MCP; it does not replace the server or the separate Orca build.

Before submitting a job, the agent shows the duration, filament consumption, a slice screenshot, and settings changes, then asks for permission. Under this skill's rules, additional confirmation is unnecessary if the user already explicitly authorized this print or the selected printer's name contains `virtual`. For an already authorized or virtual submission, the same report is shown afterward. The name `virtual` identifies an owner-approved proxy/queue workflow; it does not prove that physical printing will not start. All native checks and device-access requirements remain in effect.

## Installation

The verified platform is **macOS arm64**. The current installer is macOS-only. Windows/Linux are considered in the source, but building and installing this bundle on them has not been confirmed.

1. Install an AgentControl-enabled OrcaSlicer build from this fork's releases, or build it from source.
2. Install [uv](https://docs.astral.sh/uv/getting-started/installation/). The MCP installer uses it for an isolated Python environment and does not install `uv` silently.
3. From the repository root, install MCP and obtain a universal stdio configuration for your client:

   ```sh
   ./scripts/agent/install.sh install
   ./scripts/agent/install.sh doctor
   ```

4. Launch Orca with agent control explicitly enabled and permitted model/export directories. Reconnect MCP in your client.

For automatic Codex and/or ZCode configuration, optionally add `--client codex --client zcode`; the skill is installed for the selected clients.

Without `--client`, installation sets up only the MCP server and prints its stdio configuration; it does not install the skill. For other clients, copy the bundled `skills/orca-mcp-control/` following the client’s skill-installation documentation. If the client has no native skill support, apply the instructions in `SKILL.md` as project guidance.

The installer uses a stable absolute path to the installed command. Clients do not need the checkout or its `.venv` afterward. Without `--client`, it emits a universal stdio configuration. [Full installation, Orca launch, updates, and removal](docs/agent/INSTALL.md) · [Bundle contents and releases](docs/agent/DISTRIBUTION.md).

[Releases](https://github.com/outsid3rx/OrcaSlicer/releases) belongs to this fork only. Check file availability, signing, and notarization in the specific release; this README does not announce a published release.

## Updates and data

In this fork's builds with `SLIC3R_AGENT_CONTROL=ON`, automatic upstream application update checks are disabled; the manual update command opens the fork's Releases page. This requires a new Orca build and does not appear in an older application by updating MCP alone.

Orca, MCP, and the recipe contract have independent versions. A release pins a compatible bundle; MCP checks the running Orca's API and capabilities. Updating MCP or removing its environment preserves the recipe library, operation receipts, and Orca data. A JSON dump is for exchanging recipes and does not replace a SQLite backup containing the operation journal.

The public build does not include personal Bambuddy certificates, tokens, a local database, or user models. A custom certificate placed directly inside `.app` can be lost when replacing the application. See the [installation guide](docs/agent/INSTALL.md#updates-and-local-data) for updates and certificate trust.

## Project website

[`website/`](website/README.md) contains a static project page with a description, FAQ, universal AI installation prompt, and manual installation fallback. Ready-to-serve files live in `website/dist/`: English is the default `index.html`, and Russian is `ru/index.html`, with language navigation. No build or separate backend is required.

```sh
python3 -m http.server 8080 --directory website/dist --bind 127.0.0.1
```

For GitHub Pages, select **GitHub Actions** as the source in Settings → Pages, then manually run the **Agent website** workflow. The website source stays in this repository.

## Development and verification

```sh
cd mcp
uv sync --locked
uv run --locked python -m unittest discover -s tests -v
uv run --locked python docs/contracts/recipes-v1/verify_contract.py
```

The native build uses OrcaSlicer's existing CMake workflow. `SLIC3R_AGENT_CONTROL=ON` includes the module at compile time; the `ORCA_AGENT_CONTROL=1` environment variable separately enables it at runtime. A normal CMake build does not require MCP installation. [MCP architecture](mcp/docs/architecture.md) · [Verification record and limitations](mcp/docs/verification.md).

## Upstream project and license

This fork retains the **AGPL-3.0** license and original attribution. The slicer builds on the work of the OrcaSlicer community and its predecessors; author, dependency, and license information is preserved in the [upstream README](docs/agent/UPSTREAM_README.md) and [LICENSE.txt](LICENSE.txt).

For general slicing and calibration documentation, see the [OrcaSlicer wiki](https://www.orcaslicer.com/wiki). AgentControl, MCP, the skill, and recipe-library issues belong to this fork.
