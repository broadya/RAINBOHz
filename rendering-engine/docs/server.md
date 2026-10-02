# rendering-engine — server mode

**Status:** implemented. Eight calls under `/v1`, plain HTTP, no
authentication, one call at a time. The calls are specified by the OpenAPI
3.1.1 document [`api/openapi.yaml`](../api/openapi.yaml), which is
authoritative where it and this page differ. The conventions are
[ADR 0017](../../docs/adr/0017-rest-resources-under-v1-described-by-openapi-3-1.md).

`server` mode keeps the engine running between operations, with the loaded
score in memory, and takes its operations as REST calls. It runs the same
steps as `batch` mode ([design.md](design.md) §3.1), through the same code
(`score_pipeline.hpp`); only the ingress and the lifetime differ. Beyond what
batch mode does, it answers questions about the loaded score's timing.

The server is meant to sit deep in a network, behind an API gateway in any
real deployment. It does nothing about authentication, authorisation, TLS,
rate limiting or hostile clients, and its errors hide nothing: they name paths
and quote the engine's diagnostics. The one protection it has is
`--input-root`, which limits the files a score load can read.

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
`--db-path` are refused: a score arrives by `PUT /v1/score`, frames are asked
for one call at a time, and `engine-db` is always in memory.

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

## 2. Conventions

| | |
| --- | --- |
| Paths | All under `/v1` |
| Success | The resource itself, `application/json` |
| Failure | An RFC 9457 problem document, `application/problem+json` |
| Unknown path | 404, as a problem |
| Known path, other method | 405, as a problem, with `Allow` |
| Query parameters | Only those a call names, each at most once; anything else is 400 |
| Ids and cursors | Valid for the loaded score only (§2.2) |

### 2.1 Problems

```json
{"type": "about:blank", "title": "Bad Request", "status": 400,
 "detail": "the request is not usable",
 "errors": ["frame '-1' must be an integer from 0 to 2147483647",
            "unknown key 'extra'"]}
```

`title` is the status's reason phrase, and `detail` is the call's verdict in
one line: for a failed load, batch mode's closing verdict. Where a request has
several things wrong, `errors` lists every one, so a client can fix them
together. The call's full narrative, including the `FAILED` lines of a load
that stopped short, goes to standard output and not to the response.

### 2.2 Ids are per load

TempoMap ids are the SERIAL keys `engine-db` assigns when a score is loaded.
Every `PUT /v1/score` builds a fresh `engine-db`, so ids are assigned afresh,
and an id or a `next` link from before a load may name a different map, or
none, after it. Within one loaded score every read is stateless and
idempotent: the same call gives the same answer. Versioning of scores, which
would let an id outlive a load, is later work (§8).

## 3. The calls

### 3.1 `PUT /v1/score`

```json
{"uri": "file:///Users/me/scores/demo.cypher"}
```

Discards the score the server holds, then loads, transforms and precomputes
the named Cypher file into a fresh in-memory `engine-db`. This is batch mode's
loading sequence, `--state` and all.

| Status | When | Body |
| --- | --- | --- |
| 200 | The score loaded, transformed and precomputed | `uri` (the resolved file), `nodes`, `relationships` (row totals), `partials` |
| 400 | The body is not a JSON object with exactly a string `uri`, or the URI is not one §4 accepts | problem |
| 403 | The file is outside `--input-root` | problem |
| 404 | No regular file at that path | problem |
| 422 | The load stopped short: a statement did not apply, an integrity check failed (`--validate-db`), or the transform or precomputation failed | problem |
| 500 | Anything else, e.g. the file could not be read | problem |

**What the server holds afterwards.** A call refused with 400, 403 or 404
never reaches the score, and the score loaded before it stays. A call that
gets as far as loading discards that score first; if the load then fails, with
422 or 500, the server holds **no** score, and every call that needs one
answers 409 until a load succeeds. A score is never half-replaced, and a client
that asked for a new score never renders the old one by mistake.

### 3.2 `POST /v1/frames/{frame}/renderings`

```
POST /v1/frames/3/renderings
{"labels": ["strings", "brass"]}
```

Renders one frame of the loaded score to `--out-dir/frame-NNNNNN.wav`. The
call carries one entry of the batch request ([design.md](design.md) §3.2):
`{frame}` is an integer from 0 to 2147483647, digits only (not `3.0`, not
`+3`); the body is optional, and no body, `{}`, and `labels` omitted, `null`
or `[]` all mean every partial. No other key is accepted, and every problem is
listed in `errors`. Labels must exist in `engine-db`, but in this version they
do not select partials: every partial in the frame's scope is rendered, as in
batch mode.

| Status | When | Body |
| --- | --- | --- |
| 200 | The frame was rendered | `frame`, `uri` (the WAV file), and `set_uris` (each contributing set's file) under `--per-set-wav` |
| 400 | The frame or the body is unusable | problem |
| 409 | No score is loaded | problem |
| 422 | A label is not in `engine-db`, or the frame could not be rendered | problem |
| 500 | `--out-dir` cannot be created, or anything else | problem |

Rendering a frame again replaces its file. So does rendering the same frame
number from a different score, or with a different label set; file names will
need to say more once labels select partials. Files are never removed, not
even at shutdown.

### 3.3 `POST /v1/shutdown`

No body. Answers 200, `{"message": "shutting down"}`, then stops accepting
connections, and the process exits 0. The answer is written before the server
stops.

### 3.4 The tempo maps

A score's tempo maps form a graph. Its root is the TempoMap of the Score
(`Score -[:NEXT_POINT]-> TempoMap`), which defines time zero. A map's children
are the maps with a `TEMPO_MAP_SYNCHRONIZATION_POINT` edge to it (the edge runs
from child to parent). The API lists children everywhere, so a child that has
several parents, in a DAG, needs no change to it. Such a child is listed under
each parent, each time with that edge's synchronisation points. In this version
the transform refuses any synchronisation edge, so a loaded score's root has no
children. The calls already read them.

Every call in this section answers 409 when no score is loaded, and 400 for
an `{id}` that is not a non-negative integer or for an unknown query
parameter. A map is `{id, name, ticks_per_beat}`. A child is a map plus
`sync_point_parent` (where, in the parent's ticks, the child is rooted) and
`sync_point_child` (the alignment point in the child's own ticks). Properties
that are NULL in `engine-db` are `null`.

#### `GET /v1/tempo-maps/root`

The root map with its `children`, in child id order.

```json
{"id": 0, "name": "Main", "ticks_per_beat": 960, "children": []}
```

404 when there is no Score, or the Score has no TempoMap, as in a score
written in physical time. 422 when there is more than one Score.

#### `GET /v1/tempo-maps/{id}/children`

`{"tempo_map_id": 0, "children": [...]}`, in child id order. Not paged: few maps
have many children, and paging them needs a key to order on. The plan is a
junction node with a SERIAL key (§8). 404 for an unknown map.

#### `GET /v1/tempo-maps/{id}/points?after=…&limit=…`

A page of the map's points, in list order, which is strictly increasing
position:

```json
{"tempo_map_id": 0, "ticks_per_beat": 960,
 "points": [{"position": 0, "start_tempo": 120.0, "hold": false}],
 "next": "/v1/tempo-maps/0/points?after=0&limit=1"}
```

`after` is a position in ticks: only later points are returned, so it is
exclusive, and it need not be the position of a point. Omitted, the page
starts at the first point. `limit` is from 1 to 1000, 100 when omitted.
`next` is the relative URL of the following page, which is this call with
`after` moved to the page's last position, or `null` when this page ends the
list. Follow it until it is null. Because the cursor is a position, the pages
need no server state, and a page asked for twice is the same page.

404 for an unknown map. 422 when the list cannot be paged by position: a point
before the end of the page has no position, positions do not strictly
increase, or the list loops back on itself.

#### `GET /v1/tempo-maps/{id}/frame-position?at=beats:ticks`

Where a musical position falls in the sample stream:

```json
{"tempo_map_id": 0, "at": "1:0", "frame": 0, "offset": 25638}
```

`at` is written as HzL writes positions
([hzl-reference.md](../../hzl-compiler/docs/hzl-reference.md)): `beats:ticks`,
two non-negative integers with the ticks below the map's `ticks_per_beat`, or a
bare number of beats. It comes back written in full.

The position is converted by the map to seconds (the formulas of
[transform.md](transform.md) §6.1, through the transform's own `TempoMap`), to
samples at the sample rate, and then **floored**. A position between two
samples gives the earlier, and a position exactly on a sample gives that sample:
the range is half-open, like a frame's. The sample is returned as the frame
holding it and its index within that frame, so that
`sample = frame × frame_size + offset`, with `frame_size` as
`GET /v1/engine-config` reports it.

The conversion is in double precision, and the floor is plain. A position
that is exactly on a sample in exact arithmetic can come out a hair below it,
and so land one sample early. A held tempo whose seconds are exact binary
fractions (60 BPM with as many ticks per beat as samples per second, at
positions such as 2.5 beats) gives the exact sample.

400 for an `at` that is not of that form, ticks at or beyond the map's
resolution, or a position beyond the last tick an INT64 can count. 404 for an
unknown map. 422 when the map does not convert. The map may not be a Score's
root: only root maps convert in this version, because the time of any other
map depends on synchronisation, which the transform does not implement yet.
The map may not be valid (transform.md §6.1). `EngineConfig` may be unusable.
Or the frame may not fit a frame number.

### 3.5 `GET /v1/engine-config`

The engine parameters, from the score's `EngineConfig`:

```json
{"sample_rate": 48000, "frame_size": 48000, "channel_mode": "mono"}
```

`frame_size` is resolved: when `EngineConfig.frameSize` is NULL, a frame is one
second, and `frame_size` is the sample rate. `channel_mode` is `mono`,
`stereo`, `mid-side`, `foa-a-format` or `foa-b-format` (channel modes 0 to 4,
as the schema numbers them). It reports what the score says; rendering
implements only `mono` in this version
([rendering.md](rendering.md) §9).

409 when no score is loaded. 422 when there is not exactly one `EngineConfig`,
its `sampleRate` is NULL or not positive, its frame size is unusable, or its
`channelMode` is NULL or not one of the five.

## 4. URIs

The server names files by URI, so that its callers do not change when frames
go to object storage and come back as `https://` or `s3://` URIs instead. For
now every URI is a local `file://` one (`file_uri.hpp`):

- **Accepted:** `file://` followed by an absolute path, with an empty or
  `localhost` authority, percent-encoded as RFC 3986 requires (`%20` for a
  space). No query, no fragment.
- **Returned:** `file://` and the absolute path, with every byte other than
  letters, digits, `-`, `.`, `_`, `~` and `/` percent-encoded.

**The input root.** A score's path is resolved, with `..` removed and
symbolic links followed, before it is checked. It must then lie within
`--input-root`, itself resolved at startup. So neither `../..` nor a symbolic
link inside the root can lead out of it.

## 5. One call at a time

The server handles each connection on the thread that accepted it, start to
finish, before it accepts the next. There is no worker thread and no pool,
and each connection carries one request; a client cannot hold the server by
keeping a connection open. A long score load delays every call behind it,
which is the intended trade: one `engine-db` and one score, and no locking, no
half-loaded state visible to a concurrent call, and no question of what two
loads at once would mean. See
[ADR 0016](../../docs/adr/0016-serve-rest-with-cpp-httplib-one-call-at-a-time.md).

## 6. Example

```bash
make run-rendering-engine-server &

curl -s -X PUT localhost:8080/v1/score \
     -d "{\"uri\": \"file://$PWD/engine-db/examples/tempo-map-score.cypher\"}"
curl -s localhost:8080/v1/tempo-maps/root
#  {"children":[],"id":0,"name":"Main","ticks_per_beat":960}
curl -s 'localhost:8080/v1/tempo-maps/0/points?limit=1'
#  {"next":"/v1/tempo-maps/0/points?after=0&limit=1","points":[...],...}
curl -s localhost:8080/v1/engine-config
#  {"channel_mode":"mono","frame_size":48000,"sample_rate":48000}
curl -s 'localhost:8080/v1/tempo-maps/0/frame-position?at=1:0'
#  {"at":"1:0","frame":0,"offset":25638,"tempo_map_id":0}
curl -s -X POST localhost:8080/v1/frames/1/renderings
#  {"frame":1,"uri":"file:///…/frame-000001.wav"}
curl -s -X POST localhost:8080/v1/shutdown
```

## 7. Code

| File | Contents |
| --- | --- |
| `api/openapi.yaml` | The OpenAPI 3.1.1 document: every path, parameter, schema and response, with examples |
| `server.hpp` / `server.cpp` | `ServerCore`, which turns a call's path segments, query parameters and body into a status and a JSON body with no HTTP involved, and `runServer()`, which routes HTTP to it. `server.cpp` is the only file that includes cpp-httplib and nlohmann/json |
| `transform/timing_query.hpp` | The read-only queries behind §3.4 and §3.5, with no HTTP or JSON, in the transform because they read musical time; the interface carries only plain integers and physical units: `readRootTempoMap()`, `readTempoMapChildren()`, `readTempoMapPoints()`, `readEngineParameters()`, `framePosition()` and `parseBeatsTicks()` |
| `score_pipeline.hpp` | `loadScore()` and `renderRequestedFrames()`: the steps both modes run |
| `report.hpp` | The narrative, written to any stream: standard output in batch mode and here |
| `file_uri.hpp` | `file://` URIs to and from paths |

**Reading a map's points.** A map's points are a `NEXT_POINT` list. Ladybug
refuses a variable-length pattern of more than 30 hops
([precomputation.md](precomputation.md) §10, "Reading the linked list"), so
the list is walked in chunks. The map's head is found by key, and then each
query follows up to 30 hops from the last point read, nearest first. A page
costs a walk from the head to its end. Each hop's far end is still found by a
scan of `TempoMapPoint`, which is Ladybug's parked edge-hop cost. Both are
accepted for now (§8).

**Tests.** `transform/test_timing_query.cpp` tests the parser, and every query against
the real schema with maps built in the test, including synchronisation edges,
which a loaded score cannot have yet, and lists longer than several chunks.
`test_server.cpp` drives `ServerCore` directly, covering every status and
both media types. `test/server/run_server_check.py` (CTest `server.*`) starts
the real binary on a free port. It checks 404, 405 and 409, loads an example
score over HTTP, reads its engine parameters, and pages through its root map's
points by following `next`. For the score in musical time, it checks the frame
position of each beat of a partial's knots against the oracle's independent
times. It then renders every frame of the request, checks each returned file
with the rendering oracle, and shuts the server down. When PyYAML and
jsonschema are installed, it also validates every response body against the
OpenAPI document. `test/server/openapi_check.py` (CTest `server.openapi`)
validates the document itself, and checks every example in it against its
schema. It is skipped unless openapi-spec-validator, jsonschema and PyYAML are
installed.

## 8. Not yet

- **No versioning or delta.** A call acts on whichever score was loaded last,
  and ids and cursors are valid only until the next load (§2.2). Proper
  versioning, with ids that outlive a load, is a large piece of later work. So
  is comparing a new score with the one held and recomputing only what
  changed, which is the dirty-flag work of [design.md](design.md) §4.
- **No conversion through synchronisation.** `frame-position` converts only a
  root map. Converting a child needs the relative scaling between
  synchronisation points that the schema describes, which the transform does
  not implement yet ([transform.md](transform.md) §13).
- **Children are not paged.** When a map can have many children, they will be
  ordered by a SERIAL key on a junction node and paged on it.
- **Paging points walks the list.** Each page walks from the head of the list,
  and each hop scans `TempoMapPoint` at its far end. Fine for tempo maps of
  realistic size; when it shows, an index on position or a cached walk is
  the fix.
- **Labels select nothing**, as in batch mode ([rendering.md](rendering.md) §2).
- **Local files only.** Object storage, and with it `distributed` mode, are
  later.
