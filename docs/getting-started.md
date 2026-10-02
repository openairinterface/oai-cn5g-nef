# Getting Started with OAI NEF

This guide takes you from zero to your first NEF API call. It runs a single NEF
container in development mode, checks its health, and walks through the full lifecycle of a
Monitoring Event subscription.

For a native (non-Docker) build, see [Build from Source](build.md). For a full core-network stack
with NRF, AMF, SMF, PCF and UDR, see the [Deployment Guide](deployment.md).

---

## Prerequisites

| Requirement | Notes |
|---|---|
| Docker >= 20.10 | `docker --version` |
| Docker Compose >= 2.x | `docker compose version` |
| Port 8080 free on localhost | The NEF SBI / northbound port in the shipped config template |
| An `oai-nef` image | Pull a published image, or build one — see [Build from Source](build.md#8-build-with-docker) |

**Optional**: `curl` with HTTP/2 support (any recent build) and Python 3 for pretty-printing JSON.

---

## Step 1: Start NEF

The image ships a working configuration template at `/openair-nef/etc/config.yaml`. Its defaults
are development-friendly: the SBI/northbound port is `8080`, and `insecure_dev_mode` is `true`, so
authentication is not enforced.

Create a `docker-compose.yaml` in an empty directory:

```yaml
# docker-compose.yaml — single NEF for local development
services:
  oai-nef:
    image: oai-nef:develop
    container_name: oai-nef
    ports:
      - "8080:8080"
    environment:
      - TZ=Europe/Paris
```

Start the container:

```bash
docker compose -f docker-compose.yaml up -d oai-nef
```

> **Development mode.** The shipped template sets `nef.security.insecure_dev_mode: true`, which
> disables authentication enforcement — any caller can invoke any endpoint. This is intentional
> for local development. Do not deploy the template as-is. See the [Security defaults](../README.md#security-defaults)
> and the [API Overview](api-reference/overview.md#authentication) for how to turn authentication on.

To run with your own configuration, mount a `config.yaml` and tell the entrypoint to use it
verbatim instead of templating it:

```yaml
    environment:
      - TZ=Europe/Paris
      - MOUNT_CONFIG=yes
    volumes:
      - ./config.yaml:/openair-nef/etc/config.yaml:ro
```

Confirm the container is up and review the recent log lines:

```bash
docker ps
docker logs oai-nef 2>&1 | tail -20
```

---

## Step 2: Check health

`GET /health` is unauthenticated and exempt from the drain guard and the rate limiter, so it is
always safe to call. It reports whether NEF is serving normally or draining for shutdown.

```bash
curl --http2-prior-knowledge http://localhost:8080/health
```

`--http2-prior-knowledge` is required. NEF speaks cleartext HTTP/2 (h2c) only, `/health` included;
a plain HTTP/1.1 request gets no usable response.

A healthy NEF answers `200`:

```json
{
  "status": "ok",
  "nf_type": "NEF",
  "instance_id": "d3c2b1a0-f9e8-4d7c-b6a5-9e8f7d6c5b4a",
  "uptime_seconds": 3821,
  "draining": false
}
```

During graceful shutdown it answers `503` with `{"status": "draining", "nf_type": "NEF"}`. See
[Operational Endpoints](api-reference/operational-endpoints.md) for the full contract.

**Troubleshooting**:

| Symptom | Likely cause | Fix |
|---|---|---|
| Connection refused | Container not running, or port mapping wrong | `docker ps` to confirm the container is up; check the `8080:8080` mapping. |
| No response / hangs | Request sent as HTTP/1.1 | Add `--http2-prior-knowledge` to `curl`. |
| `503 {"status":"draining"}` | NEF is shutting down | Expected during a graceful stop; the process is not accepting new work. |

---

## Step 3: Authentication

How you authenticate depends on `jwt_secret`, `af_whitelist` and `insecure_dev_mode` in
`config.yaml`. NEF never returns `401` — every authentication failure is a `403` with a
ProblemDetails body.

### Development mode (default template)

With `jwt_secret` empty, `af_whitelist` empty and `insecure_dev_mode: true` — the shipped
defaults — NEF accepts every request without credentials. No `Authorization` header is needed. It
infers the AF identity from the identifier in the URL path. You can skip to
[Step 4](#step-4-create-your-first-monitoring-subscription).

```bash
# Development mode — no token needed.
TOKEN=""
```

### Production mode

Once you set a non-empty `jwt_secret`, every request must carry an
`Authorization: Bearer <JWT>` header. The token is a JWT signed with HMAC-SHA256 (`HS256`) using
that secret. The `sub` claim is the AF identifier and must match the identifier used in the URL
path (and the `af_id` in the whitelist, when one is configured). An `exp` claim is recommended.

A decoded payload looks like this:

```json
{
  "sub": "my-af",
  "iat": 1745539200,
  "exp": 1745625600
}
```

Export a signed token before the next steps (the header must declare `HS256`; a token claiming
`none` or an asymmetric algorithm is rejected):

```bash
TOKEN="eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..."
```

If a whitelist entry carries an `api_key`, the caller must also send it in an `X-API-Key` header.
For the full rules, see the [API Overview — Authentication](api-reference/overview.md#authentication)
and the [Security Guide](security.md).

---

## Step 4: Create your first Monitoring subscription

The Monitoring Event API lets an AF subscribe to UE network events and receive asynchronous HTTP
callbacks when they occur. This command subscribes to `LOSS_OF_CONNECTIVITY` for the AF `my-af`:

```bash
curl --http2-prior-knowledge \
  -X POST http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{
    "monitoringType": "LOSS_OF_CONNECTIVITY",
    "notificationDestination": "http://my-af:8000/notify",
    "monitorExpireTime": "2026-12-31T23:59:59Z"
  }'
```

`monitoringType` and `notificationDestination` are the two required fields; `monitorExpireTime` is
optional. `--http2-prior-knowledge` is required, as on every northbound call.

NEF answers `201 Created`. The body is the request echoed back with a `subscriptionId` and a
`self` URL added (NEF assigns sequential integer IDs; there is no `Location` header on this API):

```json
{
  "monitoringType": "LOSS_OF_CONNECTIVITY",
  "notificationDestination": "http://my-af:8000/notify",
  "monitorExpireTime": "2026-12-31T23:59:59Z",
  "subscriptionId": "3",
  "self": "http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions/3"
}
```

Save the ID for the next steps:

```bash
SUBSCRIPTION_ID="3"
```

For all fields, event types and status codes, see the [Monitoring Event API](api-reference/monitoring-event.md).

---

## Step 5: Read your subscription

Retrieve the subscription to confirm it was stored:

```bash
curl --http2-prior-knowledge \
  http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions/$SUBSCRIPTION_ID \
  -H "Authorization: Bearer $TOKEN"
```

A `200 OK` returns the full subscription object. A `404 Not Found` means the subscription no longer
exists — it may have expired, or NEF may have restarted. All subscription state is held in memory,
so a restart clears every subscription; see [Resilience](resilience.md).

---

## Step 6: Delete your subscription

Remove the subscription when it is no longer needed:

```bash
curl --http2-prior-knowledge \
  -X DELETE http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions/$SUBSCRIPTION_ID \
  -H "Authorization: Bearer $TOKEN"
# Expected: 204 No Content
```

A `204 No Content` confirms the deletion. NEF also unsubscribes from AMF to release the underlying
southbound event subscription.

---

## What's next

- [API Reference Overview](api-reference/overview.md) — authentication, the HTTP/2 requirement, the error format, and patterns common to every API.
- [Monitoring Event API](api-reference/monitoring-event.md) — all endpoints, request fields and event types for TS 29.122.
- [Deployment Guide](deployment.md) — the full Docker Compose stack with NRF, AMF, SMF, PCF and UDR.
- [Configuration Reference](configuration-reference.md) — every `config.yaml` parameter with defaults and descriptions.
- [Call Flows](call-flows.md) — sequence diagrams for NEF's interactions with NRF, AMF, PCF and UDR.
