# NEF API Reference — Overview

Read this page before any service-specific API reference. It describes the common conventions, authentication model, HTTP/2 requirements, error format, and patterns that apply to every NEF northbound REST API.

---

## Base URL

```
http://{nef-host}:{port}
```

| Component | Default | Notes |
|-----------|---------|-------|
| `nef-host` | `oai-nef` | FQDN or IP; configured via `nef.host` in `config.yaml` |
| `port` | `8080` | Configured via `nef.sbi.port`; HTTP/2 port is `9090` in Docker Compose deployments |

All NEF APIs are served under the same host and port. There is no separate management or admin port. No TLS is provided at the application layer; the server listens on cleartext HTTP/2 (h2c) only.

**Example base URL:**

```
http://oai-nef:8080
```

---

## HTTP Protocol Requirements

NEF operates exclusively on **HTTP/2 cleartext (h2c)**. HTTP/1.1 is not accepted on the primary SBI interface.

| Requirement | Detail |
|---|---|
| Protocol | HTTP/2 (RFC 7540) |
| Transport | Cleartext TCP (h2c); no TLS |
| Upgrade | HTTP/2 Prior Knowledge required; h2 upgrade via `Upgrade: h2c` is not supported |
| Library | nghttp2 v1.68.1 |

To communicate with NEF using `curl`, use `--http2-prior-knowledge`:

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <token>" \
     http://oai-nef:8080/health
```

> **Note:** If your HTTP client attempts an HTTP/1.1 connection or uses `Upgrade: h2c`, the connection will be rejected. Configure your client to use HTTP/2 Prior Knowledge (also called h2c with direct connection).

---

## API Versioning

All API paths include a `/v1/` version component immediately after the service root:

```
/3gpp-monitoring-event/v1/{scsAsId}/subscriptions
                        ^^^
                        version
```

Currently, only version `v1` is available. The version string is configurable in `config.yaml` under `nef.sbi.api_version`, but changing it does not add support for any other version — it only adjusts the path prefix that NEF registers.

---

## Authentication

NEF supports three authentication modes. The active mode is determined by the combination of `jwt_secret`, `af_whitelist`, and `insecure_dev_mode` configuration parameters.

### JWT Bearer Token

The primary authentication mechanism. The caller presents an HMAC-SHA256 signed JWT in the `Authorization` header:

```http
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
```

**Token requirements:**

| Field | Value |
|-------|-------|
| Signing algorithm | HMAC-SHA256 (`HS256`) |
| `sub` claim | AF/SCS-AS identifier string; must match the `af_id` in the AF whitelist (if a whitelist is configured) |
| Expiry (`exp`) | Recommended; tokens without `exp` are accepted but not recommended for production |

NEF validates the token signature against the `jwt_secret` configured in `config.yaml`. An invalid signature, an expired token, or a missing `Authorization` header results in a `401 Unauthorized` response.

**Example decoded JWT payload:**

```json
{
  "sub": "my-af-1",
  "iat": 1745539200,
  "exp": 1745625600
}
```

### API Key (Optional, Per-AF)

If an AF entry in `af_whitelist` includes an `api_key` field, the caller must also present the key in the `X-API-Key` header:

```http
X-API-Key: s3cr3tK3y-changeme
```

The API key check is performed **in addition to** (not instead of) JWT validation when both are configured. If `api_key` is absent from a whitelist entry, the header is not checked for that AF.

### No Authentication (Development Mode)

When all three of the following are true simultaneously, NEF operates fail-open and accepts all requests without authentication:

1. `nef.security.jwt_secret` is empty (`""`)
2. `nef.af_whitelist` is empty (`[]`)
3. `nef.security.insecure_dev_mode` is `true`

> **Warning:** Development mode (`insecure_dev_mode: true`) disables all authentication enforcement. Any caller can invoke any API endpoint without presenting credentials. This mode must **never** be used in production or in any environment accessible from untrusted networks. Configure a non-empty `jwt_secret` or a non-empty `af_whitelist` for any non-development deployment.

---

## Request Body Format

All write operations (`POST`, `PUT`, `PATCH`) require a JSON body:

| Header | Required Value |
|--------|---------------|
| `Content-Type` | `application/json` |
| Encoding | UTF-8 |
| Maximum body size | 1 MiB (1,048,576 bytes) |

Requests exceeding 1 MiB are rejected with `400 Bad Request` before the body is parsed.

---

## AF / SCS-AS ID Path Parameter

Most northbound APIs include a path parameter that identifies the calling application:

- `{scsAsId}` — used in TS 29.122 APIs (Monitoring Event, PFD Management, BDT Policy)
- `{afId}` — used in TS 29.522 and TS 29.591 APIs (Traffic Influence, QoS Monitoring, Analytics)

Both parameters serve the same purpose: they identify the SCS/AS or AF that owns the subscriptions being managed. The value must be consistent with the authenticated identity:

- When JWT authentication is active, `{scsAsId}` / `{afId}` must match the `sub` claim in the JWT.
- When API key authentication is active, the value must match the `af_id` in the AF whitelist entry that holds the presented key.

Attempting to access subscriptions owned by a different AF identity returns `403 Forbidden`.

---

## Response Status Codes

| Status Code | Meaning | When Used |
|---|---|---|
| `200 OK` | Success with body | `GET` and `PUT` (update) responses; subscription read operations |
| `201 Created` | Resource created | `POST` subscription creation; `Location` or `self` link in response body |
| `204 No Content` | Success, no body | `DELETE` operations |
| `400 Bad Request` | Invalid request | Malformed JSON, missing required field, invalid enum value, body too large |
| `401 Unauthorized` | Authentication failure | Missing or invalid JWT; expired token; JWT secret configured but no token presented |
| `403 Forbidden` | Authorization failure | Valid identity but insufficient permissions; AF not in whitelist; `allowed_apis` restriction |
| `404 Not Found` | Resource not found | Subscription ID does not exist; may also indicate the subscription has expired and been auto-deleted |
| `409 Conflict` | Resource conflict | Duplicate subscription creation attempt with the same parameters |
| `429 Too Many Requests` | Rate limit exceeded | Token-bucket rate limit reached for the calling AF or globally; retry after cooling down |
| `500 Internal Server Error` | Unexpected server error | Unhandled exception or internal state corruption; inspect NEF logs |
| `503 Service Unavailable` | Downstream NF unavailable | The circuit breaker for a required peer NF (AMF, SMF, PCF, or UDR) is in the OPEN state |

---

## Error Response Format

All error responses use the RFC 7807 Problem Details for HTTP APIs format (`application/problem+json`):

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Invalid event type: unknown_event"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string (URI) | Problem type URI; currently always `"about:blank"` |
| `title` | string | Short human-readable summary of the problem type |
| `status` | integer | HTTP status code; identical to the response status line |
| `detail` | string | Human-readable explanation specific to this occurrence |

**Additional error examples:**

```json
{
  "type": "about:blank",
  "title": "Unauthorized",
  "status": 401,
  "detail": "JWT token validation failed: signature mismatch"
}
```

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF 'my-af-1' is not authorised to call monitoring_event"
}
```

```json
{
  "type": "about:blank",
  "title": "Too Many Requests",
  "status": 429,
  "detail": "Rate limit exceeded; retry after the token bucket refills"
}
```

```json
{
  "type": "about:blank",
  "title": "Service Unavailable",
  "status": 503,
  "detail": "Circuit breaker OPEN for AMF; downstream NF is unreachable"
}
```

---

## Subscription ID Format

When NEF creates a subscription in response to a `POST` request, it assigns a subscription identifier with the following properties:

- Format: UUID v4 string (e.g., `3fa85f64-5717-4562-b3fc-2c963f66afa6`)
- Returned in the response body in the `subscriptionId` field
- Also returned as a full URL in the `self` field (or `Location` header for some APIs)

**Example 201 response body:**

```json
{
  "self": "/3gpp-monitoring-event/v1/my-af-1/subscriptions/3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "subscriptionId": "3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "eventType": "loss_of_connectivity",
  "notificationURI": "http://af.example.com/notify",
  "monitorExpireTime": "2026-05-01T00:00:00Z"
}
```

The `self` URL value is the canonical URI for the subscription and can be used directly in subsequent `GET`, `PUT`, or `DELETE` requests.

---

## Notification Callback Pattern

When a subscribed event occurs in the network, NEF delivers an asynchronous notification to the AF by making an outbound `POST` request to the `notificationURI` provided at subscription time.

**Requirements on the AF side:**

1. The AF must expose an HTTP endpoint at the `notificationURI`.
2. The endpoint must be reachable from the NEF container's network.
3. The endpoint should return `200 OK` or `204 No Content` to acknowledge receipt. NEF does not retry notifications for non-2xx responses in this release.

**Notification body format:** JSON, with a structure specific to the subscribed service (see the service-specific API reference pages).

**Example notification (Monitoring Event):**

```json
{
  "subscriptionId": "3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "eventType": "loss_of_connectivity",
  "ueId": "imsi-208950000000001",
  "timestamp": "2026-04-25T12:34:56Z"
}
```

> **Note:** The `notificationURI` must use HTTP (not HTTPS), as NEF does not support TLS for outbound notification delivery in this release. Ensure the AF callback endpoint is not exposed to the public internet without additional network-layer controls.

---

## Rate Limiting

NEF implements a **token-bucket** rate limiter to protect the server from request floods.

| Property | Value |
|----------|-------|
| Algorithm | Token bucket |
| Scope | Global (not per-AF in this release) |
| Response when limit exceeded | `429 Too Many Requests` |
| Retry behaviour | No `Retry-After` header; callers should apply exponential backoff |

When the rate limit is exceeded, NEF returns:

```http
HTTP/2 429 Too Many Requests
Content-Type: application/problem+json

{
  "type": "about:blank",
  "title": "Too Many Requests",
  "status": 429,
  "detail": "Rate limit exceeded; retry after the token bucket refills"
}
```

Additionally, HTTP/2 Rapid Reset attacks (CVE-2023-44487) are mitigated at the connection level: a burst limit of 1,000 RST_STREAM frames per connection is enforced, and connections that exceed 200 RST_STREAM frames in total are closed.

---

## Service Identifiers

The table below lists every NEF northbound service with its canonical base path, governing specification, and documentation page.

| Service Name | Base Path | Spec | Documentation Page |
|---|---|---|---|
| Nnef_EventExposure (SBI) | `/nnef-eventexposure/v1/` | TS 29.591 | [nnef-event-exposure.md](nnef-event-exposure.md) |
| Monitoring Event (T8) | `/3gpp-monitoring-event/v1/` | TS 29.122 | [monitoring-event.md](monitoring-event.md) |
| Traffic Influence | `/3gpp-traffic-influence/v1/` | TS 29.522 | [traffic-influence.md](traffic-influence.md) |
| PFD Management (T8) | `/3gpp-pfd-management/v1/` | TS 29.551 | [pfd-management.md](pfd-management.md) |
| Nnef_PFDmanagement (SBI) | `/nnef-pfdmanagement/v1/` | TS 29.551 | [nnef-pfd-management.md](nnef-pfd-management.md) |
| QoS Monitoring | `/3gpp-as-session-with-qos/v1/` | TS 29.522 | [qos-monitoring.md](qos-monitoring.md) |
| BDT Policy Control | `/3gpp-bdt/v1/` | TS 29.122 | [bdt-policy.md](bdt-policy.md) |
| Analytics Exposure | `/3gpp-analyticsexposure/v1/` | TS 29.591 | [analytics-exposure.md](analytics-exposure.md) |
| Health Check | `/health` | — | [operational-endpoints.md](operational-endpoints.md) |
| NF Notification Receiver | `/nef-notify/v1/notify/` | — | [operational-endpoints.md](operational-endpoints.md) |
