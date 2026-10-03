# playback-client

**Status: first version.** A command-line client that plays a range of a
score, in a loop, through the operating system's default audio device. Every
loop is rendered afresh: one lane of an on-disk buffer is refilled over REST
while the other plays. It is kept as a demonstration and as a test component;
more advanced playback components will come later.

This directory holds the component identified as `playback-client` in
[architecture/overview.md](../architecture/overview.md) §5.3. The architecture
places it as a client of `playback-server`. That server does not exist yet, so
this version talks to `rendering-engine`'s REST service directly
([rendering-engine/docs/server.md](../rendering-engine/docs/server.md)),
standing in for it. See [docs/design.md](docs/design.md) for the design: the
lane protocol, the race conditions it closes, the audio path and the log.

## Using it

Start a server and load a score, then play a range of it:

```bash
make run-rendering-engine-server &
curl -s -X PUT localhost:8080/v1/score \
     -d "{\"uri\": \"file://$PWD/engine-db/examples/tempo-map-score.cypher\"}"

make run-playback-client PC_START=1:0 PC_END=6:0 PC_LOOPS=3 PC_ARGS=--debug
```

or directly:

```
playback-client --start B:T --end B:T
                [--server http://127.0.0.1:8080/v1] [--loops 1]
                [--buffer-dir <dir>] [--keep-buffer]
                [--output-wav <file>] [--debug]
```

The range is `[start, end)` on the score's root tempo map, written as
`beats:ticks` or bare beats. The client must run where it can read the
server's `--out-dir`: a render returns a `file://` URI, which stands in for an
object-storage URL, and the client copies the file. `--output-wav` writes what
would have been played to a file instead of the device. The log goes to
standard error. Each fill's line gives its real-time factor; below 1,
rendering keeps up.

## Building and testing

From the repository root:

| Target | What it does |
| --- | --- |
| `make playback-client` | Configure and build, with the same presets as `rendering-engine` |
| `make test-playback-client` | Build the client and the engine, then run the unit tests and the end-to-end test |
| `make run-playback-client` | Play `PC_START` to `PC_END`, `PC_LOOPS` times, from `PC_SERVER` |
| `make format-playback-client` / `make tidy-playback-client` | `.clang-format` in place / `clang-tidy` |
| `make api-docs-playback-client` | Doxygen HTML under `build/doxygen` |
| `make clean-playback-client` | Remove the build tree |

Dependencies are built from pinned source, as rendering-engine's are: CLI11,
cpp-httplib, nlohmann/json and dr_wav, at the same pins, plus miniaudio for
audio output ([ADR 0018](../docs/adr/0018-play-audio-with-miniaudio.md)). No
third-party type appears in a public header.

## Layout

| Path | Contents |
| --- | --- |
| `include/rainbohz/playback_client/` | Public headers: units, positions, frame plan, `PlaybackApi`, `ObjectFetcher`, `LaneBoard`, the workers, sinks, session |
| `src/` | Implementations. `playback_api.cpp` alone includes httplib and JSON, `device_sink.cpp` miniaudio, `wav_io.cpp` dr_wav |
| `test/` | GoogleTest unit tests, with fakes in `test_support.hpp`; `e2e/run_playback_check.py` |
| `docs/design.md` | The design |
