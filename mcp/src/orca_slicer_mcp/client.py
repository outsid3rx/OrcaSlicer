"""Authenticated local API client. Descriptors and credentials never leave this layer."""

import asyncio
import json
import os
from pathlib import Path
import re
import stat
import sys
import time
import uuid

import httpx
from pydantic import BaseModel, ConfigDict, field_validator

from .permissions import private_file

READ_METHODS = frozenset({"system.describe", "scene.describe", "settings.describe", "settings.get",
    "slicing.result", "jobs.get", "geometry.validate", "painting.describe", "painting.raycast", "history.describe",
    "preview.render", "presets.list", "presets.describe", "printers.list", "printers.describe",
    "printers.status", "printers.filaments", "printing.status", "printing.reconcile"})


class Target(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    session_id: str
    window_id: str

    @field_validator("session_id", "window_id")
    @classmethod
    def canonical_uuid(cls, value: str) -> str:
        if str(uuid.UUID(value)) != value:
            raise ValueError("Use a full canonical UUID from orca_list_instances")
        return value


class ClientError(Exception):
    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind
        self.message = message

    def as_dict(self) -> dict:
        return {"kind": self.kind, "message": self.message}


def default_directory() -> Path:
    if sys.platform == "darwin":
        base = Path.home() / "Library/Application Support/OrcaSlicer"
    elif os.name == "nt":
        base = Path(os.environ.get("APPDATA", Path.home())) / "OrcaSlicer"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "OrcaSlicer"
    return base / "cache/agent-control"


def unique_keys(pairs: list) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def descriptor(path: Path, target: Target) -> dict:
    try:
        # O_NONBLOCK prevents a forged FIFO from blocking discovery before fstat.
        if path.is_symlink():
            raise ValueError("Symlink descriptor")
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_BINARY", 0)
        with os.fdopen(os.open(path, flags), "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > 16384:
                raise ValueError("Invalid file")
            if not private_file(stream.fileno(), info):
                raise ValueError("Descriptor must be private")
            value = json.loads(stream.read(16385), object_pairs_hook=unique_keys)
        if not isinstance(value, dict):
            raise ValueError("Invalid descriptor")
        if any(value.get(key) != val for key, val in target.model_dump().items()):
            raise ValueError("Descriptor does not match target")
        match = re.fullmatch(r"http://127\.0\.0\.1:([1-9][0-9]{0,4})/api/v1/rpc", value.get("endpoint", ""))
        if not match or int(match[1]) > 65535:
            raise ValueError("Only exact loopback endpoints are allowed")
        if type(value.get("protocol_version")) is not int or value["protocol_version"] != 1:
            raise ValueError("Unsupported protocol")
        if not re.fullmatch(r"[0-9a-f]{64}", value.get("auth_token", "")):
            raise ValueError("Invalid credential")
        if type(value.get("pid")) is not int or value["pid"] <= 0:
            raise ValueError("Invalid process identity")
        return value
    except FileNotFoundError:
        raise ClientError("instance_unavailable", "This exact window is no longer available; list instances again") from None
    except (OSError, ValueError, TypeError, RecursionError, ImportError):
        raise ClientError("invalid_descriptor", "Rejected an invalid, nonprivate or unsupported discovery file") from None


def redact(value, token: str):
    if isinstance(value, str):
        return value.replace(token, "[redacted]")
    if isinstance(value, list):
        return [redact(item, token) for item in value]
    if isinstance(value, dict):
        return {redact(key, token): redact(item, token) for key, item in value.items()
                if key.lower() not in ("auth_token", "authorization")}
    return value


class Client:
    def __init__(self, directory: Path, *, transport=None):
        self.directory = directory
        self.http = httpx.AsyncClient(timeout=3, trust_env=False, follow_redirects=False, transport=transport)

    async def close(self):
        await self.http.aclose()

    def select(self, target: Target) -> dict:
        return descriptor(self.directory / f"{target.session_id}-{target.window_id}.json", target)

    async def rpc(self, selected: dict, method: str, params: dict, request_id: str) -> dict:
        target = {key: selected[key] for key in ("session_id", "window_id")}
        payload = {"jsonrpc": "2.0", "id": request_id, "method": method,
                   "params": {**params, "target": target}}
        body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode()
        if len(body) > 65536:
            raise ClientError("request_too_large", "Request exceeds the 64 KiB native admission limit")
        try:
            # A total deadline also bounds slow trickles; HTTPX's timeout bounds each I/O.
            async with asyncio.timeout(3):
                async with self.http.stream("POST", selected["endpoint"], content=body, headers={
                    "Content-Type": "application/json", "Authorization": "Bearer " + selected["auth_token"]
                }) as response:
                    if response.status_code == 401:
                        raise ClientError("unauthorized", "Window credentials are no longer accepted")
                    if response.status_code != 200:
                        raise ClientError("http_error", f"Local API returned HTTP {response.status_code}; redirects are forbidden")
                    raw = bytearray()
                    async for part in response.aiter_bytes():
                        raw.extend(part)
                        if len(raw) > 4 * 1024 * 1024:
                            raise ClientError("invalid_response", "Local response exceeds 4 MiB")
            reply = json.loads(raw, object_pairs_hook=unique_keys)
            if not isinstance(reply, dict) or reply.get("jsonrpc") != "2.0" or reply.get("id") != request_id:
                raise ValueError("Mismatched response")
            if ("error" in reply) == ("result" in reply):
                raise ValueError("Invalid RPC envelope")
            if "error" in reply:
                error = reply["error"]
                kind = error.get("data", {}).get("kind", "rpc_error")
                message = error.get("message", "Request rejected")
                if not isinstance(kind, str) or not isinstance(message, str):
                    raise ValueError("Invalid error")
                raise ClientError(redact(kind, selected["auth_token"]), redact(message, selected["auth_token"]))
            result = reply["result"]
            if not isinstance(result, dict) or result.get("target") != target:
                raise ClientError("stale_target", "Response belongs to a different session/window; no fallback was attempted")
            return redact(result, selected["auth_token"])
        except (httpx.HTTPError, TimeoutError):
            raise ClientError("transport_error", "Local API did not provide a complete response; execution outcome may be unknown") from None
        except (ValueError, TypeError, AttributeError, RecursionError):
            raise ClientError("invalid_response", "Local API returned an invalid or mismatched response") from None

    async def poll(self, selected: dict, job_id: str, wait_seconds: float) -> dict:
        deadline = time.monotonic() + wait_seconds
        while True:
            status = await self.rpc(selected, "jobs.get", {"job_id": job_id}, uuid.uuid4().hex)
            if status.get("job_id") != job_id or status.get("state") not in ("queued", "running", "succeeded", "failed", "cancelled"):
                raise ClientError("invalid_response", "Local API returned an invalid job status")
            if status["state"] in ("succeeded", "failed", "cancelled") or status.get("waiting_for_user") or time.monotonic() >= deadline:
                return status
            await asyncio.sleep(min(0.1, max(0, deadline - time.monotonic())))

    async def call(self, target: Target, method: str, params: dict, *, request_id: str | None = None, wait_seconds: float = 1) -> dict:
        request_id = request_id or uuid.uuid4().hex
        context = {"target": target.model_dump(), "request_id": request_id}
        job_id = None
        admitted = False
        try:
            selected = self.select(target)
            ack = await self.rpc(selected, method, params, request_id)
            job_id = ack.get("job_id")
            if not isinstance(job_id, str) or not re.fullmatch(re.escape(target.window_id) + r":[1-9][0-9]*", job_id):
                raise ClientError("invalid_response", "Local API returned an invalid job identifier")
            admitted = True
            return {**await self.poll(selected, job_id, wait_seconds), **context}
        except ClientError as error:
            result = {**context, "state": "unknown" if admitted or error.kind in ("transport_error", "invalid_response", "http_error", "stale_target") else "failed",
                      "error": error.as_dict()}
            if admitted:
                result.update(job_id=job_id, recovery="Poll this job with orca_get_job; do not submit a new operation")
            elif result["state"] == "unknown":
                if method not in READ_METHODS:
                    result["recovery"] = "Retry the exact same tool arguments and request_id; never refresh state_token or invent a new ID for this retry"
                else:
                    result["recovery"] = "Retry this read tool; the original observation may still exist, but it cannot edit settings or start slicing"
            return result

    async def job(self, target: Target, job_id: str, wait_seconds: float) -> dict:
        context = {"target": target.model_dump(), "job_id": job_id}
        if not re.fullmatch(re.escape(target.window_id) + r":[1-9][0-9]*", job_id):
            return {**context, "state": "failed", "error": {"kind": "invalid_job", "message": "Job ID must belong to the explicitly selected window"}}
        try:
            return {**await self.poll(self.select(target), job_id, wait_seconds), **context}
        except ClientError as error:
            return {**context, "state": "unknown", "error": error.as_dict(), "recovery": "The job was not cancelled; retry observation of this same job"}

    async def instances(self) -> dict:
        try:
            paths = sorted(self.directory.glob("*.json"))
        except OSError:
            return {"instances": [], "error": {"kind": "discovery_unavailable", "message": "Cannot read the configured discovery directory"}}
        semaphore = asyncio.Semaphore(8)

        async def probe(path):
            async with semaphore:
                try:
                    session_id, window_id = path.stem[:36], path.stem[37:]
                    target = Target(session_id=session_id, window_id=window_id)
                    if path.name != f"{session_id}-{window_id}.json":
                        return None
                    selected = self.select(target)
                    status = await self.call(target, "scene.describe", {}, wait_seconds=0.5)
                    if status["state"] == "unknown" or status.get("error", {}).get("kind") in ("unauthorized", "instance_unavailable", "invalid_descriptor"):
                        return None
                    row = {"target": target.model_dump(), "pid": selected["pid"], "scene_job": status}
                    if status["state"] == "succeeded":
                        scene = status.get("result", {})
                        row.update(project=scene.get("project"), window_title=scene.get("window_title"))
                    return row
                except (ClientError, ValueError):
                    return None

        rows = await asyncio.gather(*(probe(path) for path in paths[:128]))
        return {"instances": [row for row in rows if row is not None], "truncated": len(paths) > 128,
                "hint": "Only ORCA_AGENT_CONTROL=1 windows are discoverable. Choose an explicit UUID pair; never infer a target from focus or list order."}
