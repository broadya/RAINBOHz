# Security policy

## Supported versions

RAINBOHz has no releases yet. Only the current `main` branch is maintained,
and fixes are made there.

## Scope

RAINBOHz is research software. It is not run as a public service.

The rendering engine's server mode is plain HTTP with no authentication,
authorisation, TLS or rate limiting, and its error messages name file paths.
It is meant to run on a trusted host, or behind an API gateway in a real
deployment; see [rendering-engine/docs/server.md](rendering-engine/docs/server.md).
Problems that only arise from exposing it directly to an untrusted network
are outside the supported use.

These are in scope, and reports are welcome:

- reading or writing files outside the server's `--input-root` or
  `--out-dir`;
- crashes, hangs or memory errors caused by a malformed HzL score, Cypher
  script or REST request;
- anything else that lets input do more than the documentation says it can.

## Reporting a vulnerability

Please report it privately through GitHub: on the repository's **Security**
tab, choose **Report a vulnerability**. Do not open a public issue.

## What to expect

RAINBOHz is maintained by one person, so responses are best-effort and there
is no fixed response time. You will get an acknowledgement, and credit in the
fix if you would like it.
