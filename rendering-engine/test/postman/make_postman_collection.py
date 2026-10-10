#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Write the Postman collection that tests the server's REST API.

    make_postman_collection.py [--spec <openapi.yaml>] [--out <collection.json>]

The collection (Postman Collection Format v2.1) is run in order, in the
Postman Collection Runner or with newman, against a server started by
`make run-rendering-engine-server`. It drives every call of
rendering-engine/docs/server.md through every status a client can provoke with
the example scores, and checks each response against the OpenAPI document.
The response schemas are copied into the collection from the document, so
regenerate the collection whenever the document changes; the JSON file is
committed so that Postman users need no Python.

Postman validates with Ajv, which knows JSON Schema draft-07 and not the
OpenAPI dialect of 2020-12. The schemas used here need nothing from 2020-12,
so they are inlined (no $ref) and stripped of `format` and the example
keywords, which Ajv would otherwise reject or warn about.

Needs PyYAML.
"""

from __future__ import annotations

import argparse
import json
import os
import uuid
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SPEC = os.path.join(HERE, "..", "..", "api", "openapi.yaml")
DEFAULT_OUT = os.path.join(HERE, "rendering-engine.postman_collection.json")

# A fixed namespace, so that regenerating gives the same ids and a clean diff.
ID_NAMESPACE = uuid.UUID("6f1d3c52-6c38-4a4e-9d0b-2b3a8c1e5f70")

# The schemas the requests check their successful responses against.
RESPONSE_SCHEMAS = [
    "Problem", "LoadResult", "RenderResult", "ShutdownResult", "TempoMapWithChildren",
    "TempoMapChildren", "TempoMapPointPage", "FramePosition", "EngineConfig",
]

# Keywords Ajv does not need, and in strict mode refuses.
STRIPPED_KEYWORDS = {"format", "example", "examples"}

# The oracle's times, in samples, of the first partial's frequency envelope nodes in
# engine-db/examples/tempo-map-score.json, and the beats they fall on (as
# CTest server.tempo-map-score checks them). frame-position must give the
# floor of each, split by a frame size of 48000.
ORACLE_BEATS = [(0, 0.0), (2, 55234.957910741934), (4, 133084.2586675095), (8, 325084.25866750954)]
FRAME_SIZE = 48000

# Run before every request: the score URIs, built from repoRoot as the server
# encodes URIs (server.md §4), so that a returned URI can be compared exactly.
COLLECTION_PREREQUEST = r"""
const root = (pm.variables.get('repoRoot') || '').replace(/\/+$/, '');
if (!root.startsWith('/')) {
    throw new Error("Set the variable repoRoot to the repository's absolute, resolved path " +
        "(`pwd -P` at its top), which is the server's --input-root under make run-rendering-engine-server");
}
// Every byte but letters, digits, - . _ ~ and / percent-encoded.
const encode = (path) => path.split('/').map((segment) => encodeURIComponent(segment)
    .replace(/[!'()*]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase())).join('/');
const examples = root + '/engine-db/examples';
pm.variables.set('tempoMapScoreUri', 'file://' + encode(examples + '/tempo-map-score.cypher'));
pm.variables.set('tempoMapScoreUriUnresolved',
    'file://localhost' + encode(examples + '/../examples/tempo-map-score.cypher'));
pm.variables.set('minimumScoreUri', 'file://' + encode(examples + '/minimum-score.cypher'));
pm.variables.set('notAScoreUri', 'file://' + encode(root + '/README.md'));
pm.variables.set('missingFileUri', 'file://' + encode(examples + '/no-such-score.cypher'));
""".strip()

# Run after every request: the conventions of server.md §2.
COLLECTION_TEST = r"""
const contentType = pm.response.headers.get('Content-Type') || '';
if (pm.response.code < 400) {
    pm.test('a success is application/json', () => {
        pm.expect(contentType).to.match(/^application\/json/);
    });
} else {
    pm.test('a failure is an RFC 9457 problem (application/problem+json)', () => {
        pm.expect(contentType).to.match(/^application\/problem\+json/);
        pm.response.to.have.jsonSchema(JSON.parse(pm.collectionVariables.get('schemas')).Problem);
        const problem = pm.response.json();
        pm.expect(problem.status).to.equal(pm.response.code);
        pm.expect(problem.title).to.equal(pm.response.status);
    });
}
""".strip()


def resolve(node: Any, spec: dict) -> Any:
    """A schema with every $ref replaced by what it names, and the stripped keywords removed.

    Args:
        node: A schema, or any part of one.
        spec: The whole OpenAPI document, which the references point into.

    Returns:
        A copy of the node.
    """
    if isinstance(node, list):
        return [resolve(item, spec) for item in node]
    if not isinstance(node, dict):
        return node
    if "$ref" in node:
        target: Any = spec
        for part in node["$ref"].removeprefix("#/").split("/"):
            target = target[part]
        return resolve(target, spec)
    return {key: resolve(value, spec) for key, value in node.items()
            if key not in STRIPPED_KEYWORDS}


def script(listen: str, source: str) -> dict:
    """A Postman event running JavaScript source."""
    return {"listen": listen, "script": {"type": "text/javascript", "exec": source.strip().split("\n")}}


def request(name: str, method: str, path: str, tests: str, *, body: Any = None, raw: str | None = None,
            prerequest: str | None = None, description: str | None = None) -> dict:
    """A Postman request item.

    Args:
        name: The item's name, which says what is expected.
        method: The HTTP method.
        path: Everything after {{baseUrl}}, e.g. /v1/engine-config.
        tests: The test script.
        body: A JSON body, or None.
        raw: A body sent as it is, for bodies that are not JSON; or None.
        prerequest: A pre-request script, or None.
        description: Markdown shown with the request, or None.

    Returns:
        The item.
    """
    item: dict[str, Any] = {"name": name, "event": []}
    if prerequest:
        item["event"].append(script("prerequest", prerequest))
    item["event"].append(script("test", tests))
    req: dict[str, Any] = {"method": method, "header": [], "url": "{{baseUrl}}" + path}
    if body is not None or raw is not None:
        req["header"].append({"key": "Content-Type", "value": "application/json"})
        req["body"] = {"mode": "raw", "raw": raw if raw is not None else json.dumps(body, indent=2),
                       "options": {"raw": {"language": "json" if raw is None else "text"}}}
    if description:
        req["description"] = description
    item["request"] = req
    return item


def folder(name: str, description: str, items: list[dict]) -> dict:
    """A Postman folder."""
    return {"name": name, "description": description, "item": items}


def status(code: int) -> str:
    """A test that the status is `code`."""
    return f"pm.test('status is {code}', () => pm.response.to.have.status({code}));\n"


def fits(schema_name: str) -> str:
    """A test that the body fits a response schema of the OpenAPI document."""
    return (f"pm.test('the body fits the OpenAPI schema {schema_name}', () => {{\n"
            f"    pm.response.to.have.jsonSchema(JSON.parse(pm.collectionVariables.get('schemas')).{schema_name});\n"
            f"}});\n")


def detail(text: str) -> str:
    """A test that the problem's detail is exactly `text`."""
    return (f"pm.test({json.dumps('detail is ' + text)}, () => "
            f"pm.expect(pm.response.json().detail).to.equal({json.dumps(text)}));\n")


def errors(*texts: str) -> str:
    """A test that the problem's errors list is exactly `texts`, in order."""
    return (f"pm.test('errors lists every problem', () => "
            f"pm.expect(pm.response.json().errors).to.eql({json.dumps(list(texts))}));\n")


def allow(methods: str) -> str:
    """A test that the Allow header is `methods`."""
    return f"pm.test('Allow is {methods}', () => pm.response.to.have.header('Allow', '{methods}'));\n"


def body_equals(value: Any) -> str:
    """A test that the JSON body equals `value`."""
    return (f"pm.test({json.dumps('the body is ' + json.dumps(value))}, () => "
            f"pm.expect(pm.response.json()).to.eql({json.dumps(value)}));\n")


NO_SCORE = status(409) + detail("no score is loaded; PUT /v1/score first")


def no_score_folder() -> dict:
    """Every call that needs a score, with none loaded."""
    return folder("1 No score", (
        "A load that fails discards the score held before it, so after it every call that needs a score "
        "answers 409 (server.md §3.1). The repository's README is inside the input root but is not Cypher, "
        "so loading it fails with 422. This puts the server in a known state however the run starts."
    ), [
        request("Load a file that is not a score: 422", "PUT", "/v1/score",
                status(422) + detail("1 statement(s) did not apply. engine-db is partially loaded."),
                body={"uri": "{{notAScoreUri}}"}),
        request("Render, no score: 409", "POST", "/v1/frames/0/renderings", NO_SCORE),
        request("Root tempo map, no score: 409", "GET", "/v1/tempo-maps/root", NO_SCORE),
        request("Tempo map children, no score: 409", "GET", "/v1/tempo-maps/0/children", NO_SCORE),
        request("Tempo map points, no score: 409", "GET", "/v1/tempo-maps/0/points", NO_SCORE),
        request("Frame position, no score: 409", "GET", "/v1/tempo-maps/0/frame-position?at=1", NO_SCORE),
        request("Engine config, no score: 409", "GET", "/v1/engine-config", NO_SCORE),
    ])


def routing_folder() -> dict:
    """Unknown paths and wrong methods."""
    return folder("2 Routing", "An unknown path is 404; a known path asked with another method is 405, with Allow.", [
        request("Unknown path: 404", "GET", "/v1/no-such-endpoint",
                status(404) + detail("no endpoint GET /v1/no-such-endpoint; the endpoints are described by api/openapi.yaml")),
        request("A path outside /v1: 404", "GET", "/tempo-maps/root", status(404)),
        request("GET /v1/score: 405", "GET", "/v1/score",
                status(405) + allow("PUT") + detail("/v1/score does not take GET; it takes PUT")),
        request("DELETE a rendering: 405", "DELETE", "/v1/frames/1/renderings", status(405) + allow("POST")),
        request("POST /v1/engine-config: 405", "POST", "/v1/engine-config", status(405) + allow("GET")),
        request("GET /v1/shutdown: 405", "GET", "/v1/shutdown", status(405) + allow("POST")),
    ])


def physical_time_folder() -> dict:
    """A score with no tempo map."""
    return folder("3 Score in physical time", (
        "engine-db/examples/minimum-score.cypher is written in physical time: it loads and renders, "
        "but its Score has no TempoMap, so there is no root map."
    ), [
        request("Load minimum-score: 200", "PUT", "/v1/score",
                status(200) + fits("LoadResult") + (
                    "pm.test('the URI and partials are the score\\'s', () => {\n"
                    "    const body = pm.response.json();\n"
                    "    pm.expect(body.uri).to.equal(pm.variables.get('minimumScoreUri'));\n"
                    "    pm.expect(body.partials).to.equal(1);\n"
                    "});\n"),
                body={"uri": "{{minimumScoreUri}}"}),
        request("Root tempo map of a score with none: 404", "GET", "/v1/tempo-maps/root",
                status(404) + detail("Score 0 has no TempoMap")),
        request("Engine config: 200", "GET", "/v1/engine-config",
                status(200) + fits("EngineConfig")
                + body_equals({"channel_mode": "mono", "frame_size": 48000, "sample_rate": 48000})),
        request("Render frame 0: 200", "POST", "/v1/frames/0/renderings",
                status(200) + fits("RenderResult") + (
                    "pm.test('frame 0 is written to frame-000000.wav', () => {\n"
                    "    const body = pm.response.json();\n"
                    "    pm.expect(body.frame).to.equal(0);\n"
                    "    pm.expect(body.uri).to.match(/^file:\\/\\/\\/.*\\/frame-000000\\.wav$/);\n"
                    "});\n")),
    ])


def load_folder() -> dict:
    """Loading the score in musical time, and loads refused without touching it."""
    still_loaded = request("The score is still loaded: 200", "GET", "/v1/tempo-maps/root",
                           status(200) + "pm.test('the root map is Main', () => "
                           "pm.expect(pm.response.json().name).to.equal('Main'));\n")
    return folder("4 Load the score in musical time", (
        "engine-db/examples/tempo-map-score.cypher, named with a `localhost` authority and a `..` "
        "segment, both of which the server resolves. Loads refused with 400, 403 or 404 never reach the "
        "score, so it is still loaded after them (server.md §3.1)."
    ), [
        request("Load tempo-map-score: 200", "PUT", "/v1/score",
                status(200) + fits("LoadResult") + (
                    "pm.test('the URI comes back resolved', () => "
                    "pm.expect(pm.response.json().uri).to.equal(pm.variables.get('tempoMapScoreUri')));\n"
                    "pm.test('the score has two partials and some rows', () => {\n"
                    "    const body = pm.response.json();\n"
                    "    pm.expect(body.partials).to.equal(2);\n"
                    "    pm.expect(body.nodes).to.be.above(0);\n"
                    "    pm.expect(body.relationships).to.be.above(0);\n"
                    "});\n"),
                body={"uri": "{{tempoMapScoreUriUnresolved}}"}),
        request("Load, body not JSON: 400", "PUT", "/v1/score",
                status(400) + detail("the body is not a JSON object"), raw="not json"),
        request("Load, no uri: 400", "PUT", "/v1/score", status(400) + errors("missing key 'uri'"), body={}),
        request("Load, uri not a string: 400", "PUT", "/v1/score", status(400) + errors("'uri' is not a string"),
                body={"uri": 7}),
        request("Load, unknown key: 400", "PUT", "/v1/score", status(400) + errors("unknown key 'extra'"),
                body={"uri": "{{tempoMapScoreUri}}", "extra": 1}),
        request("Load, not a file:// URI: 400", "PUT", "/v1/score",
                status(400) + detail("'https://example.com/score.cypher' is not a file:// URI"),
                body={"uri": "https://example.com/score.cypher"}),
        request("Load, outside the input root: 403", "PUT", "/v1/score", status(403),
                body={"uri": "file:///etc/hosts"}),
        request("Load, no such file: 404", "PUT", "/v1/score", status(404), body={"uri": "{{missingFileUri}}"}),
        still_loaded,
    ])


def tempo_maps_folder() -> dict:
    """The root map, its children, and its points, whole and paged."""
    points = [{"hold": False, "position": 0, "start_tempo": 120.0},
              {"hold": True, "position": 3840, "start_tempo": 60.0}]
    bad_id = status(400) + errors("tempo map id 'abc' is not a non-negative decimal integer")
    unknown = status(404) + detail("there is no TempoMap 999999")
    return folder("5 Tempo maps", (
        "The root map of tempo-map-score is Main, 960 ticks per beat, with two points: 120 BPM ramping to "
        "60 BPM at beat 4, then held. Its id is read from the root and used from then on, since ids are "
        "assigned per load (server.md §2.2). The paging requests follow `next` until it is null; that "
        "loop needs the Collection Runner or newman, and a single Send fetches one page."
    ), [
        request("Root tempo map: 200", "GET", "/v1/tempo-maps/root",
                status(200) + fits("TempoMapWithChildren") + (
                    "const root = pm.response.json();\n"
                    "pm.test('the root is Main, 960 ticks per beat, with no children', () => {\n"
                    "    pm.expect(root.name).to.equal('Main');\n"
                    "    pm.expect(root.ticks_per_beat).to.equal(960);\n"
                    "    pm.expect(root.children).to.eql([]);\n"
                    "});\n"
                    "pm.collectionVariables.set('tempoMapId', root.id);\n"
                    "pm.collectionVariables.set('rootChildren', JSON.stringify(root.children));\n")),
        request("Children of the root: 200", "GET", "/v1/tempo-maps/{{tempoMapId}}/children",
                status(200) + fits("TempoMapChildren") + (
                    "pm.test('they are the children the root listed', () => {\n"
                    "    const body = pm.response.json();\n"
                    "    pm.expect(body.tempo_map_id).to.equal(Number(pm.collectionVariables.get('tempoMapId')));\n"
                    "    pm.expect(body.children).to.eql(JSON.parse(pm.collectionVariables.get('rootChildren')));\n"
                    "});\n")),
        request("Points, one page: 200", "GET", "/v1/tempo-maps/{{tempoMapId}}/points",
                status(200) + fits("TempoMapPointPage") + (
                    "const page = pm.response.json();\n"
                    f"pm.test('both points, and no next page', () => {{\n"
                    f"    pm.expect(page.ticks_per_beat).to.equal(960);\n"
                    f"    pm.expect(page.points).to.eql({json.dumps(points)});\n"
                    f"    pm.expect(page.next).to.equal(null);\n"
                    f"}});\n"
                    "pm.collectionVariables.set('allPositions', JSON.stringify(page.points.map((p) => p.position)));\n")),
        request("Points, first page of one: 200", "GET", "/v1/tempo-maps/{{tempoMapId}}/points?limit=1",
                status(200) + fits("TempoMapPointPage") + (
                    "const page = pm.response.json();\n"
                    "const id = pm.collectionVariables.get('tempoMapId');\n"
                    "pm.test('one point, and next moves after it', () => {\n"
                    "    pm.expect(page.points.length).to.equal(1);\n"
                    "    pm.expect(page.next).to.equal(`/v1/tempo-maps/${id}/points?after=0&limit=1`);\n"
                    "});\n"
                    "pm.collectionVariables.set('pagedPositions', JSON.stringify(page.points.map((p) => p.position)));\n"
                    "pm.collectionVariables.set('nextPath', page.next || '');\n"
                    "pm.collectionVariables.set('pagesFollowed', 0);\n")),
        request("Points, following next: 200", "GET", "{{nextPath}}",
                status(200) + fits("TempoMapPointPage") + (
                    "const page = pm.response.json();\n"
                    "const positions = JSON.parse(pm.collectionVariables.get('pagedPositions'))\n"
                    "    .concat(page.points.map((p) => p.position));\n"
                    "const followed = Number(pm.collectionVariables.get('pagesFollowed')) + 1;\n"
                    "pm.collectionVariables.set('pagedPositions', JSON.stringify(positions));\n"
                    "pm.collectionVariables.set('pagesFollowed', followed);\n"
                    "pm.test('next stays on this map', () => {\n"
                    "    if (page.next !== null) {\n"
                    "        const id = pm.collectionVariables.get('tempoMapId');\n"
                    "        pm.expect(page.next.split('?')[0]).to.equal(`/v1/tempo-maps/${id}/points`);\n"
                    "    }\n"
                    "});\n"
                    "if (page.next !== null && followed < 1000) {\n"
                    "    pm.collectionVariables.set('nextPath', page.next);\n"
                    "    pm.execution.setNextRequest(pm.info.requestName);\n"
                    "} else {\n"
                    "    pm.test('the pages hold every point, once, in order', () => {\n"
                    "        pm.expect(page.next).to.equal(null);\n"
                    "        pm.expect(positions).to.eql(JSON.parse(pm.collectionVariables.get('allPositions')));\n"
                    "    });\n"
                    "}\n"),
                prerequest=(
                    "if (!pm.collectionVariables.get('nextPath')) {\n"
                    "    throw new Error('Run \"Points, first page of one\" first: it sets nextPath');\n"
                    "}\n")),
        request("Points after a position between points: 200", "GET",
                "/v1/tempo-maps/{{tempoMapId}}/points?after=1",
                status(200) + fits("TempoMapPointPage") + (
                    "pm.test('only the point at 3840', () => {\n"
                    "    pm.expect(pm.response.json().points.map((p) => p.position)).to.eql([3840]);\n"
                    "    pm.expect(pm.response.json().next).to.equal(null);\n"
                    "});\n")),
        request("Points after the last: 200, empty", "GET", "/v1/tempo-maps/{{tempoMapId}}/points?after=3840",
                status(200) + fits("TempoMapPointPage") + (
                    "pm.test('no points, and no next page', () => {\n"
                    "    pm.expect(pm.response.json().points).to.eql([]);\n"
                    "    pm.expect(pm.response.json().next).to.equal(null);\n"
                    "});\n")),
        request("Points, bad and unknown parameters: 400", "GET",
                "/v1/tempo-maps/{{tempoMapId}}/points?limit=0&after=-1&x=1",
                status(400) + errors("unknown query parameter 'x'",
                                     "'after' must be a position in ticks, a non-negative integer, not '-1'",
                                     "'limit' must be an integer from 1 to 1000, not '0'")),
        request("Points, limit above 1000: 400", "GET", "/v1/tempo-maps/{{tempoMapId}}/points?limit=1001",
                status(400) + errors("'limit' must be an integer from 1 to 1000, not '1001'")),
        request("Points, a parameter given twice: 400", "GET",
                "/v1/tempo-maps/{{tempoMapId}}/points?limit=1&limit=2",
                status(400) + detail("query parameter 'limit' is given more than once")),
        request("Points, id not an integer: 400", "GET", "/v1/tempo-maps/abc/points", bad_id),
        request("Children, id not an integer: 400", "GET", "/v1/tempo-maps/abc/children", bad_id),
        request("Points of an unknown map: 404", "GET", "/v1/tempo-maps/999999/points", unknown),
        request("Children of an unknown map: 404", "GET", "/v1/tempo-maps/999999/children", unknown),
    ])


def frame_position_request(at: str, written: str, frame: int, offset: int, why: str) -> dict:
    """A frame-position request expected to succeed."""
    return request(f"Frame position at {at}: {why}", "GET", f"/v1/tempo-maps/{{{{tempoMapId}}}}/frame-position?at={at}",
                   status(200) + fits("FramePosition") + (
                       f"pm.test('{written} is frame {frame}, offset {offset}', () => {{\n"
                       f"    const body = pm.response.json();\n"
                       f"    pm.expect(body.tempo_map_id).to.equal(Number(pm.collectionVariables.get('tempoMapId')));\n"
                       f"    pm.expect(body.at).to.equal('{written}');\n"
                       f"    pm.expect([body.frame, body.offset]).to.eql([{frame}, {offset}]);\n"
                       f"}});\n"))


def frame_position_folder() -> dict:
    """Musical positions to frames and offsets, against the oracle's times."""
    oracle = []
    for beat, time in ORACLE_BEATS:
        sample = int(time // 1)
        oracle.append(frame_position_request(str(beat), f"{beat}:0", sample // FRAME_SIZE, sample % FRAME_SIZE,
                                             f"the oracle's sample {sample}"))
    return folder("6 Frame position", (
        "Where a musical position on the root map falls in the sample stream (server.md §3.4). Beats 0, 2, 4 "
        "and 8 are the first partial's frequency envelope nodes; the expected frame and offset are the floor of the "
        "rendering oracle's independent times for them (engine-db/examples/tempo-map-score.json), split by "
        "the frame size of 48000."
    ), [
        frame_position_request("1:0", "1:0", 0, 25638, "200"),
        frame_position_request("1", "1:0", 0, 25638, "a bare beat is written in full"),
        *oracle,
        request("Frame position, ticks at the map's resolution: 400", "GET",
                "/v1/tempo-maps/{{tempoMapId}}/frame-position?at=1:960", status(400)),
        request("Frame position, not beats:ticks: 400", "GET",
                "/v1/tempo-maps/{{tempoMapId}}/frame-position?at=1.5",
                status(400) + errors("'at' must be beats:ticks, two non-negative integers, or a number of beats, not '1.5'")),
        request("Frame position, beyond an INT64 of ticks: 400", "GET",
                "/v1/tempo-maps/{{tempoMapId}}/frame-position?at=99999999999999999999", status(400)),
        request("Frame position, no at: 400", "GET", "/v1/tempo-maps/{{tempoMapId}}/frame-position",
                status(400) + errors("missing query parameter 'at'")),
        request("Frame position on an unknown map: 404", "GET", "/v1/tempo-maps/999999/frame-position?at=1",
                status(404) + detail("there is no TempoMap 999999")),
    ])


def engine_config_folder() -> dict:
    """The engine parameters."""
    return folder("7 Engine config", "The score's EngineConfig, with the frame size resolved (server.md §3.5).", [
        request("Engine config: 200", "GET", "/v1/engine-config",
                status(200) + fits("EngineConfig")
                + body_equals({"channel_mode": "mono", "frame_size": 48000, "sample_rate": 48000})),
        request("Engine config, unknown parameter: 400", "GET", "/v1/engine-config?x=1",
                status(400) + errors("unknown query parameter 'x'")),
    ])


def rendering_folder() -> dict:
    """Rendering a frame, with every form of body that means every partial, and the refusals."""
    rendered = status(200) + fits("RenderResult") + (
        "pm.test('frame 1 is written to frame-000001.wav', () => {\n"
        "    const body = pm.response.json();\n"
        "    pm.expect(body.frame).to.equal(1);\n"
        "    pm.expect(body.uri).to.match(/^file:\\/\\/\\/.*\\/frame-000001\\.wav$/);\n"
        "    (body.set_uris || []).forEach((uri) => pm.expect(uri).to.match(/^file:\\/\\/\\/.*\\.wav$/));\n"
        "});\n")
    frame_error = lambda frame: errors(f"frame '{frame}' must be an integer from 0 to 2147483647")  # noqa: E731
    return folder("8 Renderings", (
        "Renders frames of tempo-map-score into the server's --out-dir. No body, `{}`, and labels null or "
        "empty all mean every partial. The file itself is not checked here: CTest server.* checks each "
        "frame with the rendering oracle."
    ), [
        request("Render frame 1, no body: 200", "POST", "/v1/frames/1/renderings", rendered),
        request("Render frame 1, {}: 200", "POST", "/v1/frames/1/renderings", rendered, body={}),
        request("Render frame 1, labels null: 200", "POST", "/v1/frames/1/renderings", rendered,
                body={"labels": None}),
        request("Render frame 1, labels []: 200", "POST", "/v1/frames/1/renderings", rendered, body={"labels": []}),
        request("Render, unknown label: 422", "POST", "/v1/frames/1/renderings",
                status(422) + detail("1 requested label(s) are not in engine-db."),
                body={"labels": ["no-such-label"]}),
        request("Render, empty label: 400", "POST", "/v1/frames/1/renderings",
                status(400) + errors('every label must be a non-empty string, not ""'), body={"labels": [""]}),
        request("Render, labels not an array: 400", "POST", "/v1/frames/1/renderings",
                status(400) + errors("'labels' must be an array of strings"), body={"labels": "strings"}),
        request("Render, body not a JSON object: 400", "POST", "/v1/frames/1/renderings",
                status(400) + errors("the body is neither empty nor a JSON object"), body=[]),
        request("Render, negative frame and unknown key: 400, both listed", "POST", "/v1/frames/-1/renderings",
                status(400) + errors("frame '-1' must be an integer from 0 to 2147483647", "unknown key 'extra'"),
                body={"extra": 1}),
        request("Render frame 3.0: 400", "POST", "/v1/frames/3.0/renderings", status(400) + frame_error("3.0")),
        request("Render frame +3: 400", "POST", "/v1/frames/+3/renderings", status(400) + frame_error("+3")),
        request("Render frame 2147483648: 400", "POST", "/v1/frames/2147483648/renderings",
                status(400) + frame_error("2147483648")),
    ])


def shutdown_folder() -> dict:
    """Stopping the server, only when asked for."""
    return folder("9 Shutdown", (
        "Stops the server. Skipped unless the variable shutdownAtEnd is `true`, so that a run in Postman "
        "leaves the server up for the next one."
    ), [
        request("Shut down: 200", "POST", "/v1/shutdown",
                status(200) + fits("ShutdownResult") + body_equals({"message": "shutting down"}),
                prerequest="if (pm.variables.get('shutdownAtEnd') !== 'true') {\n"
                           "    pm.execution.skipRequest();\n"
                           "}\n"),
    ])


def collection(spec: dict) -> dict:
    """The whole collection.

    Args:
        spec: The OpenAPI document.

    Returns:
        The collection, in Postman Collection Format v2.1.
    """
    schemas = {name: resolve(spec["components"]["schemas"][name], spec) for name in RESPONSE_SCHEMAS}
    items = [no_score_folder(), routing_folder(), physical_time_folder(), load_folder(), tempo_maps_folder(),
             frame_position_folder(), engine_config_folder(), rendering_folder(), shutdown_folder()]
    variables = [
        ("baseUrl", "http://127.0.0.1:8080", "The server's origin, without /v1."),
        ("repoRoot", "", "The repository's absolute, resolved path: the server's --input-root."),
        ("shutdownAtEnd", "false", "true to shut the server down at the end of the run."),
        ("schemas", json.dumps(schemas, sort_keys=True), "Generated from api/openapi.yaml; do not edit."),
    ] + [(name, "", "Set during the run.") for name in
         ("tempoMapId", "rootChildren", "allPositions", "pagedPositions", "nextPath", "pagesFollowed")]
    return {
        "info": {
            "_postman_id": str(uuid.uuid5(ID_NAMESPACE, "collection")),
            "name": "RAINBOHz rendering-engine server API",
            "description": (
                "Tests of the rendering engine's REST API (rendering-engine/docs/server.md, "
                "rendering-engine/api/openapi.yaml), run in order against a server started with "
                "`make run-rendering-engine-server`.\n\n"
                "Set `repoRoot` to the repository's absolute path (`pwd -P` at its top) and, if the server is not "
                "on 127.0.0.1:8080, `baseUrl`. Then run the whole collection in the Collection Runner, or:\n\n"
                "```\nnewman run rendering-engine/test/postman/rendering-engine.postman_collection.json \\\n"
                "    --env-var repoRoot=\"$(pwd -P)\"\n```\n\n"
                "Folders depend on those before them. Generated by make_postman_collection.py from the OpenAPI "
                "document; edit that script, not this file."
            ),
            "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json",
        },
        "event": [script("prerequest", COLLECTION_PREREQUEST), script("test", COLLECTION_TEST)],
        "variable": [{"key": key, "value": value, "type": "string", "description": description}
                     for key, value, description in variables],
        "item": items,
    }


def main() -> int:
    """Read the document, write the collection."""
    import yaml

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--spec", default=DEFAULT_SPEC)
    parser.add_argument("--out", default=DEFAULT_OUT)
    args = parser.parse_args()

    with open(args.spec, encoding="utf-8") as handle:
        spec = yaml.safe_load(handle)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(collection(spec), handle, indent=2)
        handle.write("\n")
    print(f"make_postman_collection: wrote {os.path.relpath(args.out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
