#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""CTest driver: play a range of a real score through a real server, and check every sample.

Starts rendering-engine in server mode on a free port, then:

1. Runs playback-client before any score is loaded, and expects exit 1 with
   the hint to load a score first.
2. Loads the example score with PUT /v1/score.
3. Runs playback-client with an end before its start, and expects exit 2.
4. Runs playback-client with a --channel-mode the score does not have, and
   expects exit 1 naming the score's mode.
5. Runs playback-client over a range of several frames, for three loops,
   with --output-wav, and checks that the file is exactly the range's samples
   three times over: the frames the server rendered, trimmed at the
   positions the server reports, with no gap and nothing extra, in the
   score's channels. Checks too that the run's buffer directory was removed,
   and that the log has each fill's timing line.
6. Runs it again for two loops with --keep-buffer, and checks that both
   lanes were kept, each with every frame of the range.

The score decides the channels: CTest runs this once with a mono score and
once with a stereo one.

    run_playback_check.py --client <binary> --engine <binary> --schema <cypher>
        --score <cypher> --work-dir <dir>

Exit status 0 when every check holds, 1 on any failure, including the
rendering-engine binary not having been built. Standard library only.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import urllib.parse
import urllib.request

START_SECONDS = 30
CALL_SECONDS = 120
CLIENT_SECONDS = 240

# The channels of each mode the client plays; the server's names.
CHANNELS = {"mono": 1, "stereo-from-xy": 2, "stereo-from-mid-side": 2, "blumlein-classic": 2}

# The range played: beats 1:240 to 6:480. The tempo-map score's map ramps
# linearly in physical time to beat 4 at 2.67 s and holds 60 BPM after it, so
# the range is about 0.67 s to 5.17 s, frames 0 to 5 at one second a frame; the spatial musical score holds
# 60 BPM throughout, so it is 1.25 s to 6.5 s, frames 1 to 6. Either way the
# first and last frames are both trimmed.
START = "1:240"
END = "6:480"
LOOPS = 3


def start_server(args: argparse.Namespace, out_dir: str) -> tuple[subprocess.Popen, str]:
    """Start the engine in server mode on a free port.

    Returns:
        The process, and its base URL, e.g. http://127.0.0.1:50123.

    Raises:
        RuntimeError: If the server exits before reporting where it listens.
    """
    server = subprocess.Popen(
        [args.engine, "--mode", "server", "--schema", args.schema,
         "--host", "127.0.0.1", "--port", "0",
         "--input-root", os.path.dirname(os.path.abspath(args.score)),
         "--out-dir", out_dir],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    for line in server.stdout:
        match = re.search(r"listening on (http://\S+)", line)
        if match:
            # Keep draining, so the server never blocks on a full pipe.
            threading.Thread(target=lambda: [None for _ in server.stdout], daemon=True).start()
            return server, match.group(1)
    server.wait(timeout=START_SECONDS)
    raise RuntimeError(f"the server exited {server.returncode} before listening")


def call(base: str, method: str, path: str, body: dict | None = None) -> dict:
    """Make one REST call and return its JSON answer.

    Raises:
        urllib.error.HTTPError: On an error status.
    """
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {} if body is None else {"Content-Type": "application/json"}
    request = urllib.request.Request(base + "/v1" + path, data=data, method=method,
                                     headers=headers)
    with urllib.request.urlopen(request, timeout=CALL_SECONDS) as response:
        return json.loads(response.read())


def run_client(args: argparse.Namespace, base: str, *extra: str) -> subprocess.CompletedProcess:
    """Run playback-client against the server, capturing its log."""
    command = [args.client, "--server", base + "/v1", *extra]
    print("run_playback_check: running", " ".join(command))
    return subprocess.run(command, capture_output=True, text=True, timeout=CLIENT_SECONDS)


def read_float_wav(path: str) -> tuple[int, int, list[float]]:
    """Read a 32-bit float WAV file.

    Returns:
        The channel count, the sample rate, and the samples, interleaved.

    Raises:
        ValueError: If the file is not RIFF/WAVE 32-bit IEEE float.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a RIFF/WAVE file")
    position, channels, rate, samples = 12, None, None, None
    while position + 8 <= len(data):
        chunk, size = data[position:position + 4], struct.unpack_from("<I", data, position + 4)[0]
        body = data[position + 8:position + 8 + size]
        if chunk == b"fmt ":
            fmt, channels, rate = struct.unpack_from("<HHI", body)
            bits = struct.unpack_from("<H", body, 14)[0]
            if fmt == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: the real format is in the subformat
                fmt = struct.unpack_from("<H", body, 24)[0]
            if fmt != 3 or bits != 32:
                raise ValueError(f"{path} is format {fmt}, {bits} bits, not 32-bit float")
        elif chunk == b"data":
            samples = list(struct.unpack(f"<{size // 4}f", body))
        position += 8 + size + (size & 1)
    if channels is None or samples is None:
        raise ValueError(f"{path} has no fmt or data chunk")
    return channels, rate, samples


def check(condition: bool, what: str, failures: list[str]) -> None:
    """Record one expectation."""
    if not condition:
        print(f"run_playback_check: FAILED: {what}", file=sys.stderr)
        failures.append(what)


def range_positions(base: str) -> list[dict]:
    """The server's frame-position answers for START and END."""
    root = call(base, "GET", "/tempo-maps/root")
    positions = []
    for at in (START, END):
        query = urllib.parse.urlencode({"at": at})
        positions.append(call(base, "GET", f"/tempo-maps/{root['id']}/frame-position?{query}"))
    return positions


def range_frames(base: str) -> list[int]:
    """The frames the range needs: through END's frame, unless END is on its boundary."""
    start, end = range_positions(base)
    last = end["frame"] if end["offset"] > 0 else end["frame"] - 1
    return list(range(start["frame"], last + 1))


def expected_range(base: str, frames_dir: str) -> tuple[int, list[float]]:
    """The samples one pass over the range must give, from the server's own answers and files.

    Returns:
        The score's channel count, and the samples, interleaved.
    """
    config = call(base, "GET", "/engine-config")
    frame_size = config["frame_size"]
    width = CHANNELS[config["channel_mode"]]
    positions = range_positions(base)
    first = positions[0]["frame"] * frame_size + positions[0]["offset"]
    last = positions[1]["frame"] * frame_size + positions[1]["offset"]

    stream = []
    for frame in range(positions[0]["frame"], positions[1]["frame"] + 1):
        path = os.path.join(frames_dir, f"frame-{frame:06d}.wav")
        if not os.path.exists(path):
            call(base, "POST", f"/frames/{frame}/renderings")
        channels, rate, samples = read_float_wav(path)
        if (channels != width or rate != config["sample_rate"]
                or len(samples) != frame_size * width):
            raise ValueError(f"{path} is not a {width}-channel frame of {frame_size} "
                             f"at {config['sample_rate']}")
        stream.extend(samples)
    origin = positions[0]["frame"] * frame_size
    return width, stream[(first - origin) * width:(last - origin) * width]


def main() -> int:
    """Run the checks; see the module docstring."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--client", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--score", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    if not os.path.exists(args.engine):
        print(f"run_playback_check: FAILED: no rendering-engine at {args.engine}; "
              "build it with `make rendering-engine`", file=sys.stderr)
        return 1

    shutil.rmtree(args.work_dir, ignore_errors=True)
    frames_dir = os.path.join(args.work_dir, "frames")
    buffer_root = os.path.join(args.work_dir, "buffer")
    output_wav = os.path.join(args.work_dir, "played.wav")
    os.makedirs(buffer_root)

    failures: list[str] = []
    server, base = start_server(args, frames_dir)
    try:
        # 1. No score loaded.
        result = run_client(args, base, "--start", START, "--end", END,
                            "--output-wav", output_wav, "--buffer-dir", buffer_root)
        check(result.returncode == 1, f"with no score, exit {result.returncode}, not 1", failures)
        check("PUT /v1/score" in result.stderr, "with no score, no hint to load one", failures)

        # 2. Load the score.
        score_uri = "file://" + urllib.parse.quote(os.path.abspath(args.score), safe="/-._~")
        call(base, "PUT", "/score", {"uri": score_uri})

        # 3. A reversed range is a usage error.
        result = run_client(args, base, "--start", "6", "--end", "1:0", "--output-wav", output_wav)
        check(result.returncode == 2, f"reversed range, exit {result.returncode}, not 2", failures)

        # 4. A --channel-mode the score does not have.
        mode = call(base, "GET", "/engine-config")["channel_mode"]
        other = "stereo-from-xy" if mode == "mono" else "mono"
        result = run_client(args, base, "--start", START, "--end", END,
                            "--output-wav", output_wav, "--channel-mode", other)
        check(result.returncode == 1, f"--channel-mode {other} against {mode}, exit "
              f"{result.returncode}, not 1", failures)
        check(f"channel mode is {mode}" in result.stderr,
              f"--channel-mode {other}: no message naming {mode}", failures)

        # 5. Play three loops to a file.
        result = run_client(args, base, "--start", START, "--end", END, "--loops", str(LOOPS),
                            "--output-wav", output_wav, "--buffer-dir", buffer_root, "--debug")
        print(result.stderr, end="")
        check(result.returncode == 0, f"playing, exit {result.returncode}, not 0", failures)
        if result.returncode == 0:
            width, expected = expected_range(base, frames_dir)
            channels, _, played = read_float_wav(output_wav)
            check(channels == width, f"the output has {channels} channels, not {width}",
                  failures)
            check(len(played) == LOOPS * len(expected),
                  f"the output has {len(played)} samples, not {LOOPS} x {len(expected)}", failures)
            for loop in range(LOOPS):
                chunk = played[loop * len(expected):(loop + 1) * len(expected)]
                check(chunk == expected, f"loop {loop + 1} differs from the rendered range", failures)
            check(result.stderr.count("real-time factor") == LOOPS,
                  "the log does not have one timing line per fill", failures)
        check(os.listdir(buffer_root) == [], "the buffer directory was not removed", failures)

        # 6. Keep the buffer.
        result = run_client(args, base, "--start", START, "--end", END, "--loops", "2",
                            "--output-wav", output_wav, "--buffer-dir", buffer_root,
                            "--keep-buffer")
        check(result.returncode == 0, f"keeping the buffer, exit {result.returncode}", failures)
        runs = glob.glob(os.path.join(buffer_root, "rainbohz-playback-*"))
        check(len(runs) == 1, f"{len(runs)} run directories kept, not 1", failures)
        if len(runs) == 1:
            for lane in ("lane-a", "lane-b"):
                files = sorted(os.listdir(os.path.join(runs[0], lane)))
                check(files == [f"frame-{n:06d}.wav" for n in range_frames(base)],
                      f"{lane} holds {files}", failures)
    finally:
        try:
            call(base, "POST", "/shutdown")
            server.wait(timeout=START_SECONDS)
        except Exception:  # noqa: BLE001 -- the server is going either way
            server.kill()

    if failures:
        print(f"run_playback_check: {len(failures)} check(s) failed", file=sys.stderr)
        return 1
    print("run_playback_check: every check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
