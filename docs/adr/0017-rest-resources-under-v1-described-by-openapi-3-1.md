---
status: accepted
date: 2026-10-02
decision-makers: Alan Broady
consulted: Claude (design discussion)
---

# REST resources under /v1, described by OpenAPI 3.1, with problem+json errors

## Context and Problem Statement

Server mode ([ADR 0016](0016-serve-rest-with-cpp-httplib-one-call-at-a-time.md))
started with three calls, all `POST` with JSON bodies: `/load`, `/render` and
`/shutdown`. Every response, success or failure, was an envelope
`{status, message, log}` carrying the call's whole narrative.

The next calls are reads. A client needs to discover the score's tempo maps
(a graph rooted at the Score's TempoMap, joined by
`TEMPO_MAP_SYNCHRONIZATION_POINT`), page through a map's points, read the
engine parameters, and ask where a `beats:ticks` position falls as a frame and
an offset. The author also wants the API described by an OpenAPI document,
ready for a platform such as SwaggerHub, though nothing is published yet.

So the question is what conventions the API follows, now that it is growing,
and whether the three existing calls should change to match.

## Decision Drivers

* Reads should be safe, idempotent and cacheable in principle: `GET` on a
  resource, not `POST` of a query.
* The API must be describable by a standard document that tools can import.
* Paging must be stateless: no server-side cursor, and the same page whenever
  it is asked for, as long as the same score is loaded.
* One set of conventions across every call. Two styles in one small API
  would have to be explained forever.
* Errors should be machine-readable in a standard shape.

## Considered Options

* Keep the existing style for the new calls: `POST` bodies and the envelope.
* Resource-style `GET` calls for the new reads, and leave the three existing
  calls as they are.
* Resource style for every call, moving the existing three now.

## Decision Outcome

Chosen option: **resource style for every call, moving the existing three
now**, because the API has one client so far (the end-to-end test), so a
breaking change costs least now, before anything depends on the old shape.

* **Paths under `/v1`.** The score is a resource, `PUT /v1/score` (a load
  replaces it, and loading the same file again gives the same score). Rendering
  is `POST /v1/frames/{frame}/renderings`, with an optional body carrying
  `labels`. The reads are `GET /v1/tempo-maps/root`,
  `/v1/tempo-maps/{id}/children`, `/v1/tempo-maps/{id}/points`,
  `/v1/tempo-maps/{id}/frame-position` and `/v1/engine-config`.
  `POST /v1/shutdown` stays a control call.
* **Successes are the resource itself**, as `application/json`, with no
  envelope.
* **Failures are RFC 9457 problem documents** (`application/problem+json`),
  with `type` `about:blank`, the reason phrase as `title`, the one-line verdict
  as `detail`, and an `errors` extension listing each problem when a request
  has several. A known path asked with the wrong method gets 405 and `Allow`.
* **The narrative goes to standard output only.** Responses no longer carry
  `log`. A client gets the verdict, and an operator watching the server gets
  everything, as before.
* **OpenAPI 3.1.1** (`rendering-engine/api/openapi.yaml`), written by hand.
  3.1 aligns schemas with JSON Schema 2020-12, and import tools such as
  SwaggerHub support it. 3.2.0 is the latest release, but tool support for it
  is still thin, and nothing in this API needs it.
* **Points are paged by position**: `after` is a position in ticks
  (exclusive), `limit` is 1 to 1000 (default 100), and `next` is the relative
  URL of the following page, or null. Positions strictly increase along a
  map, so the cursor is total and needs no server state. A map's children are
  not paged. Few maps have many children, and paging them later needs a key to
  order on. The plan is a junction node with a SERIAL key.
* **Ids and cursors are valid for the loaded score only.** Ids are assigned
  on every load. Versioning of scores, and with it ids that outlive a load, is
  later work.

### Consequences

* Good, because every call follows one documented convention, and the
  document can be imported into SwaggerHub or a code generator unchanged.
* Good, because reads are plain `GET`s that a gateway can route, log and
  cache by URL.
* Good, because errors have a standard shape that generic clients understand.
* Bad, because the change broke the three existing calls. Their only client
  was this repository's own test, which moved with them.
* Bad, because the narrative is no longer in the response. A client that
  wants the FAILED lines of a load must read the server's output.
* Neutral, because paging a map's points walks its list from the head on
  every page (the list is a linked list, and positions are not indexed). That
  is acceptable now, and it is recorded in `rendering-engine/docs/server.md`.

### Confirmation

CTest `server.openapi` validates the document against OpenAPI 3.1 and checks
every example in it against its schema. The `server.*` end-to-end tests check
every live response body against the document's schema for that path, method
and status. Both checks need PyYAML, jsonschema and openapi-spec-validator.
Without them, `server.openapi` is skipped, and the end-to-end tests run
without the schema check. `test_server.cpp` checks every status and both
media types through `ServerCore`.

## Pros and Cons of the Options

### Keep the existing style

* Good, because nothing breaks.
* Bad, because reads become `POST`s, and the document would describe an RPC
  API in REST clothing.

### New calls resource-style, old calls unchanged

* Good, because nothing breaks.
* Bad, because the API has two conventions, and two error shapes, from its
  first extension onward.

### Resource style for every call (chosen)

* Good, because there is one convention, adopted while it is cheapest.
* Bad, because it is a breaking change.

## More Information

`rendering-engine/api/openapi.yaml` is the authoritative description of the
calls. `rendering-engine/docs/server.md` explains them and their limits.
`rendering-engine/include/rainbohz/rendering_engine/transform/timing_query.hpp` holds
the queries behind the reads.
