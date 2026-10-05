#!/usr/bin/env python3
"""CTest driver: serve a score over REST, read and render it, and check every frame with the oracle.

Starts rendering-engine in server mode on a free port, with --per-set-wav,
then over HTTP: loads one score, reads its engine parameters and tempo maps,
renders every frame one request names, one call per frame, and asks the
server to shut down. Every frame's file, and each partial set's file where one
was written, is then checked with render_oracle.py, exactly as
run_oracle_check.py checks batch mode's. Along the way it checks the calls a
client depends on: a call before the load is refused, the URIs returned name
files that exist, the tempo map's points can be paged to the end, a wrong
method gets 405 and an unknown path 404, and the server exits 0 when told to.

With --beats, the score's tempo map is also checked against the oracle's
score: frame-position of each of those beats must be the floor of the time,
in samples, of the corresponding envelope node of the first partial's frequency
envelope in the score's JSON mirror, split into frame and offset.

    run_server_check.py --engine <binary> --schema <cypher> --state <cypher>
        --request <yaml> --score <json> --out-dir <dir> [--spec <openapi.yaml>]
        [--beats 0,2,4]

With --spec, every response body is also validated against the response
schema the OpenAPI document gives for its path, method and status, when
PyYAML and jsonschema are available (openapi_check.py).

Exit status 0 when every call and every oracle run succeeds, 1 on any failure,
and 77 (CTest's SKIP_RETURN_CODE here) when NumPy is not available, so that
the C++ test suite does not depend on Python. Uses the standard library only,
besides the oracle's NumPy and the optional schema check.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import openapi_check  # noqa: E402  (beside this file)

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


class Client:
    """Calls the server, and checks each response against the OpenAPI document when it can."""

    def __init__(self, base: str, spec: dict | None) -> None:
        """
        Args:
            base: The server's URL, e.g. http://127.0.0.1:8080.
            spec: The OpenAPI document, or None not to check responses.
        """
        self.base = base
        self.spec = spec
        self.failures = 0

    def call(self, method: str, path: str, template: str | None = None, body: dict | None = None,
             query: dict | None = None) -> tuple[int, dict]:
        """Make one call and return the status and the parsed JSON response.

        Args:
            method: The HTTP method.
            path: The path under /v1, e.g. /score.
            template: The path as the OpenAPI document writes it, e.g.
                /tempo-maps/{id}/points, when it differs from `path`; None for
                a call the document does not describe.
            body: The JSON request body, or None for none.
            query: Query parameters, or None.

        Returns:
            The HTTP status and the response body, which the server always
            sends as a JSON object, on success and failure alike.
        """
        url = self.base + "/v1" + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        data = None if body is None else json.dumps(body).encode("utf-8")
        headers = {} if body is None else {"Content-Type": "application/json"}
        request = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=CALL_SECONDS) as response:
                status, payload, content_type = response.status, json.loads(response.read()), \
                    response.headers.get("Content-Type", "")
                allow = response.headers.get("Allow")
        except urllib.error.HTTPError as error:
            status, payload, content_type = error.code, json.loads(error.read()), \
                error.headers.get("Content-Type", "")
            allow = error.headers.get("Allow")
        payload["_allow"] = allow

        expected_type = "application/json" if status < 400 else "application/problem+json"
        self.check(content_type.startswith(expected_type),
                   f"{method} {path} gave {status} as {content_type!r}, not {expected_type}")
        if self.spec is not None and template is not None:
            try:
                validator = openapi_check.response_schema_validator(self.spec, template, method.lower(), status)
            except KeyError:
                self.check(False, f"{method} {template} answered {status}, which the OpenAPI document does not list")
            else:
                for error in validator.iter_errors({k: v for k, v in payload.items() if k != "_allow"}):
                    self.check(False, f"{method} {path} {status}: the body does not fit the document: {error.message}")
        return status, payload

    def check(self, condition: bool, what: str) -> None:
        """Report one expectation, counting it if it failed."""
        self.failures += check(condition, what)


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


def load_spec(path: str | None) -> dict | None:
    """The OpenAPI document, or None when there is none or it cannot be used here."""
    if path is None:
        return None
    try:
        import jsonschema  # noqa: F401
        import yaml  # noqa: F401
    except ImportError as error:
        print(f"run_server_check: {error.name} is not available; responses are not checked against the OpenAPI document")
        return None
    return openapi_check.load_spec(path)


def check_tempo_map(client: Client, score_path: str, beats: list[int]) -> None:
    """Read the root tempo map, page through its points, and check frame positions.

    Args:
        client: The client, with a score loaded.
        score_path: The oracle's JSON mirror of the score.
        beats: Beats whose frame position to check against the first
            partial's frequency envelope nodes, one beat per envelope node, in order.
    """
    status, root = client.call("GET", "/tempo-maps/root", "/tempo-maps/root")
    if status != 200:
        client.check(False, f"GET /tempo-maps/root gave {status}: {root.get('detail')}")
        return
    map_id = root["id"]

    status, children = client.call("GET", f"/tempo-maps/{map_id}/children", "/tempo-maps/{id}/children")
    client.check(status == 200 and children.get("children") == root["children"],
                 f"the root's children came back as {status} {children}")

    # Every point, one to a page, following `next` as a client would.
    positions = []
    path, query = f"/tempo-maps/{map_id}/points", {"limit": 1}
    for _ in range(10000):
        status, page = client.call("GET", path, "/tempo-maps/{id}/points", query=query)
        if status != 200:
            client.check(False, f"GET {path} {query} gave {status}: {page.get('detail')}")
            return
        positions += [point["position"] for point in page["points"]]
        if page["next"] is None:
            break
        parsed = urllib.parse.urlsplit(page["next"])
        client.check(parsed.path == "/v1" + path, f"next names {parsed.path}, not /v1{path}")
        query = dict(urllib.parse.parse_qsl(parsed.query))
    client.check(len(positions) > 0 and positions == sorted(set(positions)),
                 f"the points came back as {positions}, not strictly increasing")

    status, config = client.call("GET", "/engine-config", "/engine-config")
    if status != 200:
        client.check(False, f"GET /engine-config gave {status}: {config.get('detail')}")
        return
    with open(score_path, encoding="utf-8") as handle:
        score = json.load(handle)
    client.check(config["sample_rate"] == score["sample_rate"],
                 f"the sample rate is {config['sample_rate']}, the oracle's {score['sample_rate']}")
    if not beats:
        return
    nodes = [time for time, _ in score["partial_sets"][0]["partials"][0]["frequency"]]
    client.check(len(nodes) == len(beats), f"--beats names {len(beats)} beats for {len(nodes)} envelope nodes")
    for beat, time in zip(beats, nodes):
        status, at = client.call("GET", f"/tempo-maps/{map_id}/frame-position",
                                 "/tempo-maps/{id}/frame-position", query={"at": str(beat)})
        if status != 200:
            client.check(False, f"frame-position of beat {beat} gave {status}: {at.get('detail')}")
            continue
        sample = int(time // 1)
        expected = (sample // config["frame_size"], sample % config["frame_size"])
        client.check((at["frame"], at["offset"]) == expected,
                     f"beat {beat} is at frame {at['frame']}, offset {at['offset']}; the oracle's "
                     f"time {time} gives {expected}")


def main() -> int:
    """Serve, load, read, render per frame, shut down; then run the oracle per file."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--request", required=True)
    parser.add_argument("--score", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--spec")
    parser.add_argument("--beats", help="comma-separated beats of the first partial's frequency envelope nodes")
    args = parser.parse_args()

    try:
        import numpy  # noqa: F401
    except ImportError:
        print("run_server_check: NumPy is not available; skipping the server check")
        return SKIP

    shutil.rmtree(args.out_dir, ignore_errors=True)
    os.makedirs(args.out_dir)

    server, base = start_server(args)
    client = Client(base, load_spec(args.spec))
    checks: list[tuple[int, str, list[str]]] = []  # frame, file, oracle arguments
    try:
        status, _ = client.call("POST", "/frames/0/renderings", "/frames/{frame}/renderings")
        client.check(status == 409, f"a render before any load gave {status}, not 409")
        status, _ = client.call("GET", "/tempo-maps/root", "/tempo-maps/root")
        client.check(status == 409, f"a read before any load gave {status}, not 409")
        status, response = client.call("GET", "/score")
        client.check(status == 405 and response["_allow"] == "PUT",
                     f"GET /score gave {status} with Allow {response['_allow']}, not 405 with PUT")
        status, _ = client.call("GET", "/no-such-endpoint")
        client.check(status == 404, f"an unknown path gave {status}, not 404")

        status, response = client.call("PUT", "/score", "/score", body={"uri": file_uri(os.path.abspath(args.state))})
        if status != 200:
            print(f"run_server_check: PUT /score gave {status}: {response.get('detail')}", file=sys.stderr)
            return 1

        if args.beats is not None:
            check_tempo_map(client, args.score, [int(beat) for beat in args.beats.split(",") if beat])
        else:
            status, _ = client.call("GET", "/engine-config", "/engine-config")
            client.check(status == 200, f"GET /engine-config gave {status}")

        for frame in requested_frames(args.request):
            status, response = client.call("POST", f"/frames/{frame}/renderings", "/frames/{frame}/renderings")
            if status != 200:
                client.check(False, f"rendering frame {frame} gave {status}: {response.get('detail')}")
                continue
            client.check(response.get("frame") == frame, f"frame {frame} came back as {response.get('frame')}")
            wav = uri_path(response["uri"])
            client.check(os.path.isfile(wav), f"frame {frame}'s URI {response['uri']} names no file")
            checks.append((frame, wav, []))
            for set_uri in response.get("set_uris", []):
                set_wav = uri_path(set_uri)
                set_id = int(re.search(r"\.set-(\d+)\.wav$", set_wav).group(1))
                checks.append((frame, set_wav, ["--set", str(set_id)]))
    finally:
        status, _ = client.call("POST", "/shutdown", "/shutdown")
        client.check(status == 200, f"POST /shutdown gave {status}, not 200")
        server.wait(timeout=CALL_SECONDS)
        client.check(server.returncode == 0, f"the server exited {server.returncode}, not 0")

    failures = client.failures
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
