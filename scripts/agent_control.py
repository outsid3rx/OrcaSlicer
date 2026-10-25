#!/usr/bin/env python3
"""Local Orca client. Uses only Python's standard library; never prints API tokens."""

import argparse
import json
import os
from pathlib import Path
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


def default_directory():
    if sys.platform == "darwin":
        base = Path.home() / "Library/Application Support/OrcaSlicer"
    elif os.name == "nt":
        base = Path(os.environ.get("APPDATA", Path.home())) / "OrcaSlicer"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "OrcaSlicer"
    return base / "cache/agent-control"


def descriptor(path):
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0)
    with os.fdopen(os.open(path, flags), "r", encoding="utf-8") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > 16384:
            raise ValueError("Invalid descriptor file")
        if os.name == "posix" and (info.st_uid != os.geteuid() or info.st_mode & 0o077):
            raise ValueError("Descriptor must be private to the current user")
        value = json.loads(stream.read(16385))
    for key in ("session_id", "window_id"):
        if str(uuid.UUID(value[key])) != value[key]:
            raise ValueError("Invalid target identifier")
    endpoint = urllib.parse.urlsplit(value["endpoint"])
    if (endpoint.scheme != "http" or endpoint.hostname != "127.0.0.1" or not endpoint.port
            or endpoint.path != "/api/v1/rpc" or endpoint.query or endpoint.fragment
            or endpoint.username or endpoint.password):
        raise ValueError("Only local Orca endpoints are accepted")
    if value.get("protocol_version") != 1 or not isinstance(value.get("auth_token"), str):
        raise ValueError("Unsupported control protocol")
    return value


def target(value):
    return {key: value[key] for key in ("session_id", "window_id")}


def rpc(value, method, params=None, request_id=None):
    params = dict(params or {})
    if "target" in params and params["target"] != target(value):
        raise ValueError("The supplied target differs from the selected window")
    params["target"] = target(value)
    payload = {"jsonrpc": "2.0", "id": request_id or uuid.uuid4().hex,
               "method": method, "params": params}
    request = urllib.request.Request(value["endpoint"], json.dumps(payload).encode("utf-8"),
        {"Content-Type": "application/json", "Authorization": "Bearer " + value["auth_token"]})
    # Never send local credentials through an environment-configured proxy or redirect.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            raise ValueError("Local API redirects are forbidden")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    with opener.open(request, timeout=3) as response:
        raw = response.read(4 * 1024 * 1024 + 1)
    if len(raw) > 4 * 1024 * 1024:
        raise ValueError("API response exceeds the limit")
    reply = json.loads(raw)
    if reply.get("jsonrpc") != "2.0" or reply.get("id") != payload["id"]:
        raise ValueError("Unexpected RPC response")
    if "error" in reply:
        raise ValueError(json.dumps(reply["error"], ensure_ascii=False))
    result = reply["result"]
    if result.get("target") != target(value):
        raise ValueError("Endpoint no longer belongs to this session and window")
    return result


def wait(value, job_id, seconds):
    deadline = time.monotonic() + seconds
    while True:
        status = rpc(value, "jobs.get", {"job_id": job_id})
        if status["state"] in ("succeeded", "failed", "cancelled") or time.monotonic() >= deadline:
            return status
        time.sleep(0.1)


def select(directory, session, window):
    # Do not choose by title, PID, port, or which window was most recently focused.
    session, window = str(uuid.UUID(session)), str(uuid.UUID(window))
    value = descriptor(directory / (session + "-" + window + ".json"))
    if target(value) != {"session_id": session, "window_id": window}:
        raise ValueError("Descriptor does not match the selected target")
    rpc(value, "system.describe")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=default_directory(), help="Private discovery directory")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="List live explicitly enabled Orca windows")
    for name in ("call", "wait"):
        child = commands.add_parser(name)
        child.add_argument("--session", required=True)
        child.add_argument("--window", required=True)
        child.add_argument("--wait-seconds", type=float, default=30)
        if name == "call":
            child.add_argument("method")
            child.add_argument("--params", type=Path, help="JSON file with method params (target is added)")
            child.add_argument("--request-id", help="Reuse only when retrying the exact same request")
        else:
            child.add_argument("job_id")
    args = parser.parse_args()
    if args.command == "list":
        rows = []
        for path in sorted(args.directory.glob("*.json")):
            try:
                value = descriptor(path)
                rpc(value, "system.describe")  # Verify ownership, including after PID/port reuse.
                job = rpc(value, "scene.describe")
                status = wait(value, job["job_id"], 1)
                row = {**target(value), "pid": value["pid"], "state": status["state"]}
                if status["state"] == "succeeded":
                    scene = status["result"]
                    row.update(project=scene["project"], window_title=scene["window_title"])
                rows.append(row)
            except (ValueError, OSError, KeyError, urllib.error.URLError):
                continue  # Stale/crashed endpoints are not selected or deleted.
        print(json.dumps(rows, ensure_ascii=False, indent=2))
        return 0
    value = select(args.directory, args.session, args.window)
    if args.command == "call":
        params = json.loads(args.params.read_text(encoding="utf-8")) if args.params else {}
        if not isinstance(params, dict):
            raise ValueError("Params file must contain a JSON object")
        job = rpc(value, args.method, params, args.request_id)
        if args.method == "jobs.get":
            status = job
        else:
            status = wait(value, job["job_id"], max(0, args.wait_seconds))
    else:
        status = wait(value, args.job_id, max(0, args.wait_seconds))
    print(json.dumps(status, ensure_ascii=False, indent=2))
    return 1 if status["state"] in ("failed", "cancelled") else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, urllib.error.URLError) as error:
        print(f"Agent control: {error}", file=sys.stderr)
        sys.exit(1)
