"""Real MCP stdio session against two independently authenticated API endpoints."""

import json
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import secrets
import sys
import tempfile
import threading
import unittest
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


class NativeFixture:
    def __init__(self, directory: Path, label: str):
        self.target = {"session_id": str(uuid.uuid4()), "window_id": str(uuid.uuid4())}
        self.token = secrets.token_hex(32)
        self.label = label
        self.value = "2"
        self.state_token = "a" * 64
        self.requests = {}
        self.jobs = {}
        self.polls = {}
        self.cancelled = set()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.headers.get("Authorization") != "Bearer " + fixture.token:
                    self.send_error(401)
                    return
                method, params, request_id = payload["method"], payload["params"], payload["id"]
                reply = {"jsonrpc": "2.0", "id": request_id}
                if params["target"] != fixture.target:
                    reply["error"] = {"message": "Wrong target", "data": {"kind": "stale_target"}}
                elif method == "jobs.get":
                    job_id = params["job_id"]
                    if job_id not in fixture.jobs:
                        reply["error"] = {"message": "Unknown job", "data": {"kind": "unknown_job"}}
                    else:
                        status = fixture.jobs[job_id]
                        fixture.polls[job_id] = fixture.polls.get(job_id, 0) + 1
                        # First slice stays active until explicitly cancelled; others finish.
                        if status.pop("hold", False):
                            status["hold"] = True
                        elif status["state"] == "queued":
                            status["state"] = "succeeded"
                        reply["result"] = {k: v for k, v in status.items() if k != "hold"}
                elif request_id in fixture.requests:
                    old, job_id = fixture.requests[request_id]
                    if old != payload:
                        reply["error"] = {"message": "ID reused with different payload", "data": {"kind": "request_conflict"}}
                    else:
                        reply["result"] = {"target": fixture.target, "job_id": job_id}
                else:
                    job_id = fixture.target["window_id"] + ":" + str(len(fixture.jobs) + 1)
                    fixture.requests[request_id] = (payload, job_id)
                    status = {"target": fixture.target, "job_id": job_id, "state": "queued"}
                    if method == "scene.describe":
                        status["result"] = {"project": fixture.label, "window_title": fixture.label,
                            "plate_index": 0, "state_token": fixture.state_token,
                            "objects": [{"object_id": "97", "on_current_plate": True}]}
                    elif method == "system.describe":
                        status["result"] = {"app_version": "test", "protocol_version": 1}
                    elif method == "settings.describe":
                        status["result"] = {"wall_loops": {"type": "int"}}
                    elif method in ("settings.get", "settings.patch"):
                        if method == "settings.patch" and params["state_token"] != fixture.state_token:
                            status.update(state="failed", error={"kind": "conflict", "message": "Inputs changed"})
                        else:
                            if method == "settings.patch":
                                fixture.value = params["values"]["wall_loops"]
                                fixture.state_token = "b" * 64
                            status["result"] = {"state_token": fixture.state_token,
                                "values": {"wall_loops": {"value": fixture.value, "own_value": fixture.value}}}
                    elif method == "slicing.start":
                        status.update(state="running", hold=True, waiting_for_user=True)
                    elif method == "jobs.cancel":
                        original = fixture.jobs[params["job_id"]]
                        original.update(state="cancelled", hold=False, waiting_for_user=False)
                        status["result"] = {"cancel_requested": True}
                    elif method == "preview.render":
                        status["result"] = {"mime_type": "image/png", "data": base64.b64encode(b"\x89PNG\r\n\x1a\nfixture").decode(), "width": 64}
                    elif method == "printing.status":
                        status["result"] = {"operation_id": params["operation_id"],
                            "journal": {"intent": {"fixture": True}, "outcome": {"state": "outcome_unknown", "native_code": -1}}}
                    else:
                        status["result"] = {"method": method, "parameters": {k: v for k, v in params.items() if k != "target"}}
                    fixture.jobs[job_id] = status
                    reply["result"] = {"target": fixture.target, "job_id": job_id}
                raw = json.dumps(reply).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.path = directory / f"{self.target['session_id']}-{self.target['window_id']}.json"
        self.path.write_text(json.dumps({**self.target, "auth_token": self.token, "protocol_version": 1,
            "pid": 123, "endpoint": f"http://127.0.0.1:{self.server.server_port}/api/v1/rpc"}))
        self.path.chmod(0o600)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class StdioIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def test_official_client_two_windows_mutation_retry_conflict_and_cancellation(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            a, b = NativeFixture(directory, "A"), NativeFixture(directory, "B")
            try:
                params = StdioServerParameters(command=sys.executable,
                    args=["-m", "orca_slicer_mcp.server", "--directory", folder, "--recipes-db", str(directory / "recipes" / "library.sqlite3")])
                async with stdio_client(params) as (read, write):
                    async with ClientSession(read, write) as session:
                        initialized = await session.initialize()
                        self.assertEqual(initialized.serverInfo.name, "OrcaSlicer")
                        catalog = await session.list_tools()
                        self.assertEqual(len(catalog.tools), 28)
                        for tool in catalog.tools:
                            self.assertNotIn("ctx", tool.inputSchema["properties"])
                        patch_tool = next(t for t in catalog.tools if t.name == "orca_patch_settings")
                        self.assertFalse(patch_tool.annotations.readOnlyHint)
                        self.assertTrue(patch_tool.annotations.destructiveHint)
                        for name in ('orca_files', 'orca_objects', 'orca_presets', 'orca_history', 'orca_printing', 'orca_printer_selection'):
                            tool = next(t for t in catalog.tools if t.name == name)
                            self.assertTrue(tool.annotations.destructiveHint, name)
                        for name in ('orca_printers', 'orca_printing', 'orca_printer_selection'):
                            tool = next(t for t in catalog.tools if t.name == name)
                            self.assertTrue(tool.annotations.openWorldHint, name)
                        self.assertIn("request_id", patch_tool.inputSchema["required"])

                        async def call(name, args=None, error=False):
                            result = await session.call_tool(name, args or {})
                            self.assertEqual(bool(result.isError), error, result)
                            serialized = result.model_dump_json()
                            self.assertNotIn(a.token, serialized)
                            self.assertNotIn(b.token, serialized)
                            return result.structuredContent

                        listed = await call("orca_list_instances")
                        self.assertEqual({row["project"] for row in listed["instances"]}, {"A", "B"})
                        await call("orca_get_capabilities", {"target": a.target})
                        await call("orca_get_settings_schema", {"target": a.target})
                        scene = await call("orca_get_scene", {"target": a.target})
                        args = {"target": a.target, "scope": "object", "object_id": "97",
                            "state_token": scene["result"]["state_token"], "request_id": "edit-1",
                            "values": {"wall_loops": "4"}}
                        edited = await call("orca_patch_settings", args)
                        self.assertEqual(a.value, "4")
                        self.assertEqual(b.value, "2")
                        replay = await call("orca_patch_settings", args)
                        self.assertEqual(replay["job_id"], edited["job_id"])
                        conflicting = await call("orca_patch_settings", {**args, "request_id": "edit-2"}, error=True)
                        self.assertEqual(conflicting["error"]["kind"], "conflict")
                        self.assertEqual(a.value, "4")
                        selected = await call("orca_printer_selection", {
                            "target": b.target, "action": "printers.select",
                            "parameters": {"state_token": b.state_token, "backend_id": "bbl", "device_id": "explicit-device", "sync_nozzle": True},
                            "request_id": "select-device"})
                        self.assertEqual(selected["result"]["method"], "printers.select")
                        self.assertEqual(selected["result"]["parameters"]["device_id"], "explicit-device")
                        self.assertEqual(selected["target"], b.target)
                        inventory = await call("orca_printers", {
                            "target": b.target, "action": "printers.filaments",
                            "parameters": {"backend_id": "bbl", "device_id": "explicit-device"},
                            "request_id": "read-ams"})
                        self.assertEqual(inventory["result"]["method"], "printers.filaments")
                        denied_selection = await session.call_tool("orca_printers", {
                            "target": b.target, "action": "printers.select", "parameters": {}, "request_id": "read-cannot-select"})
                        self.assertTrue(denied_selection.isError)
                        # Structural misuse is rejected before contacting the native API.
                        bad_scope = await session.call_tool("orca_get_settings", {"target": a.target, "scope": "object"})
                        self.assertTrue(bad_scope.isError)
                        no_target = await session.call_tool("orca_get_scene", {})
                        self.assertTrue(no_target.isError)
                        bad_plate = await session.call_tool("orca_start_slicing", {"target": a.target,
                            "state_token": a.state_token, "plate_index": -1, "request_id": "slice-invalid"})
                        self.assertTrue(bad_plate.isError)
                        # Domain wrappers preserve exact parameters and full window identity.
                        domain_args = {"target": b.target, "action": "geometry.cut",
                            "parameters": {"state_token": b.state_token, "object_id": "97", "preview": True},
                            "request_id": "cut-preview"}
                        domain = await call("orca_geometry", domain_args)
                        self.assertEqual(domain["result"]["method"], "geometry.cut")
                        self.assertTrue(domain["result"]["parameters"]["preview"])
                        replay_domain = await call("orca_geometry", domain_args)
                        self.assertEqual(domain["job_id"], replay_domain["job_id"])
                        before = len(b.requests)
                        denied = await session.call_tool("orca_geometry", {**domain_args, "action": "printing.submit"})
                        self.assertTrue(denied.isError)
                        denied = await session.call_tool("orca_geometry", {**domain_args,
                            "parameters": {"target": a.target}})
                        self.assertTrue(denied.isError)
                        self.assertEqual(before, len(b.requests))
                        preview = await session.call_tool("orca_preview", {"target": a.target,
                            "action": "preview.render", "parameters": {}, "request_id": "preview-1"})
                        self.assertFalse(preview.isError)
                        self.assertEqual(len(preview.content), 2)
                        self.assertEqual(preview.content[1].type, "image")
                        self.assertNotIn("data", preview.structuredContent["result"])
                        self.assertTrue(preview.structuredContent["result"]["image_included"])
                        receipt = await call("orca_printing", {"target": a.target,
                            "action": "printing.status", "parameters": {"operation_id": "uncertain-op"},
                            "request_id": "observe-uncertain"}, error=True)
                        self.assertEqual(receipt["state"], "succeeded")
                        self.assertTrue(receipt["outcome_uncertain"])
                        self.assertEqual(receipt["result"]["journal"]["outcome"]["native_code"], -1)
                        self.assertIn("never automatically resubmit", receipt["recovery"])
                        bad_range = await session.call_tool("orca_get_settings", {"target": a.target,
                            "scope": "height_range", "object_id": "97", "height_range": [1]})
                        self.assertTrue(bad_range.isError)
                        pending = await call("orca_start_slicing", {"target": a.target,
                            "state_token": a.state_token, "plate_index": 0, "request_id": "slice-1", "wait_seconds": 0})
                        self.assertEqual(pending["state"], "running")
                        self.assertTrue(pending["waiting_for_user"])
                        job_args = {"target": a.target, "job_id": pending["job_id"], "wait_seconds": 0}
                        again = await call("orca_get_job", job_args)
                        self.assertEqual(again["state"], "running")
                        await call("orca_cancel_job", {**job_args, "request_id": "cancel-1"})
                        cancelled = await call("orca_get_job", job_args, error=True)
                        self.assertEqual(cancelled["state"], "cancelled")
            finally:
                a.close()
                b.close()
