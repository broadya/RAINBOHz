#!/usr/bin/env python3
"""CTest driver: serve a score over REST, render it, and check every frame with the oracle.

Starts rendering-engine in server mode on a free port, with --per-set-wav,
then over HTTP: loads one score, renders every frame one request names, one
call per frame, and asks the server to shut down. Every frame's file, and each
partial set's file where one was written, is then checked with render_oracle.py,
exactly as run_oracle_check.py checks batch mode's. Along the way it checks
the calls a client depends on: a render before the load is refused, the URIs
returned name files that exist, and the server exits 0 when told to.

    run_server_check.py --engine <binary> --schema <cypher> --state <cypher>
        --request <yaml> --score <json> --out-dir <dir>

Exit status 0 when every call and every oracle run succeeds, 1 on any failure,
and 77 (CTest's SKIP_RETURN_CODE here) when NumPy is not available, so that
the C++ test suite does not depend on Python. Uses the standard library only,
besides the oracle's NumPy.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request

SKIP = 77

# How long to wait for the server to start, and for any one call. A load
# applies the whole schema in a debug build, which takes about a second.
START_SECONDS = 30
CALL_SECONDS = 60


def requested_frames(request_path: str) -> list[int]:
    """Read the frame numbers a request names, as run_oracle_check.py does.

    Args:
        request_path: The YAML request file.

    Returns:
        The frame numbers, in file order.
    """
    frames = []
    with open(request_path, encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^\s*-\s*frame:\s*(\d+)\s*(#.*)?$", line)
            if match:
                frames.append(int(match.group(1)))
    return frames


def file_uri(path: str) -> str:
    """The file:// URI of an absolute path, encoded as the server encodes it."""
    return "file://" + urllib.parse.quote(path, safe="/-._~")


def uri_path(uri: str) -> str:
    """The local path a file:// URI names."""
    if not uri.startswith("file://"):
        raise ValueError(f"not a file:// URI: {uri}")
    return urllib.parse.unquote(uri[len("file://"):])


def post(base: str, endpoint: str, body: dict | None) -> tuple[int, dict]:
    """POST a JSON body and return the status and the parsed JSON response.

    Args:
        base: The server's URL, e.g. http://127.0.0.1:8080.
        endpoint: The path, e.g. /load.
        body: The request body, or None for an empty one.

    Returns:
        The HTTP status and the response body, which the server always
        sends as a JSON object, on success and failure alike.
    """
    data = b"" if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(
        base + endpoint, data=data, method="POST", headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=CALL_SECONDS) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def start_server(args: argparse.Namespace) -> tuple[subprocess.Popen, str]:
    """Start the engine in server mode on a free port.

    The input root is the directory holding the score, and the port is 0,
    so the server picks one and reports it on its `listening on` line.

    Returns:
        The process, and the server's base URL.

    Raises:
        RuntimeError: If the server exits or does not report a port in time.
    """
    server = subprocess.Popen(
        [
            args.engine, "--mode", "server", "--schema", args.schema,
            "--host", "127.0.0.1", "--port", "0",
            "--input-root", os.path.dirname(os.path.abspath(args.state)),
            "--out-dir", args.out_dir, "--per-set-wav",
        ],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    # The listening line is the server's last line before it waits for calls;
    # reading up to it cannot block on output that never comes, short of a
    # hung start, which the CTest timeout catches. From then on a thread keeps
    # reading, so that the server never blocks on a full pipe mid-call.
    for line in server.stdout:
        print(line, end="")
        match = re.search(r"listening on (http://\S+)", line)
        if match:
            threading.Thread(target=lambda: [None for _ in server.stdout], daemon=True).start()
            return server, match.group(1)
    server.wait(timeout=START_SECONDS)
    raise RuntimeError(f"the server exited {server.returncode} before listening")


def check(condition: bool, what: str) -> int:
    """Report one expectation; return 1 if it failed, 0 if it held."""
    if condition:
        return 0
    print(f"run_server_check: FAILED: {what}", file=sys.stderr)
    return 1


def main() -> int:
    """Serve, load, render per frame, shut down; then run the oracle per file."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--request", required=True)
    parser.add_argument("--score", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    try:
        import numpy  # noqa: F401
    except ImportError:
        print("run_server_check: NumPy is not available; skipping the server check")
        return SKIP

    shutil.rmtree(args.out_dir, ignore_errors=True)
    os.makedirs(args.out_dir)

    server, base = start_server(args)
    failures = 0
    checks: list[tuple[int, str, list[str]]] = []  # frame, file, oracle arguments
    try:
        status, response = post(base, "/render", {"frame": 0})
        failures += check(status == 409, f"a render before any load gave {status}, not 409")

        status, response = post(base, "/load", {"uri": file_uri(os.path.abspath(args.state))})
        if status != 200:
            print("\n".join(response.get("log", [])))
            print(f"run_server_check: /load gave {status}: {response.get('message')}", file=sys.stderr)
            return 1

        for frame in requested_frames(args.request):
            status, response = post(base, "/render", {"frame": frame})
            if status != 200:
                print("\n".join(response.get("log", [])))
                failures += check(False, f"/render of frame {frame} gave {status}: {response.get('message')}")
                continue
            failures += check(response.get("frame") == frame, f"frame {frame} came back as {response.get('frame')}")
            wav = uri_path(response["uri"])
            failures += check(os.path.isfile(wav), f"frame {frame}'s URI {response['uri']} names no file")
            checks.append((frame, wav, []))
            for set_uri in response.get("set_uris", []):
                set_wav = uri_path(set_uri)
                set_id = int(re.search(r"\.set-(\d+)\.wav$", set_wav).group(1))
                checks.append((frame, set_wav, ["--set", str(set_id)]))
    finally:
        status, _ = post(base, "/shutdown", None)
        failures += check(status == 200, f"/shutdown gave {status}, not 200")
        server.wait(timeout=CALL_SECONDS)
        failures += check(server.returncode == 0, f"the server exited {server.returncode}, not 0")

    oracle = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "oracle", "render_oracle.py")
    for frame, wav, extra in checks:
        result = subprocess.run(
            [sys.executable, oracle, "--score", args.score, "--frame", str(frame), "--wav", wav, *extra],
            capture_output=True, text=True, check=False,
        )
        print(result.stdout, end="")
        if result.returncode != 0:
            print(result.stderr, end="", file=sys.stderr)
            failures += 1

    if failures:
        print(f"run_server_check: {failures} check(s) failed", file=sys.stderr)
        return 1
    print(f"run_server_check: {len(checks)} file(s) served and checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
