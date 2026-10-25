import asyncio
import json
import os
from pathlib import Path
import tempfile
import unittest
import uuid
from unittest.mock import patch

import httpx

from orca_slicer_mcp.client import Client, ClientError, Target, descriptor


class ClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.target = Target(session_id=str(uuid.uuid4()), window_id=str(uuid.uuid4()))
        self.token = "ab" * 32
        self.selected = {**self.target.model_dump(), "pid": 123, "protocol_version": 1,
                         "auth_token": self.token, "endpoint": "http://127.0.0.1:54321/api/v1/rpc"}
        self.path = self.directory / f"{self.target.session_id}-{self.target.window_id}.json"
        self.write_descriptor()
        self.requests = []
        self.responder = None
        self.client = Client(self.directory, transport=httpx.MockTransport(self.handle))
        self.job_id = self.target.window_id + ":1"

    def write_descriptor(self, value=None):
        self.path.write_text(json.dumps(self.selected if value is None else value))
        self.path.chmod(0o600)

    def handle(self, request):
        payload = json.loads(request.content)
        self.requests.append(payload)
        self.assertEqual(request.headers["authorization"], "Bearer " + self.token)
        self.assertEqual(str(request.url), self.selected["endpoint"])
        return self.responder(request, payload)

    def reply(self, payload, result):
        return httpx.Response(200, json={"jsonrpc": "2.0", "id": payload["id"],
                                        "result": {"target": self.target.model_dump(), **result}})

    def respond_job(self, request, payload):
        if payload["method"] == "jobs.get":
            return self.reply(payload, {"job_id": self.job_id, "state": "succeeded", "result": {"ok": True}})
        return self.reply(payload, {"job_id": self.job_id})

    async def asyncTearDown(self):
        await self.client.close()
        self.temp.cleanup()

    async def test_explicit_target_and_success(self):
        self.responder = self.respond_job
        status = await self.client.call(self.target, "settings.patch", {"state_token": "original"}, request_id="edit-1")
        self.assertEqual(status["state"], "succeeded")
        self.assertEqual(status["request_id"], "edit-1")
        self.assertEqual([r["method"] for r in self.requests], ["settings.patch", "jobs.get"])
        self.assertTrue(all(r["params"]["target"] == self.target.model_dump() for r in self.requests))

    async def test_other_window_never_falls_back_to_only_available_window(self):
        other = Target(session_id=self.target.session_id, window_id=str(uuid.uuid4()))
        status = await self.client.call(other, "scene.describe", {})
        self.assertEqual(status["error"]["kind"], "instance_unavailable")
        self.assertEqual(self.requests, [])

    async def test_mismatched_descriptor_rejected_before_network(self):
        self.write_descriptor({**self.selected, "window_id": str(uuid.uuid4())})
        status = await self.client.call(self.target, "scene.describe", {})
        self.assertEqual(status["error"]["kind"], "invalid_descriptor")
        self.assertEqual(self.requests, [])

    async def test_response_target_mismatch_does_not_poll(self):
        self.responder = lambda req, p: self.reply(p, {"job_id": self.job_id, "target": {
            **self.target.model_dump(), "window_id": str(uuid.uuid4())}})
        status = await self.client.call(self.target, "settings.patch", {}, request_id="one")
        self.assertEqual(status["state"], "unknown")
        self.assertEqual(status["error"]["kind"], "stale_target")
        self.assertEqual(len(self.requests), 1)

    async def test_pending_wait_returns_handle_without_resubmission(self):
        self.responder = lambda req, p: self.reply(p, {"job_id": self.job_id, **(
            {"state": "running", "progress_percent": 31} if p["method"] == "jobs.get" else {})})
        status = await self.client.call(self.target, "slicing.start", {}, request_id="slice", wait_seconds=0)
        self.assertEqual((status["state"], status["job_id"]), ("running", self.job_id))
        await self.client.job(self.target, self.job_id, 0)
        self.assertEqual([r["method"] for r in self.requests], ["slicing.start", "jobs.get", "jobs.get"])

    async def test_waiting_for_user_returns_immediately(self):
        self.responder = lambda req, p: self.reply(p, {"job_id": self.job_id, **(
            {"state": "running", "waiting_for_user": True} if p["method"] == "jobs.get" else {})})
        status = await asyncio.wait_for(self.client.call(self.target, "slicing.start", {}, wait_seconds=30), 1)
        self.assertTrue(status["waiting_for_user"])
        self.assertEqual(len(self.requests), 2)

    async def test_lost_admission_response_exact_retry_preserves_envelope(self):
        count = 0
        def responder(req, p):
            nonlocal count
            if p["method"] != "jobs.get":
                count += 1
                if count == 1:
                    raise httpx.ReadTimeout("connection lost after native admission", request=req)
            return self.respond_job(req, p)
        self.responder = responder
        params = {"state_token": "original-token", "values": {"wall_loops": "4"}}
        first = await self.client.call(self.target, "settings.patch", params, request_id="stable-id")
        self.assertEqual(first["state"], "unknown")
        self.assertNotIn("job_id", first)
        self.assertEqual(first["request_id"], "stable-id")
        self.assertIn("exact", first["recovery"])
        self.assertEqual(len(self.requests), 1)  # No hidden retry.
        second = await self.client.call(self.target, "settings.patch", params, request_id=first["request_id"])
        self.assertEqual(second["state"], "succeeded")
        self.assertEqual(self.requests[0], self.requests[1])

    async def test_expanded_and_unknown_mutations_never_get_read_retry_advice(self):
        def responder(req, payload):
            raise httpx.ReadTimeout("admission response was lost", request=req)
        self.responder = responder
        for method in ("geometry.cut", "printing.submit", "slicing.start_many", "future.mutation"):
            with self.subTest(method=method):
                result = await self.client.call(self.target, method, {}, request_id=method)
                self.assertEqual(result["state"], "unknown")
                self.assertIn("exact same tool arguments", result["recovery"])
        self.assertEqual(len(self.requests), 4)

    async def test_lost_read_response_allows_safe_read_retry(self):
        def responder(req, payload):
            raise httpx.ReadTimeout("read admission response was lost", request=req)
        self.responder = responder
        status = await self.client.call(self.target, "scene.describe", {})
        self.assertEqual(status["state"], "unknown")
        self.assertNotIn("job_id", status)
        self.assertIn("Retry this read tool", status["recovery"])
        self.assertNotIn("exact same tool arguments", status["recovery"])
        self.assertEqual([r["method"] for r in self.requests], ["scene.describe"])

    async def test_poll_failure_keeps_known_job_for_observation(self):
        def responder(req, p):
            if p["method"] == "jobs.get":
                raise httpx.ReadError("lost poll", request=req)
            return self.reply(p, {"job_id": self.job_id})
        self.responder = responder
        status = await self.client.call(self.target, "settings.patch", {}, request_id="edit")
        self.assertEqual(status["state"], "unknown")
        self.assertEqual(status["job_id"], self.job_id)
        self.assertIn("orca_get_job", status["recovery"])

    async def test_terminal_native_failures_and_cancelled_jobs_are_preserved(self):
        for state, kind in (("failed", "conflict"), ("failed", "queue_timeout"), ("cancelled", "cancelled")):
            with self.subTest(state=state, kind=kind):
                self.requests.clear()
                self.responder = lambda req, p: self.reply(p, {"job_id": self.job_id, **(
                    {"state": state, "error": {"kind": kind, "message": "native diagnostic"}}
                    if p["method"] == "jobs.get" else {})})
                status = await self.client.call(self.target, "settings.patch", {}, request_id="edit")
                self.assertEqual(status["state"], state)
                self.assertEqual(status["error"]["kind"], kind)
                self.assertEqual(len(self.requests), 2)

    async def test_expired_job_observation_does_not_recreate_operation(self):
        self.responder = lambda req, p: httpx.Response(200, json={"jsonrpc": "2.0", "id": p["id"],
            "error": {"code": -32004, "data": {"kind": "unknown_job"}, "message": "Retained result expired"}})
        status = await self.client.job(self.target, self.job_id, 0)
        self.assertEqual(status["error"]["kind"], "unknown_job")
        self.assertEqual([r["method"] for r in self.requests], ["jobs.get"])

    async def test_conflict_survives_without_refresh_or_retry(self):
        self.responder = lambda req, p: httpx.Response(200, json={"jsonrpc": "2.0", "id": p["id"],
            "error": {"code": -32000, "data": {"kind": "conflict"}, "message": "Read settings again"}})
        status = await self.client.call(self.target, "settings.patch", {"state_token": "old"}, request_id="edit")
        self.assertEqual(status["state"], "failed")
        self.assertEqual(status["error"]["kind"], "conflict")
        self.assertEqual(len(self.requests), 1)

    async def test_cancel_ack_does_not_claim_original_job_cancelled(self):
        self.responder = self.respond_job
        status = await self.client.call(self.target, "jobs.cancel", {"job_id": self.target.window_id + ":99"}, request_id="cancel")
        self.assertEqual(status["job_id"], self.job_id)
        self.assertEqual(self.requests[0]["params"]["job_id"], self.target.window_id + ":99")
        self.assertEqual(self.requests[1]["params"]["job_id"], self.job_id)

    async def test_wrong_window_job_rejected_without_network(self):
        status = await self.client.job(self.target, str(uuid.uuid4()) + ":1", 0)
        self.assertEqual(status["error"]["kind"], "invalid_job")
        self.assertEqual(self.requests, [])

    async def test_tokens_are_redacted_recursively_from_results_and_errors(self):
        self.responder = lambda req, p: self.reply(p, {"job_id": self.job_id, **({"state": "succeeded", "result": {
            "nested": [{"auth_token": self.token, "Authorization": "Bearer " + self.token,
                        "label": "prefix " + self.token}], self.token: "key-value"}}
            if p["method"] == "jobs.get" else {})})
        status = await self.client.call(self.target, "scene.describe", {})
        text = json.dumps(status)
        self.assertNotIn(self.token, text)
        self.assertNotIn("Authorization", text)
        self.assertIn("[redacted]", text)
        self.responder = lambda req, p: httpx.Response(200, json={"jsonrpc": "2.0", "id": p["id"],
            "error": {"data": {"kind": self.token}, "message": "secret " + self.token}})
        status = await self.client.call(self.target, "scene.describe", {})
        self.assertNotIn(self.token, json.dumps(status))

    async def test_discovery_never_exposes_endpoint_or_credentials(self):
        self.responder = self.respond_job
        value = await self.client.instances()
        self.assertEqual(len(value["instances"]), 1)
        self.assertEqual(value["instances"][0]["target"], self.target.model_dump())
        self.assertNotIn(self.token, json.dumps(value))
        self.assertNotIn(self.selected["endpoint"], json.dumps(value))

    async def test_redirect_is_not_followed(self):
        self.responder = lambda req, p: httpx.Response(307, headers={"Location": "http://example.invalid/steal"})
        status = await self.client.call(self.target, "settings.patch", {}, request_id="edit")
        self.assertEqual(status["state"], "unknown")
        self.assertEqual(status["error"]["kind"], "http_error")
        self.assertEqual(len(self.requests), 1)

    async def test_rotated_credentials_are_not_retried_or_disclosed(self):
        self.responder = lambda req, p: httpx.Response(401, content="private diagnostic " + self.token)
        status = await self.client.call(self.target, "settings.patch", {}, request_id="edit")
        self.assertEqual(status["error"]["kind"], "unauthorized")
        self.assertNotIn(self.token, json.dumps(status))
        self.assertEqual(len(self.requests), 1)

    async def test_request_and_response_limits(self):
        self.responder = self.respond_job
        status = await self.client.call(self.target, "settings.patch", {"values": {"x": "a" * 65536}}, request_id="large")
        self.assertEqual(status["error"]["kind"], "request_too_large")
        self.assertEqual(self.requests, [])
        self.responder = lambda req, p: httpx.Response(200, content=b" " * (4 * 1024 * 1024 + 1))
        status = await self.client.call(self.target, "scene.describe", {})
        self.assertEqual(status["error"]["kind"], "invalid_response")

    async def test_duplicate_response_keys_are_not_accepted(self):
        self.responder = lambda req, p: httpx.Response(200, content=(
            '{"jsonrpc":"2.0","id":"fixed","result":{},"result":{}}'))
        status = await self.client.call(self.target, "scene.describe", {}, request_id="fixed")
        self.assertEqual(status["error"]["kind"], "invalid_response")

    async def test_proxy_environment_is_disabled_at_client_construction(self):
        with patch("orca_slicer_mcp.client.httpx.AsyncClient", wraps=httpx.AsyncClient) as factory:
            with patch.dict(os.environ, {"HTTP_PROXY": "http://example.invalid:9000"}):
                client = Client(self.directory, transport=httpx.MockTransport(self.handle))
                await client.close()
            self.assertIs(factory.call_args.kwargs["trust_env"], False)
            self.assertIs(factory.call_args.kwargs["follow_redirects"], False)

    async def test_malformed_rpc_and_unknown_job_states_are_rejected(self):
        for response in ({"jsonrpc": "2.0", "id": "wrong", "result": {}},
                         {"jsonrpc": "2.0", "id": "fixed", "result": {}, "error": {}},
                         {"jsonrpc": "2.0", "id": "fixed", "error": []}):
            with self.subTest(response=response):
                self.responder = lambda req, p: httpx.Response(200, json=response)
                status = await self.client.call(self.target, "scene.describe", {}, request_id="fixed")
                self.assertEqual(status["error"]["kind"], "invalid_response")
        self.responder = lambda req, p: self.reply(p, {"job_id": self.job_id, "state": "invented"})
        status = await self.client.job(self.target, self.job_id, 0)
        self.assertEqual(status["error"]["kind"], "invalid_response")

    async def test_hostile_descriptor_values_are_rejected(self):
        cases = [{"endpoint": endpoint} for endpoint in (
            "https://127.0.0.1:123/api/v1/rpc", "http://localhost:123/api/v1/rpc",
            "http://127.0.0.1:123/api/v1/rpc?x=1", "http://127.0.0.1:123/api/v1/rpc#x",
            "http://user@127.0.0.1:123/api/v1/rpc", "http://127.0.0.1:65536/api/v1/rpc",
            "http://192.168.1.1:123/api/v1/rpc", "file:///tmp/sensitive")]
        cases += [{"protocol_version": True}, {"pid": True}, {"auth_token": "bad"}]
        for changes in cases:
            with self.subTest(changes=changes):
                self.write_descriptor({**self.selected, **changes})
                with self.assertRaises(ClientError) as caught:
                    descriptor(self.path, self.target)
                self.assertEqual(caught.exception.kind, "invalid_descriptor")
                self.assertNotIn(self.token, str(caught.exception))

    async def test_duplicate_keys_and_oversized_descriptors_are_rejected(self):
        for raw in ('{"pid":1,"pid":2}', '"' + "x" * 17000 + '"'):
            self.path.write_text(raw)
            with self.assertRaises(ClientError) as caught:
                descriptor(self.path, self.target)
            self.assertEqual(caught.exception.kind, "invalid_descriptor")

    @unittest.skipUnless(os.name == "posix", "POSIX private-file invariants")
    async def test_nonprivate_symlink_and_fifo_descriptors_are_rejected(self):
        self.path.chmod(0o644)
        with self.assertRaises(ClientError):
            descriptor(self.path, self.target)
        self.path.unlink()
        other = self.directory / "private.json"
        other.write_text(json.dumps(self.selected))
        other.chmod(0o600)
        self.path.symlink_to(other)
        with self.assertRaises(ClientError):
            descriptor(self.path, self.target)
        self.path.unlink()
        os.mkfifo(self.path, 0o600)
        with self.assertRaises(ClientError):
            descriptor(self.path, self.target)


if __name__ == "__main__":
    unittest.main()
