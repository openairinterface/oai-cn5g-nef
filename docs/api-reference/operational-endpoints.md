# Operational Endpoints

Read the [API Overview](overview.md) before this page for common HTTP/2 and authentication conventions.

---

## Overview

NEF exposes two operational endpoints that are not part of any 3GPP service definition:

| Endpoint | Method | Purpose |
|---|---|---|
| `/health` | `GET` | Liveness and readiness probe |
| `/nef-notify/v1/notify/{nf_sub_id}` | `POST` | Inbound notification receiver for southbound NFs |

---

## Health Check Endpoint

### `GET /health`

An unauthenticated liveness/readiness endpoint. No `Authorization` header is required. Returns a JSON body describing the NEF's current operational state.

**Request**

```bash
curl --http2-prior-knowledge http://oai-nef:8080/health
```

Or without specifying HTTP/2 (health check callers such as Docker and Kubernetes probes typically use HTTP/1.1 — this endpoint also accepts HTTP/1.1):

```bash
curl http://oai-nef:8080/health
```

**Response 200 OK**

```json
{
  "status": "healthy",
  "nfId": "d3c2b1a0-f9e8-4d7c-b6a5-9e8f7d6c5b4a",
  "nfStatus": "REGISTERED"
}
```

### Response Field Reference

| Field | Type | Description |
|---|---|---|
| `status` | string | Always `"healthy"` when NEF is responding. A non-responding NEF means the probe fails. |
| `nfId` | string (UUID) | NEF's NF instance ID assigned at startup; consistent across the process lifetime |
| `nfStatus` | string | NF registration state; one of `REGISTERED`, `UNDISCOVERABLE`, `DEREGISTERED` |

### `nfStatus` Values

| Value | Meaning |
|---|---|
| `REGISTERED` | NEF has successfully registered with NRF and is accepting requests |
| `UNDISCOVERABLE` | NEF is running but has marked itself as undiscoverable at NRF (e.g., during drain) |
| `DEREGISTERED` | NEF has deregistered from NRF; graceful shutdown is in progress |

### Use Cases

**Kubernetes liveness probe**

```yaml
livenessProbe:
  httpGet:
    path: /health
    port: 8080
  initialDelaySeconds: 10
  periodSeconds: 15
  failureThreshold: 3
```

**Kubernetes readiness probe**

```yaml
readinessProbe:
  httpGet:
    path: /health
    port: 8080
  initialDelaySeconds: 5
  periodSeconds: 10
```

**Docker HEALTHCHECK**

```dockerfile
HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=3 \
  CMD curl -sf http://localhost:8080/health || exit 1
```

**Load balancer health check** — Point the health check to `GET http://oai-nef:8080/health`. Any `200` response indicates the instance is ready to serve traffic. Remove the instance from the pool on probe failure or when `nfStatus` is `DEREGISTERED`.

---

## Inbound Notification Endpoint

### `POST /nef-notify/v1/notify/{nf_sub_id}`

NEF uses this endpoint to **receive** inbound event notifications from southbound NFs — AMF, SMF, and PCF. When NEF subscribes to a peer NF (for example, subscribing to AMF via `Namf_EventExposure` on behalf of an AF), it provides this URL as the callback URI. The peer NF calls this endpoint when a subscribed event occurs.

> **This endpoint is not intended for AF or operator use.** It is a machine-to-machine interface between NEF and the 5GC NFs it has subscribed to. AFs should never call this endpoint. Restrict network access to this path to the internal 5GC network segment.

### Path Parameter

| Parameter | Type | Description |
|---|---|---|
| `nf_sub_id` | string | Internal subscription tracking ID assigned by NEF when it created the southbound NF subscription. Opaque to external callers. |

### Request

The request body varies by source NF:

| Source NF | Body Format | TS Reference |
|---|---|---|
| AMF | `Namf_EventExposure` notification body | TS 29.518 §5.2.6 |
| SMF | `Nsmf_EventExposure` notification body | TS 29.508 §4.2.6 |
| PCF | `Npcf_PolicyAuthorization` notification body | TS 29.514 §4.2.6 |

**Response: 204 No Content** — NEF acknowledges receipt. If NEF cannot find the `nf_sub_id` in its internal state, it returns `404 Not Found` so the calling NF can clean up its side of the subscription.

### Notification Flow

```
sequenceDiagram
    AMF->>NEF: POST /nef-notify/v1/notify/{nf_sub_id}
    NEF->>NEF: Look up AF subscription by nf_sub_id
    NEF->>AF: POST {notificationURI}
    AF-->>NEF: 204 No Content
    NEF-->>AMF: 204 No Content
```

**Step-by-step**:

1. AMF (or SMF/PCF) fires a southbound notification to NEF at the registered callback URI (`/nef-notify/v1/notify/{nf_sub_id}`).
2. NEF resolves `nf_sub_id` to the original AF subscription in its internal subscription table.
3. NEF translates the internal NF event format into the T8 northbound format using `nef_notification_mapper`.
4. NEF POSTs the translated notification to the AF's `notificationURI`.
5. NEF returns `204 No Content` to the originating NF.

### curl example (debugging purposes only)

This example illustrates the format of an AMF-to-NEF notification. AFs do not call this endpoint.

```bash
# Called BY AMF → NEF. Shown for debugging and integration tracing only.
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/nef-notify/v1/notify/sub-amf-001 \
  -H "Content-Type: application/json" \
  -d '{
    "subscriptionId": "sub-amf-001",
    "reportList": [
      {
        "type": "LOSS_OF_CONNECTIVITY",
        "supi": "imsi-208950000000001",
        "timeStamp": "2026-04-25T14:22:31Z"
      }
    ]
  }'
```

**Expected response**: `HTTP/2 204`

### PCF notification example

```bash
# Called BY PCF → NEF for a QoS or traffic influence notification.
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/nef-notify/v1/notify/sub-pcf-042 \
  -H "Content-Type: application/json" \
  -d '{
    "appSessionId": "pcf-appsession-9f3b2c1a",
    "evNotifs": [
      {
        "event": "QOS_NOTIF",
        "qosNotifType": "GUARANTEED",
        "flows": [{"flowNumber": 1, "packFiltResults": []}]
      }
    ]
  }'
```

### Security Recommendation

The `/nef-notify/v1/notify/` path should be reachable **only** from the internal 5GC network segment where AMF, SMF, and PCF reside. It should not be exposed to external networks or AF clients. Use firewall rules, network policies (Kubernetes `NetworkPolicy`), or Docker network segmentation to enforce this.

---

## Related Pages

- [API Overview](overview.md) — Common HTTP/2 and authentication conventions
- [Nnef_EventExposure SBI API](nnef-event-exposure.md) — Internal NF event subscriptions
- [Monitoring Event API](monitoring-event.md) — How AF subscriptions are created that trigger southbound AMF subscriptions
