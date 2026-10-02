# rendering-engine — server mode

**Status:** implemented, first version. Three calls, plain HTTP, no
authentication, one call at a time.

`server` mode keeps the engine running between operations, with the loaded
score in memory, and takes its operations as REST calls. It runs the same
steps as `batch` mode ([design.md](design.md) §3.1), through the same code
(`score_pipeline.hpp`); only the ingress and the lifetime differ.

The server is meant to sit deep in a network, behind an API gateway in any
real deployment. It does nothing about authentication, authorisation, TLS,
rate limiting or hostile clients, and its error messages hide nothing: they
name paths and quote the engine's diagnostics in full. The one protection it
has is `--input-root`, which limits the files a `/load` can read.

## 1. Starting it

```
rendering-engine --mode server --host <address> --port <port>
                 --input-root <dir> --out-dir <dir>
                 [--schema <file.cypher>] [--validate-db] [--per-set-wav] [--verbose]
```

All four of `--host`, `--port`, `--input-root` and `--out-dir` are required;
the server will not guess which interface to expose or which files it may
read. `--port 0` asks the operating system for a free port. `--input-root`
must be an existing directory. `--out-dir` is created at startup, so a server
that cannot write fails before it listens. `--state`, `--request` and
`--db-path` are refused: a score arrives by `/load`, frames are asked for by
`/render`, and `engine-db` is always in memory.

`--schema`, `--validate-db`, `--per-set-wav` and `--verbose` mean what they mean
in batch mode, and apply to every call.

Once it can accept connections the server prints its settings and a line

```
  listening on http://127.0.0.1:8080
```

with the real port when `--port 0` was given. Scripts wait for that line. From
then on, each call's narrative is printed to standard output as it is answered.

`make run-rendering-engine-server` starts one on `127.0.0.1:8080`, with the
repository as its input root.

## 2. The calls

Every call is a `POST` with a JSON body, and every response is a JSON object
with at least these keys:

| Key | Meaning |
| --- | --- |
| `status` | `"ok"` or `"error"` |
| `message` | One line: what happened, or why not |
| `log` | The call's narrative, one string per line, exactly as batch mode prints it. It holds the `FAILED` lines when something did not apply |

Anything else (an unknown path, a `GET`) gets 404 with the same shape.

### 2.1 `POST /load`

```json
{"uri": "file:///Users/me/scores/demo.cypher"}
```

Discards the score the server holds, then loads, transforms and precomputes
the named Cypher file into a fresh in-memory `engine-db`. This is batch mode's
loading sequence, `--state` and all.

| Status | When | Extra keys |
| --- | --- | --- |
| 200 | The score loaded, transformed and precomputed | `uri` (the resolved file), `nodes`, `relationships` (row totals), `partials` |
| 400 | The body is not a JSON object with exactly a string `uri`, or the URI is not one §3 accepts | |
| 403 | The file is outside `--input-root` | |
| 404 | No regular file at that path | |
| 422 | The load stopped short: a statement did not apply, an integrity check failed (`--validate-db`), or the transform or precomputation failed. `message` is batch mode's closing verdict | |
| 500 | Anything else, e.g. the file could not be read | |

**What the server holds afterwards.** A call refused with 400, 403 or 404
never reaches the score, and the score loaded before it stays. A call that
gets as far as loading discards that score first; if the load then fails, with
422 or 500, the server holds **no** score, and `/render` answers 409 until a
load succeeds. A score is never half-replaced, and a client that asked for a
new score never renders the old one by mistake.

### 2.2 `POST /render`

```json
{"frame": 3, "labels": ["strings", "brass"]}
```

Renders one frame of the loaded score to `--out-dir/frame-NNNNNN.wav`. The
body follows one entry of the batch request ([design.md](design.md) §3.2):
`frame` is an integer from 0 to 2147483647 (not `3.0`, not `"3"`); `labels` is
optional, and omitted, `null` and `[]` all mean every partial. No other key is
accepted, and every problem is reported in `log` together. Labels must exist in
`engine-db`, but in this version they do not select partials: every partial
in the frame's scope is rendered, as in batch mode.

| Status | When | Extra keys |
| --- | --- | --- |
| 200 | The frame was rendered | `frame`, `uri` (the WAV file), and `set_uris` (each contributing set's file) under `--per-set-wav` |
| 400 | The body is not such an object | |
| 409 | No score is loaded | |
| 422 | A label is not in `engine-db`, or the frame could not be rendered | |
| 500 | `--out-dir` cannot be created, or anything else | |

Rendering a frame again replaces its file. So does rendering the same frame
number from a different score, or with a different label set; file names will
need to say more once labels select partials. Files are never removed, not
even at shutdown.

### 2.3 `POST /shutdown`

No body. Answers 200, `{"status": "ok", "message": "shutting down"}`, then
stops accepting connections, and the process exits 0. The answer is written
before the server stops.

## 3. URIs

The server names files by URI, so that its callers do not change when frames
go to object storage and come back as `https://` or `s3://` URIs instead. For
now every URI is a local `file://` one (`file_uri.hpp`):

- **Accepted:** `file://` followed by an absolute path, with an empty or
  `localhost` authority, percent-encoded as RFC 3986 requires (`%20` for a
  space). No query, no fragment.
- **Returned:** `file://` and the absolute path, with every byte other than
  letters, digits, `-`, `.`, `_`, `~` and `/` percent-encoded.

**The input root.** A `/load` path is resolved — `..` removed, symbolic links
followed — before it is checked, and must then lie within `--input-root`,
itself resolved at startup. So neither `../..` nor a symbolic link inside the
root can lead out of it.

## 4. One call at a time

The server handles each connection on the thread that accepted it, start to
finish, before it accepts the next. There is no worker thread and no pool,
and each connection carries one request; a client cannot hold the server by
keeping a connection open. A long `/load` delays every call behind it, which
is the intended trade: one `engine-db` and one score, and no locking, no
half-loaded state visible to a concurrent render, and no question of what two
loads at once would mean. See
[ADR 0016](../../docs/adr/0016-serve-rest-with-cpp-httplib-one-call-at-a-time.md).

## 5. Example

```bash
make run-rendering-engine-server &

curl -s -X POST localhost:8080/load \
     -d "{\"uri\": \"file://$PWD/engine-db/examples/tempo-map-score.cypher\"}"
curl -s -X POST localhost:8080/render -d '{"frame": 1}'
#  {"frame":1,"log":[...],"message":"rendered file:///…/frame-000001.wav",
#   "status":"ok","uri":"file:///…/frame-000001.wav"}
curl -s -X POST localhost:8080/shutdown
```

## 6. Code

| File | Contents |
| --- | --- |
| `server.hpp` / `server.cpp` | `ServerCore`, which turns a request body into a status and a JSON body with no HTTP involved, and `runServer()`, which serves it over HTTP. `server.cpp` is the only file that includes cpp-httplib and nlohmann/json |
| `score_pipeline.hpp` | `loadScore()` and `renderRequestedFrames()`: the steps both modes run |
| `report.hpp` | The narrative, written to any stream: standard output in batch mode, the response's `log` here |
| `file_uri.hpp` | `file://` URIs to and from paths |

Tests: `test_server.cpp` drives `ServerCore` directly, covering every status
above. `test/server/run_server_check.py` (CTest `server.*`) starts the real
binary on a free port, loads an example score over HTTP, renders every frame
of its request, checks each returned file with the rendering oracle, and
shuts the server down.

## 7. Not yet

- **Labels select nothing**, as in batch mode ([rendering.md](rendering.md) §2).
- **No source version.** A request renders whichever score was loaded last.
- **No delta.** Every `/load` rebuilds everything; a later version will compare
  the new score with the one held and recompute only what changed, which is
  the dirty-flag work of [design.md](design.md) §4.
- **Local files only.** Object storage, and with it `distributed` mode, are
  later.
