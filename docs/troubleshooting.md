# Troubleshooting

---

## Diagnosis Tools

Start here when investigating any NEF issue.

| Tool | Purpose | Command |
|---|---|---|
| Health endpoint | Check NEF liveness and drain state | `curl --http2-prior-knowledge http://localhost:8080/health` |
| Docker logs | View NEF logs | `docker logs oai-nef 2>&1 \| tail -50` |
| Live log follow | Tail logs continuously | `docker logs -f oai-nef` |
| Grep for errors | Find error messages | `docker logs oai-nef 2>&1 \| grep -i "error\|fail"` |

> **Note:** NEF speaks HTTP/2 cleartext (h2c) only. Append
> `--http2-prior-knowledge` to every `curl` command. See
> [HTTP/2 Connection Issues](#http2-connection-issues) if curl reports a
> protocol error.

The `/health` body tells you two things and nothing more: whether the server is
draining, and — when it is not — its instance ID and uptime. A healthy NEF
answers `200`:

```json
{"status":"ok","nf_type":"NEF","instance_id":"...","uptime_seconds":42,"draining":false}
```

A draining NEF answers `503` with a short body: `{"status":"draining","nf_type":"NEF"}`.
`/health` does **not** report NRF registration state.

---

## Common Problems

### NEF returns 503 on API calls

**Symptom**: API calls return `503 Service Unavailable` with a Problem Details
body.

There are two causes, and the detail string tells them apart.

**Cause A — the server is draining.** Detail: `"Server is draining"`. NEF is
in graceful shutdown. `begin_request()` rejects every route except `/health`
with `503` once draining starts. This is expected during a restart or scale-down.

**Fix**: wait for the new NEF instance to come up, or confirm the container is
not mid-shutdown (`docker ps`, `docker logs oai-nef | tail`).

**Cause B — the dispatcher pool is saturated.** Detail:
`"Server is overloaded, please retry later"`. The bounded request queue is full
or the dispatcher has stopped, so the adapter rejects the dispatch.

**Fix**:
1. Check whether a slow or hung southbound NF is tying up dispatcher workers —
   the 26 blocking endpoints park an HTTP worker until their handler returns
   (see [`ARCHITECTURE.md`](ARCHITECTURE.md) §7).
2. Reduce request concurrency, or verify the downstream NFs are responsive.

> A tripped southbound circuit breaker does **not** by itself produce a
> northbound `503`. On the async request path, southbound calls do not consult
> the breaker; a discovery failure surfaces as `500`/`502` from the handler
> instead. See [Resilience Guide](resilience.md#southbound-circuit-breaker).

---

### NEF returns 429 Too Many Requests

**Symptom**: API calls return `429` with `{"title": "Too Many Requests", ...}`
and detail `"Rate limit exceeded"`.

**Cause**: the caller's token bucket is empty. The rate limiter is **per
caller**, keyed by the bearer token (or, with no token, the peer address). It
runs before authorization, so it applies even to unauthenticated requests.

**Fix**:
- Slow the request rate for that caller. The default bucket sustains 100
  requests/second with a burst of 200.
- If several clients present no token and share a source address (for example
  behind one proxy), they share a single bucket and can throttle each other.
  Give each a distinct bearer token.
- See [Resilience Guide](resilience.md#rate-limiter) for the full behaviour.

---

### Subscription Expired — AF receives 404

**Symptom**: an AF calls `GET /subscriptions/{id}` and gets `404 Not Found`.
Notifications have stopped.

**Cause**: the subscription's `monitorExpireTime` elapsed. NEF's periodic
expiry sweep deleted the local record and issued the matching southbound cleanup
(a monitoring-event subscription unsubscribes from AMF; traffic-influence
deletes the PCF app-session and UDR influence data). The log line
`Subscription <id> expired - cleaning up` confirms it.

**Fix**:
- Re-subscribe with a fresh `POST`.
- Set `monitorExpireTime` further in the future, or omit it for a
  non-expiring subscription.
- For production, add renewal on the AF side: update `monitorExpireTime` before
  it elapses, or track expiry client-side and re-`POST` in time.

---

### Notifications Stopped Unexpectedly

**Symptom**: the subscription still exists (`GET` returns `200 OK`) but the AF
stops receiving notifications.

**Cause A — the AF callback is unreachable, and its breaker opened.** NEF
delivers notifications with a retry-and-backoff loop guarded by the northbound
circuit breaker. After 10 consecutive failed deliveries to one callback
endpoint (scheme + host + port), the breaker opens and NEF drops further
notifications to it without sending. Because that breaker has no cooldown and no
half-open probe, it does not recover on its own.

**Fix**:
1. Confirm the AF callback is reachable from inside the NEF container:
   ```bash
   docker exec oai-nef curl --http2-prior-knowledge -v http://your-af:8000/notify
   ```
2. Look for `Circuit breaker OPEN for endpoint` and
   `dropping notification without sending` in the logs.
3. Restart NEF to clear the opened endpoint breaker once the AF is reachable
   again (there is no runtime reset).

**Cause B — `notificationUri` does not resolve inside the container.** NEF
resolves the callback from within its Docker network. A hostname that resolves
on the host may not resolve inside the container.

**Fix**: put the AF on the same Docker network as NEF, or use a name that
resolves inside the container. See [Deployment Guide](deployment.md).

**Cause C — the subscription expired in a race window.** `monitorExpireTime`
elapsed between your `GET` and now.

**Fix**: `GET` the subscription again. A `404` means it expired — re-subscribe
as above.

---

### NEF container exits immediately at startup

**Symptom**: `docker ps` shows the container is not running; `docker logs
oai-nef` shows a startup error.

**Cause A — config file missing or malformed.** NEF reads
`/openair-nef/etc/config.yaml` at startup. If it is not mounted or contains
invalid YAML, the process exits.

**Fix**:
1. Verify the mount (for example
   `-v $(pwd)/etc/config.yaml:/openair-nef/etc/config.yaml`).
2. Validate the YAML:
   `python3 -c "import yaml; yaml.safe_load(open('etc/config.yaml'))"`.

**Cause B — port 8080 already in use.** If another process holds port 8080, NEF
cannot start its HTTP/2 server.

**Fix**:
1. Find the conflict: `lsof -i :8080`.
2. Stop it, or change the NEF port via `NEF_INTERFACE_PORT_FOR_SBI` and update
   the `-p` mapping to match.

---

### NRF Registration Failure

**Symptom**: NEF logs show `NEF NRF registration failed` or
`NRF heartbeat failed ... — will re-register`. Southbound calls that need a
fresh NRF discovery fail.

**Cause**: NRF is unreachable at the configured address.

Note: `/health` still returns `200` with `status: "ok"` while NRF registration
is failing — the health endpoint does not reflect registration state. Diagnose
from the logs and from discovery-dependent request failures instead.

**Fix**:
1. Confirm the NRF container is up: `docker ps | grep nrf`.
2. Check the NRF address NEF uses — `nrf.host` / `nrf.port` in
   `etc/config.yaml`, or the `NRF_FQDN` environment variable — and confirm it
   resolves inside the NEF container:
   ```bash
   docker exec oai-nef nslookup oai-nrf
   ```
3. Check NRF's own logs: `docker logs oai-nrf 2>&1 | tail -50`.

---

### Authorization Errors (403)

**Symptom**: an API call returns `403 Forbidden`.

NEF does not use `401`. Every authorization failure — missing token, invalid
token, unknown AF, disallowed API — returns `403` with the Problem Details
detail `"AF not authorized for this service"` (or, for Nnef SBI services,
`"NF not authorized for this Nnef service"`).

**Common causes and fixes**:

- **A bearer token is required but missing or invalid.** When
  `nef.security.jwt_secret` is set, every request needs a valid JWT. NEF
  validates HS256 tokens signed with that secret and checks the `scope` claim
  (must match the API), the `sub` claim (must match the AF/SCS-AS ID in the
  path) and, if present, `exp`. There is no `aud` check, and tokens are signed
  with the shared `jwt_secret`, not obtained from another NF.
- **The AF is not authorized for this API.** When no JWT secret is set, NEF
  falls back to the `nef.af_whitelist` list. Add the AF's `af_id` (and, if you
  restrict per-API, the API name under `allowed_apis`) and restart NEF.
- **Auth is unconfigured.** With no `jwt_secret` and an empty `af_whitelist`,
  NEF fails closed and denies everything unless
  `nef.security.insecure_dev_mode: true` is set.

**Dev shortcut**: `nef.security.insecure_dev_mode: true` allows unauthenticated
access when no JWT secret and no whitelist are configured. **Never enable it in
production.**

See [API Reference Overview](api-reference/overview.md#authentication) and the
[Security Guide](security.md) for the full authorization model.

---

### HTTP/2 Connection Issues

**Symptom**: `curl` returns `curl: (1) Received HTTP/0.9 when not allowed` or a
similar protocol error.

**Cause**: NEF speaks HTTP/2 cleartext (h2c) only. Standard `curl` defaults to
HTTP/1.1, which NEF rejects.

**Fix**: pass `--http2-prior-knowledge` on every request:

```bash
curl --http2-prior-knowledge http://localhost:8080/health
```

For AF code, configure the HTTP client for HTTP/2 prior knowledge (direct h2c),
not HTTP/1.1 or TLS-based h2. NEF provides no TLS in this release — do not use
`https://` URLs.

See
[API Reference Overview](api-reference/overview.md#http-protocol-requirements)
for supported protocol details.

---

### PFD Transaction Aborted

**Symptom**: `PUT /3gpp-pfd-management/v1/{scsAsId}/transactions/{transId}`
returns `500`, with a detail like `PFD transaction aborted: UDR write failed
for app <id>`.

**Cause**: one of the per-application UDR writes in the batch failed. NEF writes
each application's PFD data to UDR in turn and commits local state only after
all writes succeed. On a mid-batch failure it issues compensating `DELETE`s over
the already-written applications in reverse order, then aborts the whole
transaction with `500`. Local state is not committed in this case, so there is
nothing partial to see on the NEF side; the rollback is best-effort against UDR.

**Fix**:
1. Confirm UDR is reachable and healthy.
2. If a rollback `DELETE` also failed (logged), UDR may hold an orphaned
   application entry. Issue a `DELETE` on the transaction to clean up, then
   re-attempt the `PUT` with all applications in a single request.

See [Call Flows — PFD Management](call-flows.md#4-pfd-management-atomic-batch-with-rollback).

---

## Log Reference

Real log strings you can grep for. Prefixes and wording are taken from the
source; match on the distinctive substring rather than the whole line.

| Log substring | Meaning |
|---|---|
| `NEF successfully registered to NRF` | Registration succeeded |
| `NEF NRF registration failed` | Registration failed at startup or after a heartbeat failure |
| `NRF heartbeat failed` | A heartbeat did not reach NRF; NEF re-registers |
| `[SBI] Circuit breaker OPEN for NF` | Southbound breaker tripped; the call was dropped without sending |
| `Circuit breaker OPEN for endpoint` | Northbound breaker tripped for an AF callback; notifications are dropped |
| `Subscription <id> expired - cleaning up` | An expired monitoring/QoS/TI subscription was removed |
| `Nnef_EventExposure subscription <id> expired - removing` | An expired Nnef_EventExposure subscription was removed |
| `JWT validation failed for AF` | A bearer token failed validation |

---

## Related Guides

- [Resilience Guide](resilience.md) — circuit breakers, rate limiter, and
  in-memory state / restart behaviour
- [Call Flows](call-flows.md) — end-to-end sequences for each API
- [Deployment Guide](deployment.md) — environment variables, Docker, networking
- [API Reference Overview](api-reference/overview.md) — HTTP/2 requirements,
  authentication, and error response format
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — request path, threading, and resilience
  context
