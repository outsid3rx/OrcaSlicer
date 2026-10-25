"""Official MCP stdio integration of recipe storage and guarded native application.

All native endpoints are authenticated localhost fixtures. No real slicer, project,
printer or global recipe library is used by this suite.
"""

import asyncio
from contextlib import asynccontextmanager
import copy
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import socket
import sys
import tempfile
import threading
import unittest
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def operation_id():
    return str(uuid.uuid4())


def ref(record):
    payload = record["payload"]
    return {"recipe_id": payload["recipe_id"], "revision": payload["revision"], "sha256": record["sha256"]}


class RecipeNativeFixture:
    """Small native queue with real native-shaped schema and intentionally paged data."""

    def __init__(self, directory, label):
        self.target = {"session_id": operation_id(), "window_id": operation_id()}
        self.token = secrets.token_hex(32)
        self.label = label
        self.state_token = "a" * 64
        self.values = {"wall_loops": "2", "enable_support": "0"}
        self.values.update({f"fixture_setting_{i:03d}": "2" for i in range(104)})
        self.own = {key: None for key in self.values}
        self.definitions = {key: {"type": 2, "min": 0, "max": 100, "enum_values": [], "enum_labels": [],
            "label": key, "description": "Fixture writable setting", "category": "Fixture", "units": "",
            "nullable": False, "scopes": ["process", "object", "volume", "height_range"],
            "writable": True, "object_override": True} for key in self.values}
        self.definitions["enable_support"]["type"] = 8
        self.definitions["enable_support"]["max"] = 1
        self.requests = {}
        self.jobs = {}
        self.patch_requests = 0
        self.patch_executions = 0
        self.get_pages = []
        self.schema_pages = []
        self.drop_next_patch_reply = False
        self.lock = threading.Lock()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def send_json(self, value):
                raw = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_POST(self):
                if self.headers.get("Authorization") != "Bearer " + fixture.token:
                    self.send_error(401)
                    return
                payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                request_id, method, params = payload["id"], payload["method"], payload["params"]
                reply = {"jsonrpc": "2.0", "id": request_id}
                with fixture.lock:
                    if params.get("target") != fixture.target:
                        reply["error"] = {"message": "Wrong window", "data": {"kind": "stale_target"}}
                    elif method == "jobs.get":
                        job = fixture.jobs.get(params.get("job_id"))
                        if job is None:
                            reply["error"] = {"message": "Unknown retained job", "data": {"kind": "unknown_job"}}
                        else:
                            reply["result"] = copy.deepcopy(job)
                    elif request_id in fixture.requests:
                        old, job_id = fixture.requests[request_id]
                        if old != payload:
                            reply["error"] = {"message": "Request identity conflict", "data": {"kind": "request_conflict"}}
                        else:
                            reply["result"] = {"target": fixture.target, "job_id": job_id}
                    else:
                        job_id = fixture.target["window_id"] + ":" + str(len(fixture.jobs) + 1)
                        fixture.requests[request_id] = (copy.deepcopy(payload), job_id)
                        job = {"target": fixture.target, "job_id": job_id, "state": "succeeded"}
                        try:
                            job["result"] = fixture.execute(method, params)
                        except ValueError as error:
                            job.update(state="failed", error={"kind": str(error), "message": "Fixture request rejected"})
                        fixture.jobs[job_id] = job
                        reply["result"] = {"target": fixture.target, "job_id": job_id}
                        if method == "settings.patch" and fixture.drop_next_patch_reply:
                            fixture.drop_next_patch_reply = False
                            self.close_connection = True
                            self.connection.shutdown(socket.SHUT_RDWR)
                            self.connection.close()
                            return
                self.send_json(reply)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.path = directory / f"{self.target['session_id']}-{self.target['window_id']}.json"
        self.path.write_text(json.dumps({**self.target, "auth_token": self.token, "protocol_version": 1,
            "pid": 123, "endpoint": f"http://127.0.0.1:{self.server.server_port}/api/v1/rpc"}))
        self.path.chmod(0o600)

    def page(self, params):
        keys = sorted(params.get("keys", self.values))
        if len(params.get("keys", [])) > 100:
            raise ValueError("invalid_params")
        offset, limit = params.get("offset", 0), min(params.get("limit", 80), 100)
        chosen = keys[offset:offset + limit]
        return chosen, {"offset": offset, "limit": limit, "total": len(keys),
            "next_offset": offset + limit if offset + limit < len(keys) else None}

    def result_values(self, keys):
        return {key: {"value": self.values[key], "own_value": self.own[key],
            "overridden": self.own[key] is not None,
            "source": "object" if self.own[key] is not None else "process",
            "inheritance": [{"scope": "process", "present": True},
                            {"scope": "object", "present": self.own[key] is not None}]} for key in keys}

    def execute(self, method, params):
        if method == "system.describe":
            return {"protocol_version": 1, "api_revision": 3, "app_version": "fixture-1",
                "target": self.target, "capabilities": ["scene.describe", "settings.describe", "settings.get", "settings.patch", "jobs.get"],
                "scopes": ["process", "plate", "object", "volume", "height_range"],
                "method_params_schemas": {"settings.patch": {"type": "object"}}}
        if method == "scene.describe":
            return {"target": self.target, "project": self.label, "window_title": self.label,
                "state_token": self.state_token, "current_plate_id": "plate:1:0", "plate_index": 0,
                "document_epoch": 1, "counts": {"objects": 1, "instances": 1, "volumes": 1, "plates": 1},
                "plates": [{"id": "plate:1:0", "index": 0}], "objects": [{"object_id": "97", "id": "97", "instance_ids": ["98"], "on_current_plate": True}],
                "instances": [{"id": "98", "object_id": "97", "plate_id": "plate:1:0"}], "volumes": [], "has_more": False}
        if method == "settings.describe":
            keys, pagination = self.page(params)
            self.schema_pages.append(keys)
            if any(key not in self.definitions for key in keys):
                raise ValueError("forbidden_setting")
            return {"settings": {key: copy.deepcopy(self.definitions[key]) for key in keys},
                "scope": params.get("scope", "process"), "state_token": self.state_token, "pagination": pagination,
                "serialization": "Use native serialized strings; booleans are 0 or 1"}
        if method in ("settings.get", "settings.patch"):
            if params.get("object_id", "97") != "97" or params.get("plate_id", "plate:1:0") != "plate:1:0":
                raise ValueError("stale_target")
            if method == "settings.patch":
                self.patch_requests += 1
                if params.get("state_token") != self.state_token:
                    raise ValueError("conflict")
                incoming = params.get("values", {})
                if len(incoming) > 256 or any(key not in self.values for key in incoming):
                    raise ValueError("invalid_params")
                self.patch_executions += 1
                self.values.update(incoming)
                self.own.update(incoming)
                self.state_token = hashlib.sha256(str(self.patch_executions).encode()).hexdigest()
                # Native patch receipts are NOT a full read for larger recipes.
                keys = sorted(incoming)[:100]
                pagination = {"offset": 0, "limit": 100, "total": len(incoming),
                    "next_offset": 100 if len(incoming) > 100 else None}
            else:
                keys, pagination = self.page(params)
                self.get_pages.append((self.patch_executions, keys))
            if any(key not in self.values for key in keys):
                raise ValueError("forbidden_setting")
            return {"scope": params.get("scope", "object"), "object_id": params.get("object_id"),
                "volume_id": params.get("volume_id"), "range": params.get("range"),
                "plate_id": "plate:1:0", "plate_index": 0, "preset_name": "Fixture preset",
                "state_token": self.state_token, "values": self.result_values(keys), "pagination": pagination}
        raise ValueError("unsupported_fixture_method")

    def binding(self):
        return {"target": self.target, "scope": "object", "plate_id": "plate:1:0",
            "object_id": "97", "state_token": self.state_token}

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class RecipeStdioIntegrationTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.a = RecipeNativeFixture(self.directory, "Recipe A")
        self.b = RecipeNativeFixture(self.directory, "Protected B")
        self.database = self.directory / "library.sqlite3"

    def tearDown(self):
        self.a.close()
        self.b.close()
        self.temp.cleanup()

    @asynccontextmanager
    async def session(self, database=None):
        params = StdioServerParameters(command=sys.executable, args=["-m", "orca_slicer_mcp.server",
            "--directory", str(self.directory), "--recipes-db", str(database or self.database)])
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                yield session

    async def call(self, session, tool, action, parameters, *, request_id=None, error=False):
        args = {"action": action, "parameters": parameters}
        if request_id is not None:
            args["request_id"] = request_id
        result = await session.call_tool(tool, args)
        self.assertEqual(bool(result.isError), error, result)
        self.assertIsNotNone(result.structuredContent, result)
        serialized = result.model_dump_json()
        self.assertNotIn(self.a.token, serialized)
        self.assertNotIn(self.b.token, serialized)
        envelope = result.structuredContent
        self.assertEqual(envelope["contract_version"], 1)
        if error and not envelope.get("ok"):
            return envelope
        self.assertTrue(envelope["ok"], envelope)
        return envelope["data"]

    async def terminal(self, session, op):
        for _ in range(30):
            if op["status"] not in ("dispatching", "observing"):
                return op
            await asyncio.sleep(0.05)
            op = await self.call(session, "orca_recipes", "operation.get",
                {"operation_id": op["operation_id"], "wait_seconds": 0.2})
        self.fail("Recipe operation never reached a bounded terminal receipt")

    def draft(self, values, name="Fixture recipe"):
        return {"name": name, "description": "Local integration fixture, not a calibrated print recipe",
            "scope": "object", "values": values, "compatibility": {}, "evidence": []}

    async def create(self, session, values, name="Fixture recipe"):
        return await self.call(session, "orca_recipe_library", "create",
            {"draft": self.draft(values, name), "capture_binding": self.a.binding()}, request_id=operation_id())

    async def plan(self, session, refs, **parameters):
        return await self.call(session, "orca_recipe_apply", "plan",
            {"recipes": refs, "binding": self.a.binding(), **parameters}, request_id=operation_id())

    @staticmethod
    def submit_parameters(plan):
        return {"plan_id": plan["plan_id"], "plan_sha256": plan["plan_sha256"],
            "accepted_warning_ids": [w["id"] for w in plan["warnings"] if w["acknowledgement_required"]]}

    async def test_recipe_lifecycle_paged_apply_and_portable_dump(self):
        values = {key: "1" if key == "enable_support" else "4" if key == "wall_loops" else "3"
                  for key in self.a.values}
        protected_values = copy.deepcopy(self.b.values)
        async with self.session() as session:
            catalog = {tool.name: tool for tool in (await session.list_tools()).tools}
            self.assertEqual(len(catalog), 28)
            for name in ("orca_recipes", "orca_recipe_library", "orca_recipe_apply"):
                self.assertFalse(catalog[name].annotations.openWorldHint)
                self.assertNotIn("ctx", catalog[name].inputSchema["properties"])
            self.assertTrue(catalog["orca_recipes"].annotations.readOnlyHint)
            self.assertFalse(catalog["orca_recipe_library"].annotations.readOnlyHint)
            self.assertFalse(catalog["orca_recipe_library"].annotations.destructiveHint)
            self.assertTrue(catalog["orca_recipe_apply"].annotations.destructiveHint)
            described = await self.call(session, "orca_recipes", "describe", {})
            self.assertEqual(described["library_generation"], 0)
            created = await self.create(session, values)
            record = created["record"]
            first = ref(record)
            self.assertEqual(record["payload"]["values"], values)
            self.assertEqual(set(record["payload"]["native_contract"]["definitions"]), set(values))
            got = await self.call(session, "orca_recipes", "get", {"ref": first})
            self.assertEqual(got["record"], record)
            listed = await self.call(session, "orca_recipes", "list", {})
            self.assertEqual([item["ref"] for item in listed["items"]], [first])
            plan = await self.plan(session, [first])
            self.assertEqual(plan["values"], values)
            self.assertEqual(plan["binding"]["target"], self.a.target)
            self.assertEqual(len(plan["diff"]), len(values))
            submit = self.submit_parameters(plan)
            request_id = operation_id()
            op = await self.call(session, "orca_recipe_apply", "apply", submit, request_id=request_id)
            op = await self.terminal(session, op)
            self.assertEqual(op["status"], "succeeded", op)
            self.assertEqual(op["verification"], "matched")
            self.assertEqual(self.a.values, values)
            self.assertEqual(self.a.own, values)
            self.assertEqual(self.b.values, protected_values)
            self.assertEqual(self.a.patch_executions, 1)
            self.assertEqual(self.b.patch_executions, 0)
            verified = set().union(*(set(keys) for generation, keys in self.a.get_pages if generation == 1))
            self.assertEqual(verified, set(values), "Every changed key must be reread beyond the first native page")
            self.assertTrue(all(len(keys) <= 100 for _, keys in self.a.get_pages))
            same = await self.call(session, "orca_recipe_apply", "apply", submit, request_id=request_id)
            alias = await self.call(session, "orca_recipe_apply", "apply", submit, request_id=operation_id())
            self.assertEqual(same["operation_id"], op["operation_id"])
            self.assertEqual(alias["operation_id"], op["operation_id"])
            self.assertEqual(self.a.patch_executions, 1)
            mismatch = await self.call(session, "orca_recipe_apply", "apply", {**submit, "plan_sha256": "0" * 64},
                request_id=operation_id(), error=True)
            self.assertFalse(mismatch["ok"])
            self.assertEqual(self.a.patch_executions, 1)
            updated_values = {**values, "wall_loops": "7"}
            revised = await self.call(session, "orca_recipe_library", "revise", {
                "recipe_id": first["recipe_id"], "expected_head_version": created["head_version"],
                "draft": self.draft(updated_values), "capture_binding": self.a.binding()}, request_id=operation_id())
            self.assertEqual(revised["record"]["payload"]["revision"], 2)
            self.assertEqual(revised["record"]["payload"]["parent"], first)
            stale = await self.call(session, "orca_recipe_library", "revise", {
                "recipe_id": first["recipe_id"], "expected_head_version": created["head_version"],
                "draft": self.draft(values), "capture_binding": self.a.binding()}, request_id=operation_id(), error=True)
            self.assertFalse(stale["ok"])
            forked = await self.call(session, "orca_recipe_library", "fork", {"source": first,
                "draft": self.draft({"wall_loops": "9"}, "Forked fixture"), "capture_binding": self.a.binding()}, request_id=operation_id())
            fork_ref = ref(forked["record"])
            self.assertNotEqual(fork_ref["recipe_id"], first["recipe_id"])
            self.assertEqual(forked["record"]["payload"]["derived_from"], first)
            conflict = await self.call(session, "orca_recipe_apply", "plan", {
                "recipes": [first, fork_ref], "binding": self.a.binding()}, request_id=operation_id(), error=True)
            self.assertEqual(conflict["error"]["kind"], "composition_conflict")
            resolved = await self.plan(session, [first, fork_ref], resolutions=[{"key": "wall_loops", "chosen_value": "9"}])
            self.assertEqual(resolved["values"]["wall_loops"], "9")
            await self.call(session, "orca_recipe_library", "archive", {"recipe_id": fork_ref["recipe_id"],
                "expected_head_version": forked["head_version"], "archived": True}, request_id=operation_id())
            active = await self.call(session, "orca_recipes", "list", {})
            self.assertEqual(len(active["items"]), 1)
            exported = await self.call(session, "orca_recipes", "export", {"selection": "all"})
            self.assertEqual(len(exported["recipes"]), 3)
            self.assertEqual(len(exported["heads"]), 2)
            original_by_ref = {(r["payload"]["recipe_id"], r["payload"]["revision"]): r for r in exported["recipes"]}
            async with self.session(self.directory / "imported.sqlite3") as imported:
                duplicate_keys = '{"format":"orca.recipe-library","format":"orca.recipe-library","format_version":1,"exported_at":"2026-10-25T00:00:00Z","recipes":[],"heads":[]}'
                rejected = await self.call(imported, "orca_recipe_library", "import.prepare", {"dump": duplicate_keys}, request_id=operation_id(), error=True)
                self.assertFalse(rejected["ok"])
                still_empty = await self.call(imported, "orca_recipes", "list", {})
                self.assertEqual(still_empty["items"], [])
                import_plan = await self.call(imported, "orca_recipe_library", "import.prepare", {"dump": json.dumps(exported)}, request_id=operation_id())
                receipt = await self.call(imported, "orca_recipe_library", "import.commit", {
                    "import_plan_id": import_plan["import_plan_id"], "import_plan_sha256": import_plan["import_plan_sha256"],
                    "accepted_warning_ids": [w["id"] for w in import_plan["warnings"] if w["acknowledgement_required"]]}, request_id=operation_id())
                self.assertEqual(len(receipt["inserted"]), 3)
                roundtrip = await self.call(imported, "orca_recipes", "export", {"selection": "all"})
                self.assertEqual({(r["payload"]["recipe_id"], r["payload"]["revision"]): r for r in roundtrip["recipes"]}, original_by_ref)
                self.assertEqual(roundtrip["heads"], exported["heads"])
                self.assertEqual(self.a.patch_executions, 1, "Dump import never applies settings")

    async def test_lost_native_admission_reply_is_durable_and_never_resubmitted(self):
        async with self.session() as session:
            created = await self.create(session, {"wall_loops": "8"})
            recipe_ref = ref(created["record"])
            plan = await self.plan(session, [recipe_ref])
            submit = self.submit_parameters(plan)
            request_id = operation_id()
            self.a.drop_next_patch_reply = True
            uncertain = await self.call(session, "orca_recipe_apply", "apply", submit, request_id=request_id, error=True)
            self.assertEqual(uncertain["status"], "outcome_unknown")
            self.assertIsNone(uncertain["native_job_id"])
            self.assertNotEqual(uncertain["recovery"]["action"], "none")
            self.assertEqual(self.a.values["wall_loops"], "8")
            self.assertEqual(self.a.patch_executions, 1)
            for retry_id in (request_id, operation_id()):
                replay = await self.call(session, "orca_recipe_apply", "apply", submit, request_id=retry_id, error=True)
                self.assertEqual(replay["operation_id"], uncertain["operation_id"])
                self.assertEqual(replay["status"], "outcome_unknown")
            self.assertEqual(self.a.patch_requests, 1)
        # A new MCP process must preserve the consumed intent without inventing
        # an admission retry; the native effect has already happened.
        async with self.session() as session:
            observed = await self.call(session, "orca_recipes", "operation.get", {
                "operation_id": uncertain["operation_id"], "wait_seconds": 0}, error=True)
            self.assertEqual(observed["status"], "outcome_unknown")
            replay = await self.call(session, "orca_recipe_apply", "apply", submit, request_id=operation_id(), error=True)
            self.assertEqual(replay["operation_id"], uncertain["operation_id"])
            self.assertEqual(self.a.patch_requests, 1)
            self.assertEqual(self.a.patch_executions, 1)
            self.assertEqual(self.b.patch_executions, 0)


if __name__ == "__main__":
    unittest.main()
