# Getting Started with OAI NEF

This guide takes you from zero to your first NEF API call in under ten minutes.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| Docker >= 20.10 | `docker --version` |
| Docker Compose >= 2.x | `docker compose version` |
| Port 8080 open on localhost | Used for the NEF SBI / northbound AF port |
| Access to `docker.io/oaisoftwarealliance` | Or a locally built image |

**Optional**: Python 3.8+ is useful for scripted examples but is not required to complete this guide.

---

## Step 1: Pull and Start NEF

Create a minimal `docker-compose.yaml` in an empty directory. For a full production configuration that includes NRF, AMF, SMF, PCF, and UDR, see the [Deployment Guide](deployment.md).

```yaml
# docker-compose.yaml — minimal standalone NEF for local development
version: '3.8'
services:
  oai-nef:
    image: docker.io/oaisoftwarealliance/oai-nef:latest
    container_name: oai-nef
    ports:
      - "8080:8080"
    environment:
      - TZ=Europe/Paris
      - NEF_INTERFACE_NAME_FOR_SBI=eth0
      - NEF_INTERFACE_PORT_FOR_SBI=8080
      - NEF_INTERFACE_HTTP2_PORT_FOR_SBI=9090
      - NEF_API_VERSION=v1
      - INSTANCE=0
      - PID_DIRECTORY=/var/run
      - INSECURE_DEV_MODE=true
```

Start the container:

```bash
docker compose -f docker-compose.yaml up -d oai-nef
```

> **Note**: `INSECURE_DEV_MODE=true` disables JWT validation. This is intentional for local development. Never use this setting in a shared or production environment. See [Security Guide](security.md) for production auth setup.

Verify the container started and review the last few log lines:

```bash
docker logs oai-nef 2>&1 | tail -20
```

You should see lines similar to:

```
[info] NEF started, listening on 0.0.0.0:8080
[info] NRF registration skipped (standalone dev mode)
```

---

## Step 2: Verify Health

The `/health` endpoint is unauthenticated and always returns a JSON body describing the NEF's current state. It is safe to call at any time without a token.

```bash
curl http://localhost:8080/health
```

Expected response:

```json
{"status": "healthy", "nfId": "...", "nfStatus": "REGISTERED"}
```

**Troubleshooting**:

| Symptom | Likely cause | Fix |
|---|---|---|
| `nfStatus: "UNDISCOVERABLE"` | NRF registration failed | Check `NRF_FQDN` and `NRF_PORT` in the container environment. Ensure NRF is reachable. |
| Connection refused | Container not running or port mapping wrong | `docker ps` to confirm container is up; check `-p 8080:8080` mapping. |
| `{"status": "starting"}` | NEF not yet ready | Wait a few seconds and retry. |

---

## Step 3: Obtain a JWT Token

### Development mode (`INSECURE_DEV_MODE=true`)

When `INSECURE_DEV_MODE` is `true`, no `Authorization` header is required. NEF accepts all requests and infers the AF identity from the URL path parameter. You can skip token generation entirely and proceed to [Step 4](#step-4-create-your-first-monitoring-subscription).

```bash
# Development mode — no token needed. Leave TOKEN empty or unset.
TOKEN=""
```

### Production mode (`INSECURE_DEV_MODE=false`)

In production, every request must carry an `Authorization: Bearer <JWT>` header. The JWT is signed with HMAC-SHA256 using the `jwt_secret` value from `etc/config.yaml`. The `sub` claim must match the AF identifier used in the URL path (e.g., `my-af`).

Generate a token from your authentication service (refer to AUSF documentation for the full token acquisition flow). The JWT payload must contain at minimum:

```json
{
  "sub": "my-af",
  "iat": <issued-at unix timestamp>
}
```

Example — export the token into an environment variable before the subsequent steps:

```bash
# Production mode — obtain a signed JWT from your authentication endpoint
TOKEN="eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9..."
```

For detailed JWT configuration, claim validation rules, and API key alternatives, see [Security Guide](security.md).

---

## Step 4: Create Your First Monitoring Subscription

The Monitoring Event API lets you subscribe to UE network events (such as loss of connectivity) and receive asynchronous HTTP callbacks when they occur.

The following command creates a subscription for the `LOSS_OF_CONNECTIVITY` event on a specific UE:

```bash
curl --http2-prior-knowledge \
  -X POST http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{
    "monitoringType": "LOSS_OF_CONNECTIVITY",
    "notificationURI": "http://my-af:8000/notify",
    "supi": "imsi-208950000000001",
    "monitorExpireTime": "2026-12-31T23:59:59Z"
  }'
```

> **Note**: `--http2-prior-knowledge` is required. All NEF northbound APIs run on HTTP/2; plain HTTP/1.1 requests will be rejected.

Expected response — `201 Created`:

```json
{
  "subscriptionId": "3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "self": "http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions/3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "monitoringType": "LOSS_OF_CONNECTIVITY",
  "notificationURI": "http://my-af:8000/notify",
  "supi": "imsi-208950000000001",
  "monitorExpireTime": "2026-12-31T23:59:59Z"
}
```

Save the `subscriptionId` for the next steps:

```bash
SUBSCRIPTION_ID="3fa85f64-5717-4562-b3fc-2c963f66afa6"
```

For full field documentation and all supported event types, see [Monitoring Event API](api-reference/monitoring-event.md).

---

## Step 5: Query Your Subscription

Retrieve the subscription you just created to confirm it was stored correctly:

```bash
curl --http2-prior-knowledge \
  http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions/$SUBSCRIPTION_ID \
  -H "Authorization: Bearer $TOKEN"
```

Expected response — `200 OK` with the full subscription object. If you receive `404 Not Found`, the subscription may have been expired (check `monitorExpireTime`) or NEF may have restarted (subscriptions are held in memory; see [Resilience](resilience.md#in-memory-state-and-restart-behavior)).

---

## Step 6: Delete Your Subscription

Clean up the subscription when it is no longer needed:

```bash
curl --http2-prior-knowledge \
  -X DELETE http://localhost:8080/3gpp-monitoring-event/v1/my-af/subscriptions/$SUBSCRIPTION_ID \
  -H "Authorization: Bearer $TOKEN"
# Expected: 204 No Content
```

A `204 No Content` response confirms the subscription was deleted. NEF also sends a corresponding `DELETE` to AMF to cancel the underlying southbound event subscription.

---

## What's Next

- [API Reference Overview](api-reference/overview.md) — Authentication, HTTP/2 requirements, error format, and common patterns for all NEF APIs.
- [Monitoring Event API](api-reference/monitoring-event.md) — All endpoints, request fields, and event types for TS 29.122.
- [Deployment Guide](deployment.md) — Full Docker Compose stack with NRF, AMF, SMF, PCF, and UDR; all environment variables.
- [Configuration Reference](configuration-reference.md) — Every `config.yaml` parameter with defaults and descriptions.
- [Call Flows](call-flows.md) — Mermaid sequence diagrams showing NEF interactions with NRF, AMF, PCF, and UDR.
