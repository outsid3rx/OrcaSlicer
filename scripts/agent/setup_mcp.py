#!/usr/bin/env python3
"""User-local Orca MCP installation. No native application or user data changes."""
from __future__ import annotations

import argparse
import asyncio
import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

NAME = "orca-slicer"
PACKAGE = "orca-slicer-mcp"
SKILL = "orca-mcp-control"
REPO = Path(__file__).resolve().parents[2]


def run(args: list[str]) -> str:
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout.strip()


def backup(path: Path) -> Path:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    target = path.with_name(path.name + ".orca-backup-" + stamp)
    if path.is_dir() and not path.is_symlink():
        shutil.copytree(path, target)
    else:
        shutil.copy2(path, target, follow_symlinks=False)
    return target


def write_config(path: Path, content: str, dry: bool) -> None:
    if path.exists() and path.read_text() == content:
        return
    print(f"{'Would write' if dry else 'Write'} {path}")
    if dry:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        print(f"Backup: {backup(path)}")
    fd, temporary = tempfile.mkstemp(prefix=".orca-config-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(content)
        os.chmod(temporary, path.stat().st_mode & 0o777 if path.exists() else 0o600)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def local_entry(entry: object) -> dict:
    if not isinstance(entry, dict):
        raise ValueError("Existing Orca MCP entry must be an object/table")
    if "url" in entry or entry.get("type", "stdio") != "stdio" or entry.get("transport", "stdio") != "stdio":
        raise ValueError("Existing Orca MCP entry is remote; refusing to replace it with a local server")
    if not isinstance(entry.get("args", []), list) or any(not isinstance(arg, str) for arg in entry.get("args", [])):
        raise ValueError("Existing Orca MCP args must be a list of strings")
    env = entry.get("env", {})
    if not isinstance(env, dict) or any(not isinstance(value, str) for value in env.values()):
        raise ValueError("Existing Orca MCP env must map names to strings")
    return entry


def codex_config(source: str, command: Path | None) -> str:
    """Keep unrelated TOML bytes and verify unrelated parsed values are identical."""
    before = tomllib.loads(source)
    servers = before.get("mcp_servers", {})
    if not isinstance(servers, dict):
        raise ValueError("Codex mcp_servers must be a table")
    if NAME not in servers and command is None:
        return source
    existing = local_entry(servers[NAME]) if NAME in servers else None
    # Resolve table headers with the TOML parser, including quoted table names.
    lines = source.splitlines(keepends=True)
    starts = []
    for index, line in enumerate(lines):
        if not re.match(r"^\s*\[(?!\[)", line):
            continue
        try:
            header = tomllib.loads(line + "\n__orca_probe__ = true\n")
        except tomllib.TOMLDecodeError:
            continue
        target = header.get("mcp_servers", {}).get(NAME) if isinstance(header.get("mcp_servers"), dict) else None
        starts.append((index, isinstance(target, dict)))
    if command is not None and existing is not None:
        # Update only a regular table's simple command assignment. Preserve args,
        # environment, timeouts, enabled flags, comments, and nested tables.
        main_tables = []
        for index, managed in starts:
            if managed:
                parsed = tomllib.loads(lines[index] + "\n__orca_probe__ = true\n")
                if parsed["mcp_servers"][NAME].get("__orca_probe__") is True:
                    main_tables.append(index)
        if len(main_tables) != 1:
            raise ValueError("Use a regular [mcp_servers.orca-slicer] table for safe updates")
        start = main_tables[0]
        end = next((index for index, _ in starts if index > start), len(lines))
        candidates = []
        for index in range(start + 1, end):
            match = re.match(r'''^(\s*(?:command|"command"|'command')\s*=\s*)(?:"(?:[^"\\]|\\.)*"|'[^']*')(\s*(?:\#.*)?)(\r?\n)?$''', lines[index])
            if match:
                candidates.append((index, match))
        if "command" in existing:
            if len(candidates) != 1:
                raise ValueError("Cannot safely update a multiline/dotted Orca MCP command")
            index, match = candidates[0]
            lines[index] = match[1] + json.dumps(str(command)) + match[2] + (match[3] or "")
        else:
            lines.insert(start + 1, f"command = {json.dumps(str(command))}\n")
        result = "".join(lines)
        expected = dict(before)
        expected["mcp_servers"] = dict(servers)
        expected["mcp_servers"][NAME] = {**existing, "command": str(command)}
        if tomllib.loads(result) != expected:
            raise ValueError("Cannot safely edit this TOML layout; unrelated values would change")
        return result
    removed = set()
    for number, (start, managed) in enumerate(starts):
        if managed:
            end = starts[number + 1][0] if number + 1 < len(starts) else len(lines)
            while end > start + 1 and (not lines[end - 1].strip() or lines[end - 1].lstrip().startswith("#")):
                end -= 1
            removed.update(range(start, end))
    result = "".join(line for i, line in enumerate(lines) if i not in removed)
    if command is not None:
        result = result.rstrip() + f'\n\n[mcp_servers.{NAME}]\ncommand = {json.dumps(str(command))}\nargs = []\n'
    after = tomllib.loads(result)
    expected_servers = dict(servers)
    expected_servers.pop(NAME, None)
    actual_servers = dict(after.get("mcp_servers", {}))
    actual_servers.pop(NAME, None)
    if expected_servers != actual_servers or {k: v for k, v in before.items() if k != "mcp_servers"} != {k: v for k, v in after.items() if k != "mcp_servers"}:
        raise ValueError("Cannot safely edit this TOML layout; unrelated values would change")
    if command is None and NAME in after.get("mcp_servers", {}):
        raise ValueError("Cannot safely remove an inline/dotted MCP table; use a regular [mcp_servers.orca-slicer] table")
    return result


def zcode_config(source: str, command: Path | None) -> str:
    data = json.loads(source) if source.strip() else {}
    if not isinstance(data, dict):
        raise ValueError("ZCode config must be a JSON object")
    mcp = data.setdefault("mcp", {})
    if not isinstance(mcp, dict):
        raise ValueError("ZCode mcp must be an object")
    servers = mcp.setdefault("servers", {})
    if not isinstance(servers, dict):
        raise ValueError("ZCode mcp.servers must be an object")
    if command is None and NAME not in servers:
        return source
    if command is None:
        servers.pop(NAME, None)
    else:
        existing = local_entry(servers[NAME]) if NAME in servers else {"args": []}
        servers[NAME] = {**existing, "type": "stdio", "command": str(command)}
    return json.dumps(data, ensure_ascii=False, indent=2) + "\n"


def config_path(home: Path, client: str) -> Path:
    return home / (".codex/config.toml" if client == "codex" else ".zcode/cli/config.json")


def configure(home: Path, clients: list[str], command: Path | None, dry: bool) -> None:
    # Validate all selected files before writing any of them.
    changes = []
    for client in clients:
        path = config_path(home, client)
        if command is None and not path.exists():
            continue
        source = path.read_text() if path.exists() else ""
        convert = codex_config if client == "codex" else zcode_config
        changes.append((path, convert(source, command)))
    for path, content in changes:
        write_config(path, content, dry)


def install_skills(home: Path, clients: list[str], source: Path, dry: bool, remove: bool = False) -> None:
    for client in clients:
        target = home / (".codex/skills" if client == "codex" else ".zcode/skills") / SKILL
        marker = target / ".orca-agent-managed"
        exists = target.exists() or target.is_symlink()
        if remove:
            if exists and marker.is_file():
                print(f"{'Would remove' if dry else 'Remove'} {target}")
                if not dry:
                    backup(target)
                    if target.is_symlink():
                        target.unlink()
                    else:
                        shutil.rmtree(target)
            elif exists:
                print(f"Keep unmanaged skill: {target}")
            continue
        if not (source / "SKILL.md").is_file():
            raise ValueError(f"Missing skill: {source}")
        print(f"{'Would copy' if dry else 'Copy'} skill to {target}")
        if dry:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if exists:
            print(f"Backup: {backup(target)}")
            if target.is_symlink():
                target.unlink()
            else:
                shutil.rmtree(target)
        shutil.copytree(source, target)
        marker.write_text("Installed by OrcaSlicer scripts/agent/install.sh\n")


async def probe(command: Path, args: list[str] | None = None, env: dict | None = None) -> None:
    """MCP protocol checks only; never call native tools or printer operations."""
    process = await asyncio.create_subprocess_exec(str(command), *(args or []), env={**os.environ, **(env or {})}, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL, limit=4 * 1024 * 1024)
    async def exchange(message: dict) -> dict:
        process.stdin.write((json.dumps(message) + "\n").encode())
        await process.stdin.drain()
        while True:
            raw = await asyncio.wait_for(process.stdout.readline(), 15)
            if not raw:
                raise ValueError("MCP exited before replying")
            response = json.loads(raw)
            if not isinstance(response, dict):
                raise ValueError("Invalid MCP response: expected an object")
            if response.get("id") == message.get("id"):
                if "error" in response:
                    raise ValueError(f"MCP error: {response['error']}")
                result = response.get("result", {})
                if not isinstance(result, dict):
                    raise ValueError("Invalid MCP result: expected an object")
                return result
    try:
        initialized = await asyncio.wait_for(exchange({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "orca-installer-doctor", "version": "1"}}}), 20)
        if "protocolVersion" not in initialized or "serverInfo" not in initialized:
            raise ValueError("Invalid MCP initialize response")
        process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        await process.stdin.drain()
        tools = (await asyncio.wait_for(exchange({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}), 20)).get("tools", [])
        if not isinstance(tools, list) or not tools or any(not isinstance(tool, dict) or not isinstance(tool.get("name"), str) or not isinstance(tool.get("inputSchema"), dict) for tool in tools):
            raise ValueError("Missing MCP tools or input schemas")
        print(f"MCP initialize and {len(tools)} tool schemas: OK. Native listener/printers were not contacted.")
    finally:
        if process.returncode is None:
            try:
                process.terminate()
            except ProcessLookupError:
                pass
        try:
            await asyncio.wait_for(process.wait(), 3)
        except TimeoutError:
            process.kill()
            await process.wait()


def package_source(root: Path, wheel: Path | None) -> Path:
    if wheel is not None:
        return wheel
    if (root / "mcp/pyproject.toml").is_file():
        return root / "mcp"
    wheels = list(root.glob("orca_slicer_mcp-*.whl"))
    if len(wheels) != 1:
        raise ValueError("Expected mcp/pyproject.toml or exactly one orca_slicer_mcp-*.whl; use --wheel explicitly")
    return wheels[0].resolve()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "update", "config", "doctor", "uninstall"])
    parser.add_argument("--client", action="append", choices=["codex", "zcode"], default=[], help="Repeat to configure both clients; omit for generic stdio JSON")
    parser.add_argument("--wheel", type=Path, help="Absolute path to a packaged MCP wheel")
    parser.add_argument("--dry-run", action="store_true", help="Plan only; do not change files, install packages, or start MCP")
    args = parser.parse_args(argv)
    try:
        if sys.platform != "darwin":
            raise ValueError("This installer currently supports macOS only")
        uv = shutil.which("uv")
        if uv is None:
            raise ValueError("uv is required: https://docs.astral.sh/uv/getting-started/installation/")
        home = Path.home()
        clients = list(dict.fromkeys(args.client))
        if args.wheel and (not args.wheel.is_absolute() or not args.wheel.is_file() or args.wheel.suffix != ".whl"):
            raise ValueError("--wheel must name an existing absolute .whl file")
        # uv tool dir is read-only and does not need network or tool execution.
        command = Path(run([uv, "tool", "dir", "--bin"])) / PACKAGE
        if not command.is_absolute():
            raise ValueError("uv returned a non-absolute tool bin directory")
        if args.action in ("install", "update"):
            source = package_source(REPO, args.wheel)
            if not source.exists():
                raise ValueError(f"Missing MCP package: {source}")
            if clients and not (REPO / "skills" / SKILL / "SKILL.md").is_file():
                raise ValueError(f"Missing skill: {REPO / 'skills' / SKILL}")
            # Preflight configs before any installation or backup.
            configure(home, clients, command, True)
            if args.dry_run:
                print(f"Would run: uv tool install --force --reinstall --python 3.12 {source}")
            else:
                run([uv, "tool", "install", "--force", "--reinstall", "--python", "3.12", str(source)])
                if not command.is_file():
                    raise ValueError(f"uv installation did not provide {command}")
            configure(home, clients, command, args.dry_run)
            install_skills(home, clients, REPO / "skills" / SKILL, args.dry_run)
        elif args.action == "config":
            if not args.dry_run and not command.is_file():
                raise ValueError(f"Executable missing: {command}; run install first")
            if clients and not (REPO / "skills" / SKILL / "SKILL.md").is_file():
                raise ValueError(f"Missing skill: {REPO / 'skills' / SKILL}")
            configure(home, clients, command, args.dry_run)
            install_skills(home, clients, REPO / "skills" / SKILL, args.dry_run)
        elif args.action == "uninstall":
            print("Uninstall removes the shared uv tool. Nonselected client configurations are retained and may need reconfiguration.")
            configure(home, clients, None, args.dry_run)
            install_skills(home, clients, REPO / "skills" / SKILL, args.dry_run, remove=True)
            if args.dry_run:
                print(f"Would run: uv tool uninstall {PACKAGE}")
            elif command.exists():
                run([uv, "tool", "uninstall", PACKAGE])
            print("Recipe libraries, print journals, and backups are retained.")
        elif args.action == "doctor":
            launch_entries = []
            for client in clients:
                path = config_path(home, client)
                if not path.is_file():
                    raise ValueError(f"Client configuration missing: {path}")
                data = tomllib.loads(path.read_text()) if client == "codex" else json.loads(path.read_text())
                # Validate the shape before accessing nested client settings.
                if client == "codex":
                    codex_config(path.read_text(), command)
                    entry = data.get("mcp_servers", {}).get(NAME)
                else:
                    zcode_config(path.read_text(), command)
                    entry = data.get("mcp", {}).get("servers", {}).get(NAME)
                if not isinstance(entry, dict) or entry.get("command") != str(command) or (client == "zcode" and entry.get("type") != "stdio"):
                    raise ValueError(f"Missing or stale {NAME} entry: {path}")
                local_entry(entry)
                launch_entries.append(entry)
                print(f"{client} configuration: OK")
            if not args.dry_run:
                if not command.is_file() or not os.access(command, os.X_OK):
                    raise ValueError(f"Executable missing or not executable: {command}")
                for entry in launch_entries or [{}]:
                    asyncio.run(probe(command, entry.get("args", []), entry.get("env", {})))
            else:
                print(f"Would check executable and MCP initialize/tools/list: {command}")
        if not clients and args.action in ("install", "update", "config"):
            print(json.dumps({"command": str(command), "args": []}, indent=2))
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError, TimeoutError) as error:
        print(f"Error: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.strip(), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
