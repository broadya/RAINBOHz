# SPDX-License-Identifier: Apache-2.0

"""pg: loop an HzL piece through rendering-engine and playback-client.

The playground's glue. It is not part of the repository (`playground/` is in
`.git/info/exclude`), and uses only the standard library, run by the root
`.venv`'s Python so that `hzl_compiler` is importable for `hzlc`.

One session at a time. Its state is `.run/state.json`: the piece, the
server's port, both PIDs and the loop's span in samples.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

PLAYGROUND = Path(__file__).resolve().parent.parent
REPO = PLAYGROUND.parent
PIECES = PLAYGROUND / "pieces"
RUN = PLAYGROUND / ".run"
STATE = RUN / "state.json"
SERVER_LOG = RUN / "server.log"
CLIENT_LOG = RUN / "client.log"
VENV_PY = REPO / ".venv" / "bin" / "python"
PRESET = os.environ.get("PG_PRESET", "macos-debug")
ENGINE = REPO / "rendering-engine" / "build" / PRESET / "bin" / "rendering-engine"
CLIENT = REPO / "playback-client" / "build" / PRESET / "bin" / "playback-client"

# Enough loops to be endless in practice: 1e6 two-second bars is 23 days.
LOOPS = 1_000_000


class PgError(Exception):
    """A failure to report to the user in one line, ending the command."""


def check_tools() -> None:
    """Checks that the venv and both binaries exist.

    Raises:
        PgError: naming what is missing and the make target that builds it.
    """
    missing = []
    if not VENV_PY.exists():
        missing.append(f"{VENV_PY} (make venv)")
    if not ENGINE.exists():
        missing.append(f"{ENGINE} (make rendering-engine RE_PRESET={PRESET})")
    if not CLIENT.exists():
        missing.append(f"{CLIENT} (make playback-client PC_PRESET={PRESET})")
    if missing:
        raise PgError("missing, run from the repository root:\n  " + "\n  ".join(missing))


def read_piece(piece: str) -> dict[str, str]:
    """Reads a piece's settings file, `pieces/<piece>/<piece>.yaml`.

    It is a flat `key: value` file, parsed by hand so that pg needs nothing
    beyond the standard library.

    Args:
        piece: The piece's directory name under `pieces/`.

    Returns:
        `score` (an absolute path), `loop_start`, `loop_end` and
        `channel_mode` (hzlc's --channel-mode, "0", mono, when not given).

    Raises:
        PgError: if the piece, its settings file, a key or the score is missing.
    """
    path = PIECES / piece / f"{piece}.yaml"
    if not path.exists():
        raise PgError(f"no piece '{piece}': {path} does not exist")
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        key, _, value = line.partition(":")
        values[key.strip()] = value.strip().strip("'\"")
    for key in ("score", "loop_start", "loop_end"):
        if not values.get(key):
            raise PgError(f"{path}: '{key}' is missing")
    score = (PIECES / piece / values["score"]).resolve()
    if not score.exists():
        raise PgError(f"{path}: score {score} does not exist")
    values["score"] = str(score)
    values.setdefault("channel_mode", "0")
    return values


def compile_piece(piece: str, score: str, channel_mode: str) -> Path:
    """Compiles a piece's score to `.run/<piece>.tables/` with hzlc.

    The output is hzlc's directory of Parquet tables and their manifest,
    which the engine loads far faster than the Cypher form. It is compiled
    into a temporary directory first and swapped in only when hzlc succeeds,
    so a failed compile leaves the last good tables in place.

    Args:
        piece: The piece's name, which names the output directory.
        score: Absolute path of the score's .yaml.
        channel_mode: hzlc's --channel-mode, 0 to 7; 1 is stereo from M/S.

    Returns:
        The compiled score's manifest.json.

    Raises:
        PgError: with hzlc's message, if the compile fails.
    """
    RUN.mkdir(exist_ok=True)
    out = RUN / f"{piece}.tables"
    tmp = RUN / f"{piece}.tables.new"
    old = RUN / f"{piece}.tables.old"
    for leftover in (tmp, old):
        shutil.rmtree(leftover, ignore_errors=True)
    result = subprocess.run(
        [str(VENV_PY), "-m", "hzl_compiler", "compile", score, "-o", str(tmp), "-q",
         "--channel-mode", channel_mode],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        shutil.rmtree(tmp, ignore_errors=True)
        raise PgError("compile failed, nothing changed:\n" + (result.stderr or result.stdout))
    # A directory cannot replace a non-empty one in a single rename, so the
    # old tables step aside first.
    if out.exists():
        out.rename(old)
    tmp.rename(out)
    shutil.rmtree(old, ignore_errors=True)
    return out / "manifest.json"


def call(port: int, method: str, path: str, body: Any = None, timeout: float = 120) -> Any:
    """Makes one REST call to the server.

    Args:
        port: The server's port on 127.0.0.1.
        method: HTTP method.
        path: Path under /v1, with any query string.
        body: JSON-serialisable body, or None for none.
        timeout: Seconds to wait for the answer; a score load can take a while.

    Returns:
        The decoded JSON answer.

    Raises:
        PgError: on a problem answer (with its detail and errors) or if the
            server cannot be reached.
    """
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(f"http://127.0.0.1:{port}/v1{path}", data=data, method=method)
    request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read() or b"null")
    except urllib.error.HTTPError as error:
        try:
            problem = json.loads(error.read())
            detail = f"{problem.get('detail', '')}"
            for line in problem.get("errors", []):
                detail += f"\n  {line}"
        except ValueError:
            detail = str(error)
        raise PgError(f"{method} {path}: {error.code} {detail}") from error
    except OSError as error:
        raise PgError(f"{method} {path}: cannot reach the server on port {port}: {error}") from error


def load_score(port: int, manifest: Path) -> None:
    """Loads a compiled score into the server, replacing the one it holds.

    Args:
        port: The server's port.
        manifest: The compiled score's manifest, under the playground (the
            input root).

    Raises:
        PgError: if the load fails. After a 422 the server holds no score.
    """
    answer = call(port, "PUT", "/score", {"uri": manifest.resolve().as_uri()})
    print(f"pg: loaded {answer['partials']} partials")


def loop_span(port: int, start: str, end: str) -> list[int]:
    """Finds where the loop's start and end fall, in samples.

    Used to tell whether a reload changed the loop the client is playing:
    the client works its frame range out once, at start.

    Args:
        port: The server's port.
        start: Loop start, beats:ticks.
        end: Loop end, beats:ticks.

    Returns:
        [start sample, end sample].

    Raises:
        PgError: if the server cannot answer.
    """
    root = call(port, "GET", "/tempo-maps/root")
    frame_size = call(port, "GET", "/engine-config")["frame_size"]
    span = []
    for at in (start, end):
        query = urllib.parse.urlencode({"at": at})
        pos = call(port, "GET", f"/tempo-maps/{root['id']}/frame-position?{query}")
        span.append(pos["frame"] * frame_size + pos["offset"])
    return span


def alive(pid: int | None) -> bool:
    """Tells whether a process is still running.

    Args:
        pid: The process to look for; None or 0 means none.

    Returns:
        True when it exists and is not a finished child awaiting reaping.
        Reaps it in that case, as a side effect.
    """
    if not pid:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    # A finished child of ours that nobody reaped would still answer kill 0.
    try:
        done, _ = os.waitpid(pid, os.WNOHANG)
        return done == 0
    except ChildProcessError:
        return True


def stop_process(pid: int | None, sig: int, wait: float) -> None:
    """Sends a signal and waits for the process to go, killing it if it does not.

    Does nothing when the process has already gone.

    Args:
        pid: The process, or None for nothing to do.
        sig: The first signal to send.
        wait: Seconds to wait before SIGKILL.
    """
    if not alive(pid):
        return
    assert pid is not None
    os.kill(pid, sig)
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        if not alive(pid):
            return
        time.sleep(0.1)
    os.kill(pid, signal.SIGKILL)


def start_server() -> tuple[int, int]:
    """Starts rendering-engine in server mode in the background.

    The server reads scores from beneath the playground and writes frames to
    `.run/frames`. It is given port 0 and the real port is read from its log.

    Returns:
        (pid, port).

    Raises:
        PgError: if it exits or does not report a port within 30 s.
    """
    log = SERVER_LOG.open("w")
    process = subprocess.Popen(
        [str(ENGINE), "--mode", "server", "--host", "127.0.0.1", "--port", "0",
         "--input-root", str(PLAYGROUND), "--out-dir", str(RUN / "frames")],
        stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
    )
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        match = re.search(r"listening on http://[^:]+:(\d+)", SERVER_LOG.read_text())
        if match:
            return process.pid, int(match.group(1))
        if process.poll() is not None:
            raise PgError(f"the server exited; see {SERVER_LOG}")
        time.sleep(0.1)
    process.kill()
    raise PgError(f"the server did not start listening; see {SERVER_LOG}")


def start_client(port: int, start: str, end: str) -> int:
    """Starts playback-client in the background, looping [start, end).

    Its log is appended to `.run/client.log`, and its buffer is made under
    `.run/buffer`.

    Args:
        port: The server's port.
        start: Loop start, beats:ticks.
        end: Loop end, beats:ticks.

    Returns:
        The client's PID.
    """
    buffer = RUN / "buffer"
    buffer.mkdir(parents=True, exist_ok=True)
    with CLIENT_LOG.open("a") as log:
        log.write(f"--- pg: client started {time.strftime('%H:%M:%S')}, {start} to {end}\n")
        log.flush()
        process = subprocess.Popen(
            [str(CLIENT), "--server", f"http://127.0.0.1:{port}/v1", "--start", start,
             "--end", end, "--loops", str(LOOPS), "--buffer-dir", str(buffer)],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        )
    return process.pid


def read_state() -> dict[str, Any] | None:
    """Reads `.run/state.json`.

    Returns:
        The running session's state, or None when there is none.
    """
    if not STATE.exists():
        return None
    state: dict[str, Any] = json.loads(STATE.read_text())
    return state


def write_state(state: dict[str, Any]) -> None:
    """Saves the session's state.

    Args:
        state: Written to `.run/state.json`, replacing what was there.
    """
    STATE.write_text(json.dumps(state, indent=2) + "\n")


def cmd_start(args: argparse.Namespace) -> None:
    """`pg start [piece]`: compile, start the server, load, start looping.

    Args:
        args: The parsed command line.

    Raises:
        PgError: on any failure, with the reason; prints progress to stdout.
    """
    state = read_state()
    if state and alive(state.get("server_pid")):
        raise PgError(f"'{state['piece']}' is already running: pg reload, or pg stop first")
    check_tools()
    piece = read_piece(args.piece)
    manifest = compile_piece(args.piece, piece["score"], piece["channel_mode"])
    server_pid, port = start_server()
    print(f"pg: server on port {port} (pid {server_pid})")
    state = {"piece": args.piece, "port": port, "server_pid": server_pid, "client_pid": None}
    write_state(state)
    load_score(port, manifest)
    state["span"] = loop_span(port, piece["loop_start"], piece["loop_end"])
    state["loop"] = [piece["loop_start"], piece["loop_end"]]
    state["channel_mode"] = piece["channel_mode"]
    state["client_pid"] = start_client(port, piece["loop_start"], piece["loop_end"])
    write_state(state)
    print(f"pg: looping '{args.piece}' {piece['loop_start']} to {piece['loop_end']}; "
          "pg reload after editing, pg stop to end")


def cmd_reload(_: argparse.Namespace) -> None:
    """`pg reload`: recompile, swap the score in, restart the client if needed.

    Args:
        args: The parsed command line.

    Raises:
        PgError: on any failure, with the reason; prints progress to stdout.
    """
    state = read_state()
    if not state or not alive(state.get("server_pid")):
        raise PgError("nothing is running: pg start")
    piece = read_piece(state["piece"])
    manifest = compile_piece(state["piece"], piece["score"], piece["channel_mode"])
    port = state["port"]
    load_score(port, manifest)
    loop = [piece["loop_start"], piece["loop_end"]]
    span = loop_span(port, *loop)
    reasons = []
    if not alive(state.get("client_pid")):
        reasons.append("the client had stopped")
    if span != state.get("span") or loop != state.get("loop"):
        reasons.append("the loop changed")
    if piece["channel_mode"] != state.get("channel_mode"):
        reasons.append("the channel mode changed")
    if reasons:
        stop_process(state.get("client_pid"), signal.SIGINT, 5)
        state["client_pid"] = start_client(port, *loop)
        print(f"pg: restarted playback ({', '.join(reasons)})")
    else:
        print("pg: the next loop plays the new version")
    state["span"], state["loop"] = span, loop
    state["channel_mode"] = piece["channel_mode"]
    write_state(state)


def cmd_stop(_: argparse.Namespace) -> None:
    """`pg stop`: stop the client, shut the server down, clear the state.

    Args:
        args: The parsed command line.

    Raises:
        PgError: on any failure, with the reason; prints progress to stdout.
    """
    state = read_state()
    if not state:
        print("pg: nothing is running")
        return
    stop_process(state.get("client_pid"), signal.SIGINT, 5)
    if alive(state.get("server_pid")):
        try:
            call(state["port"], "POST", "/shutdown", timeout=5)
        except PgError:
            pass
        stop_process(state.get("server_pid"), signal.SIGTERM, 5)
    STATE.unlink(missing_ok=True)
    print("pg: stopped")


def tail(path: Path, lines: int) -> str:
    """Gives the end of a log, for `pg status`.

    Args:
        path: The log file.
        lines: How many lines from its end.

    Returns:
        Those lines, indented by two spaces, or a note that there is no log.
    """
    if not path.exists():
        return "  (no log)"
    return "\n".join("  " + line for line in path.read_text().splitlines()[-lines:])


def cmd_status(_: argparse.Namespace) -> None:
    """`pg status`: what is running, and the end of each log.

    Args:
        args: The parsed command line.

    Raises:
        PgError: on any failure, with the reason; prints progress to stdout.
    """
    state = read_state()
    if not state:
        print("pg: nothing is running")
        return
    print(f"piece  {state['piece']}, loop {' to '.join(state.get('loop', []))}")
    print(f"server pid {state['server_pid']}, port {state['port']}: "
          f"{'running' if alive(state['server_pid']) else 'NOT RUNNING'}")
    print(f"client pid {state['client_pid']}: "
          f"{'running' if alive(state.get('client_pid')) else 'NOT RUNNING'}")
    print(f"\n{SERVER_LOG.name}:\n{tail(SERVER_LOG, 5)}\n\n{CLIENT_LOG.name}:\n{tail(CLIENT_LOG, 8)}")


def cmd_new(args: argparse.Namespace) -> None:
    """`pg new <name> [--from piece]`: copy a piece to start a new one.

    Args:
        args: The parsed command line.

    Raises:
        PgError: on any failure, with the reason; prints progress to stdout.
    """
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", args.name):
        raise PgError(f"'{args.name}' is not a plain name")
    source, target = PIECES / args.source, PIECES / args.name
    if not source.is_dir():
        raise PgError(f"no piece '{args.source}'")
    if target.exists():
        raise PgError(f"{target} already exists")
    shutil.copytree(source, target)
    settings = target / f"{args.name}.yaml"
    (target / f"{args.source}.yaml").rename(settings)
    old = read_piece(args.name)["score"]
    new = Path(old).with_name(f"{args.name}.yaml")
    text = Path(old).read_text(encoding="utf-8")
    text = re.sub(r"(?m)^(\s*name:\s*)\S+", rf"\g<1>{args.name}", text, count=1)
    new.write_text(text, encoding="utf-8")
    Path(old).unlink()
    rel = new.relative_to(target).as_posix()
    settings.write_text(
        re.sub(r"(?m)^score:.*$", f"score: {rel}", settings.read_text()), encoding="utf-8"
    )
    print(f"pg: made {target.relative_to(PLAYGROUND)}; pg start {args.name}")


def main() -> int:
    """Parses the command line and runs one command.

    Returns:
        0 on success, 1 on a reported failure.
    """
    parser = argparse.ArgumentParser(prog="pg", description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser("start", help="compile a piece and start looping it")
    start.add_argument("piece", nargs="?", default="beep")
    start.set_defaults(run=cmd_start)
    commands.add_parser("reload", help="recompile and play the new version").set_defaults(
        run=cmd_reload)
    commands.add_parser("stop", help="stop playback and the server").set_defaults(run=cmd_stop)
    commands.add_parser("status", help="show what is running").set_defaults(run=cmd_status)
    new = commands.add_parser("new", help="start a new piece as a copy of another")
    new.add_argument("name")
    new.add_argument("--from", dest="source", default="beep", help="piece to copy (default beep)")
    new.set_defaults(run=cmd_new)
    args = parser.parse_args()
    try:
        args.run(args)
    except PgError as error:
        print(f"pg: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
