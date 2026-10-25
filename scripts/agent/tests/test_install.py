"""Offline installer checks; all writes are confined to temporary homes."""
import contextlib
import asyncio
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import sys
import tomllib
import unittest
from unittest.mock import AsyncMock, patch

SPEC = importlib.util.spec_from_file_location("setup_mcp", Path(__file__).parents[1] / "setup_mcp.py")
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.command = self.home / "bin" / setup.PACKAGE

    def test_codex_preserves_comments_and_unrelated_tables(self):
        source = '# keep header\nmodel = "x"\n[mcp_servers.other]\ncommand = "other" # keep inline\n[mcp_servers."orca-slicer"]\ncommand = "old"\n[mcp_servers."orca-slicer".env]\nA = "b"\n# project comment\n[projects."/tmp"]\ntrust_level = "trusted"\n'
        result = setup.codex_config(source, self.command)
        self.assertIn('# keep header\nmodel = "x"', result)
        self.assertIn('command = "other" # keep inline', result)
        self.assertIn('# project comment\n[projects.', result)
        self.assertEqual(tomllib.loads(result)["mcp_servers"][setup.NAME], {"command": str(self.command), "env": {"A": "b"}})
        removed = setup.codex_config(result, None)
        self.assertNotIn(setup.NAME, tomllib.loads(removed)["mcp_servers"])

    def test_unsafe_inline_toml_is_rejected(self):
        with self.assertRaises(ValueError):
            setup.codex_config('mcp_servers = {orca-slicer = {command = "old"}}\n', None)

    def test_unrelated_multiline_toml_not_corrupted(self):
        source = 'text = """\n[mcp_servers.orca-slicer]\nnot a table\n"""\n'
        try:
            result = setup.codex_config(source, self.command)
        except ValueError:
            return  # Refusing an ambiguous layout is safe.
        self.assertEqual(tomllib.loads(result)["text"], tomllib.loads(source)["text"])

    def test_zcode_strict_structure_and_preserved_settings(self):
        result = json.loads(setup.zcode_config('{"theme":"dark","mcp":{"servers":{"other":{"command":"x"}}}}', self.command))
        self.assertEqual(result["theme"], "dark")
        self.assertEqual(result["mcp"]["servers"]["other"], {"command": "x"})
        self.assertEqual(result["mcp"]["servers"][setup.NAME]["args"], [])
        self.assertEqual(result["mcp"]["servers"][setup.NAME]["type"], "stdio")
        for bad in ('[]', '{"mcp":[]}', '{"mcp":{"servers":[]}}'):
            with self.assertRaises(ValueError):
                setup.zcode_config(bad, self.command)

    def test_update_preserves_local_options_and_comments(self):
        source = '[mcp_servers.orca-slicer]\ncommand = "old" # retained\nargs = ["--recipes-db", "/private/recipes.db", "--directory", "/cache"]\nenabled = false\nstartup_timeout_sec = 45\n[mcp_servers.orca-slicer.env]\nSECRET = "private"\n'
        result = setup.codex_config(source, self.command)
        self.assertEqual(result, source.replace('command = "old"', 'command = ' + json.dumps(str(self.command))))
        entry = {"command": "old", "type": "stdio", "args": ["--directory", "/cache"], "env": {"CUSTOM": "private"}, "timeoutMs": 42000, "enabled": False}
        result = json.loads(setup.zcode_config(json.dumps({"mcp": {"servers": {setup.NAME: entry}}}), self.command))
        self.assertEqual(result["mcp"]["servers"][setup.NAME], {**entry, "command": str(self.command)})

    def test_remote_entries_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "remote"):
            setup.codex_config('[mcp_servers.orca-slicer]\nurl="https://example.com/mcp"\n', self.command)
        for entry in ({"type": "http", "url": "https://example.com"}, {"type": "sse", "command": "old"}):
            with self.assertRaisesRegex(ValueError, "remote"):
                setup.zcode_config(json.dumps({"mcp": {"servers": {setup.NAME: entry}}}), self.command)

    def test_source_and_release_wheel_selection(self):
        root = self.home / "release"
        root.mkdir()
        (root / "mcp").mkdir()
        wheel = root / "orca_slicer_mcp-0.4.0-py3-none-any.whl"
        wheel.touch()
        self.assertEqual(setup.package_source(root, None), wheel.resolve())
        (root / "orca_slicer_mcp-0.5.0-py3-none-any.whl").touch()
        with self.assertRaises(ValueError):
            setup.package_source(root, None)
        (root / "mcp/pyproject.toml").touch()
        self.assertEqual(setup.package_source(root, None), root / "mcp")
        self.assertEqual(setup.package_source(root, wheel), wheel)

    def test_config_backups_and_dry_run(self):
        path = setup.config_path(self.home, "codex")
        path.parent.mkdir(parents=True)
        path.write_text('# original\nmodel="x"\n')
        setup.configure(self.home, ["codex"], self.command, True)
        self.assertEqual(list(path.parent.glob("*.orca-backup-*")), [])
        setup.configure(self.home, ["codex"], self.command, False)
        backups = list(path.parent.glob("*.orca-backup-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), '# original\nmodel="x"\n')
        setup.configure(self.home, ["codex"], self.command, False)
        self.assertEqual(len(list(path.parent.glob("*.orca-backup-*"))), 1)

    def test_all_client_preflight_before_writes(self):
        path = setup.config_path(self.home, "zcode")
        path.parent.mkdir(parents=True)
        path.write_text("invalid json")
        with self.assertRaises(ValueError):
            setup.configure(self.home, ["codex", "zcode"], self.command, False)
        self.assertFalse(setup.config_path(self.home, "codex").exists())

    def test_skill_symlink_conflict_keeps_original(self):
        source = self.home / "source"
        source.mkdir()
        (source / "SKILL.md").write_text("new skill")
        original = self.home / "external-skill"
        original.mkdir()
        (original / "SKILL.md").write_text("old skill")
        target = self.home / ".zcode/skills" / setup.SKILL
        target.parent.mkdir(parents=True)
        target.symlink_to(original, target_is_directory=True)
        setup.install_skills(self.home, ["zcode"], source, False)
        self.assertEqual((original / "SKILL.md").read_text(), "old skill")
        self.assertFalse(target.is_symlink())
        self.assertEqual((target / "SKILL.md").read_text(), "new skill")
        self.assertTrue(next(target.parent.glob("*.orca-backup-*")).is_symlink())
        setup.install_skills(self.home, ["zcode"], source, False, remove=True)
        self.assertFalse(target.exists())
        self.assertTrue(original.exists())

    def invoke(self, args, runner=None):
        calls = []
        def fake_run(command):
            calls.append(command)
            if command[1:4] == ["tool", "dir", "--bin"]:
                return str(self.command.parent)
            if runner:
                runner(command)
            return ""
        with patch.object(setup.sys, "platform", "darwin"), patch.object(setup.shutil, "which", return_value="/fake/uv"), patch.object(setup.Path, "home", return_value=self.home), patch.object(setup, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = setup.main(args)
        return result, calls

    def test_install_uses_noneditable_absolute_source(self):
        def installed(_):
            self.command.parent.mkdir(parents=True, exist_ok=True)
            self.command.write_text("#!/bin/sh\n")
        result, calls = self.invoke(["install", "--client", "codex"], installed)
        self.assertEqual(result, 0)
        command = calls[1]
        self.assertEqual(command[1:6], ["tool", "install", "--force", "--reinstall", "--python"])
        self.assertNotIn("--editable", command)
        self.assertTrue(Path(command[-1]).is_absolute())
        data = tomllib.loads(setup.config_path(self.home, "codex").read_text())
        self.assertEqual(data["mcp_servers"][setup.NAME]["command"], str(self.command))

    def test_dry_run_no_installs_or_files(self):
        result, calls = self.invoke(["install", "--client", "codex", "--dry-run"])
        self.assertEqual(result, 0)
        self.assertEqual(len(calls), 1)
        self.assertEqual(list(self.home.iterdir()), [])

    def test_missing_install_entry_errors(self):
        result, _ = self.invoke(["install", "--client", "codex"])
        self.assertEqual(result, 1)
        self.assertFalse(setup.config_path(self.home, "codex").exists())

    def test_doctor_missing_config_entry_errors(self):
        path = setup.config_path(self.home, "codex")
        path.parent.mkdir(parents=True)
        path.write_text('model="x"\n')
        result, _ = self.invoke(["doctor", "--client", "codex"])
        self.assertEqual(result, 1)

    def test_doctor_uses_configured_arguments_and_environment(self):
        self.command.parent.mkdir()
        self.command.write_text("#!/bin/sh\n")
        self.command.chmod(0o700)
        options = ["--recipes-db", str(self.home / "custom.db")]
        environment = {"CUSTOM": "private value"}
        path = setup.config_path(self.home, "zcode")
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps({"mcp": {"servers": {setup.NAME: {"type": "stdio", "command": str(self.command), "args": options, "env": environment}}}}))
        with patch.object(setup, "probe", new_callable=AsyncMock) as probe:
            result, _ = self.invoke(["doctor", "--client", "zcode"])
        self.assertEqual(result, 0)
        probe.assert_awaited_once_with(self.command, options, environment)

    def test_uninstall_retains_nonselected_configuration(self):
        setup.configure(self.home, ["codex", "zcode"], self.command, False)
        path = setup.config_path(self.home, "zcode")
        original = path.read_bytes()
        result, _ = self.invoke(["uninstall", "--client", "codex"])
        self.assertEqual(result, 0)
        self.assertEqual(path.read_bytes(), original)

    def test_uninstall_retains_data(self):
        data = self.home / "Library/Application Support/OrcaSlicer/recipes.sqlite3"
        data.parent.mkdir(parents=True)
        data.write_bytes(b"user library")
        setup.configure(self.home, ["codex"], self.command, False)
        result, _ = self.invoke(["uninstall", "--client", "codex"])
        self.assertEqual(result, 0)
        self.assertEqual(data.read_bytes(), b"user library")
        self.assertNotIn(setup.NAME, tomllib.loads(setup.config_path(self.home, "codex").read_text()).get("mcp_servers", {}))

    def test_doctor_protocol_without_tool_calls(self):
        executable = self.home / "fake-mcp"
        log = self.home / "messages.jsonl"
        executable.write_text(f'#!{sys.executable}\n' + '''import json, sys
from pathlib import Path
for line in sys.stdin:
    request = json.loads(line)
    with Path(''' + repr(str(log)) + ''').open("a") as stream:
        stream.write(line)
    if "id" not in request:
        continue
    result = {"protocolVersion": "2024-11-05", "serverInfo": {"name": "fake", "version": "1"}} if request["method"] == "initialize" else {"tools": [{"name": "sample", "inputSchema": {"type": "object"}}]}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
''')
        executable.chmod(0o700)
        asyncio.run(setup.probe(executable))
        methods = [json.loads(line)["method"] for line in log.read_text().splitlines()]
        self.assertEqual(methods, ["initialize", "notifications/initialized", "tools/list"])

    def test_missing_uv_and_invalid_wheel(self):
        with patch.object(setup.sys, "platform", "darwin"), patch.object(setup.shutil, "which", return_value=None), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(setup.main(["install"]), 1)
        result, calls = self.invoke(["install", "--wheel", "relative.whl"])
        self.assertEqual(result, 1)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
