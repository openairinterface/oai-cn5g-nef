<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# NEF API Reference — Overview

Read this page before any service-specific API reference. Everything here applies to every NEF
northbound API: how to reach the server, how to authenticate, what a response looks like when
things go wrong, and how notifications come back.

If you only remember three things from this page: the transport is cleartext HTTP/2 with no
negotiation, every authorization failure is a `403`, and an oversized body kills the stream rather
than returning a status code.

**Contents**

| Section | Answers |
|---|---|
| [Base URL](#base-url) and [HTTP protocol](#http-protocol-requirements) | Where do I send the request, and how? |
| [API versioning](#api-versioning) | What does the `/v1/` in the path mean? |
| [Authentication](#authentication) | What credentials do I present? |
| [Request body format](#request-body-format) | What are the limits on what I send? |
| [AF / SCS-AS ID path parameter](#af--scs-as-id-path-parameter) | Which identifier goes in the path? |
| [Response status codes](#response-status-codes) and [error format](#error-response-format) | What can come back, and what does it mean? |
| [Subscription identifiers](#subscription-identifiers) | How do I address a resource after creating it? |
| [Notification callbacks](#notification-callback-pattern) | How do events reach me? |
| [Rate limiting](#rate-limiting) | When will I be throttled? |
| [Service identifiers](#service-identifiers) | Which page documents the API I want? |

---

## Base URL

Every NEF API is served from one host and one port. There is no separate management or admin
port, and no TLS at the application layer.

```
http://{nef-host}:{port}
```

| Component | Default | Where it comes from |
|-----------|---------|---------------------|
| `nef-host` | `oai-nef` | FQDN or IP; `nfs.nef.host` in `config.yaml` |
| `port` | `8080` | `nfs.nef.sbi.port` in `config.yaml` |

So a typical base URL is:

```
http://oai-nef:8080
```

The container images also expose `9090/tcp` alongside `80/tcp`, so a deployment that remaps the
SBI port will differ from the `8080` above. The port the process actually binds is whatever
`nfs.nef.sbi.port` says.

---

## HTTP Protocol Requirements

NEF speaks **HTTP/2 cleartext (h2c)** and nothing else. The server is built directly on the
nghttp2 C API with libevent; there is no TLS layer and no HTTP/1.1 path, so a client has to open
the connection with HTTP/2 Prior Knowledge.

| Requirement | Detail |
|---|---|
| Protocol | HTTP/2 (RFC 7540) |
| Transport | Cleartext TCP (h2c); no TLS |
| Upgrade | HTTP/2 Prior Knowledge required; the `Upgrade: h2c` handshake is not implemented |
| Library | nghttp2 v1.68.1 |

With `curl`, that means `--http2-prior-knowledge` on every call:

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <token>" \
     http://oai-nef:8080/health
```

> **Note:** A client that opens the connection as HTTP/1.1, or that tries to upgrade with
> `Upgrade: h2c`, will not get a response it can parse. This applies to health probes too — see
> [operational endpoints](operational-endpoints.md) for probe configurations that work.

---

## API Versioning

Every path carries a version component immediately after the service root:

```
/3gpp-monitoring-event/v1/{scsAsId}/subscriptions
                        ^^^
                        version
```

`v1` is the only version that exists. The string is read from `nfs.nef.sbi.api_version` in
`config.yaml` and concatenated onto each service's base path at startup, so changing it renames
the prefix NEF registers — it does not add support for a second version, and it does not keep the
old one working.

---

## Authentication

NEF has one credential that matters (a JWT bearer token), one optional extra (a per-AF API key),
and one escape hatch for development (fail-open mode). Which of the three is in force depends on
`jwt_secret`, `af_whitelist` and `insecure_dev_mode` in `config.yaml`.

**Every failure in this section is a `403` with a ProblemDetails body. NEF never emits `401`** —
there is no `UNAUTHORIZED` status anywhere in its source.

### JWT bearer token

The primary mechanism. The caller presents an HMAC-SHA256 signed JWT in the `Authorization`
header:

```http
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
```

| Field | Value |
|-------|-------|
| Signing algorithm | HMAC-SHA256 (`HS256`) |
| `sub` claim | AF/SCS-AS identifier; must match the `af_id` in the AF whitelist, when a whitelist is configured |
| Expiry (`exp`) | Recommended. A token without `exp` is accepted, but do not rely on that in production |

NEF validates the signature against `nef.security.jwt_secret`. An invalid signature, an expired
token and a missing `Authorization` header all produce the same `403`.

A decoded payload looks like this:

```json
{
  "sub": "my-af-1",
  "iat": 1745539200,
  "exp": 1745625600
}
```

### API key (parsed but NOT enforced)

A whitelist entry may carry an `api_key` field, and it is read from the configuration, but nothing
in the request path checks it. `validate_api_key()` exists on the AF profile and has no callers; no
handler reads an `X-API-Key` header. Do not rely on it as an access control: an AF that passes the
whitelist or JWT check is authorized regardless of any `api_key` value. The field is retained for a
future implementation only.

### Development mode (no authentication)

NEF accepts every request without credentials only when all three of these hold at once:

1. `nef.security.jwt_secret` is empty (`""`)
2. `nef.af_whitelist` is empty (`[]`)
3. `nef.security.insecure_dev_mode` is `true`

With the first two empty and `insecure_dev_mode` left at `false`, NEF is fail-closed instead and
denies everything.

> **Warning:** `insecure_dev_mode: true` disables authentication enforcement entirely — any caller
> can invoke any endpoint. The shipped `etc/config.yaml` has it enabled. Set a non-empty
> `jwt_secret` or a non-empty `af_whitelist` for any deployment reachable from an untrusted
> network.

---

## Request Body Format

Write operations (`POST`, `PUT`, `PATCH`) take a UTF-8 JSON body with
`Content-Type: application/json`, and the body has a hard 1 MiB ceiling.

The ceiling is worth reading carefully, because it does not behave like the other limits on this
page:

| Limit | Value |
|---|---|
| Maximum body size | 1 MiB (1,048,576 bytes), the server's `max_request_body_size` |
| What happens on overflow | The HTTP/2 layer sends **`RST_STREAM` with `ENHANCE_YOUR_CALM`** while the body is still arriving |

There is no status code and no ProblemDetails body for an oversized request. The client sees a
reset stream, not a `4xx`. If your client reports a broken or cancelled stream on a large `POST`,
check the body size first.

---

## AF / SCS-AS ID Path Parameter

Most northbound APIs put the calling application's identity in the path, under one of two names:

- `{scsAsId}` — Monitoring Event, AS Session with QoS / QoS Monitoring (TS 29.122), PFD
  Management, BDT Policy
- `{afId}` — Traffic Influence, Analytics Exposure

They mean the same thing: the SCS/AS or AF that owns the subscriptions being managed. The value
has to agree with the authenticated identity:

- Under JWT authentication, it must match the `sub` claim.
- Under API key authentication, it must match the `af_id` of the whitelist entry holding the
  presented key.

Reaching for another AF's subscriptions returns `403 Forbidden`.

---

## Response Status Codes

These are the status codes NEF actually emits, derived from the `http_status_code::` constants
referenced in `src/nef_app/` and `src/api-server/`.

| Status Code | Meaning | When Used |
|---|---|---|
| `200 OK` | Success with body | `GET` and `PUT` (update) responses; subscription read operations |
| `201 Created` | Resource created | `POST` subscription creation; `Location` header set from the response body's `self`, where the service produces one |
| `204 No Content` | Success, no body | `DELETE` operations |
| `400 Bad Request` | Invalid request | Malformed JSON, missing required field, invalid path parameter |
| `403 Forbidden` | Authentication or authorization failure | Missing, invalid or expired JWT; AF not in the whitelist; `allowed_apis` restriction. **NEF has no `401` path.** |
| `404 Not Found` | Resource not found | Subscription ID does not exist; may also mean the subscription expired and was auto-deleted |
| `405 Method Not Allowed` | Wrong HTTP method for the path | A route matched but the method is not supported on that resource |
| `422 Unprocessable Entity` | Model validation failure | The body parsed as JSON but failed the generated model's `validate()` |
| `429 Too Many Requests` | Rate limit exceeded | Token bucket for this caller is empty |
| `500 Internal Server Error` | Unexpected server error | Unhandled exception or internal state corruption; inspect NEF logs |
| `502 Bad Gateway` | Southbound failure | A required peer NF (AMF, SMF, PCF or UDR) errored or was unreachable, under a FATAL-502 continuation policy |
| `503 Service Unavailable` | NEF cannot accept the request | The server is draining for graceful shutdown, or the dispatcher queue is full or stopped |
| `504 Gateway Timeout` | Southbound timeout | The peer NF call timed out — a southbound `408` maps to `504` |

Two things NEF never sends: `401 Unauthorized` and `409 Conflict`. A southbound `303 See Other` is
treated as success (BDT policy creation) and answered northbound as `201 Created`; `303` is never
returned to the AF.

---

## Error Response Format

Every error response is RFC 7807 Problem Details (`application/problem+json`), with the same four
fields and nothing else:

| Field | Type | Description |
|-------|------|-------------|
| `type` | string (URI) | Problem type URI; always `"about:blank"` |
| `title` | string | Derived from the status code, so always the standard reason phrase |
| `status` | integer | HTTP status code; identical to the response status line |
| `detail` | string | Human-readable explanation specific to this occurrence |

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Invalid event type: unknown_event"
}
```

Some `detail` strings are fixed literals in the source, and it is worth knowing which, because
matching on them is the only way to tell two same-status cases apart.

**Authorization failure** — one literal, in `nef_app::reject_unauthorized_af`. It is not per-AF or
per-API:

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF not authorized for this service"
}
```

The Nnef variant reads `"NF not authorized for this Nnef service"`.

**Rate limit** — also a fixed literal:

```json
{
  "type": "about:blank",
  "title": "Too Many Requests",
  "status": 429,
  "detail": "Rate limit exceeded"
}
```

**Service unavailable** — two distinct literals, and they mean different things. `"Server is
draining"` means NEF is shutting down and you should stop sending; `"Server is overloaded, please
retry later"` means the dispatcher queue rejected the task and a retry is reasonable.

```json
{
  "type": "about:blank",
  "title": "Service Unavailable",
  "status": 503,
  "detail": "Server is overloaded, please retry later"
}
```

Illustrative `detail` strings elsewhere in this reference show the *shape* of a message, not its
literal text.

---

## Subscription Identifiers

When NEF creates a subscription it assigns a UUID v4, for example
`3fa85f64-5717-4562-b3fc-2c963f66afa6`. That identifier is the last path segment of the
subscription's own URI, and it is what you use in the follow-up `GET`, `PUT` and `DELETE`.

Where the identifier appears in the response body is service-specific — the per-service pages
give the exact shape. Two examples of the range:

- Nnef_EventExposure returns both `subscriptionId` and a `self` URI.
- Monitoring Event (T8) echoes the request body back with a `subId` field added.

Where a service does produce a `self` string on a `201`, NEF copies it into the `Location` header.
QoS Monitoring and BDT Policy make both the header and the `self` field absolute by prefixing the
server address; the others leave `self` as a path.

---

## Notification Callback Pattern

Subscriptions are asynchronous. When a subscribed event fires, NEF makes an outbound `POST` to the
`notificationURI` you supplied at subscription time. The body is JSON, with a structure specific
to the service — see the per-service pages.

```json
{
  "subscriptionId": "3fa85f64-5717-4562-b3fc-2c963f66afa6",
  "eventType": "loss_of_connectivity",
  "ueId": "imsi-208950000000001",
  "timestamp": "2026-04-25T12:34:56Z"
}
```

What the AF has to provide:

1. An HTTP endpoint at the `notificationURI`, reachable from NEF's network.
2. A `200 OK` or `204 No Content` acknowledgement. NEF does not retry a notification that gets a
   non-2xx response in this release.
3. An `http://` URL. NEF's outbound client is built with TLS disabled, so an `https://`
   `notificationURI` will not be delivered even though the URI validator accepts the scheme.

### Callback URIs NEF will refuse

`validate_callback_uri` rejects a subscription up front if the `notificationURI` points somewhere
NEF should not be aimed at. This is deliberate anti-SSRF filtering, and it catches ordinary
lab setups, so check it first when a subscription is rejected as malformed.

Rejected as malformed or unsupported: an empty URI, a URI with no `://`, a scheme other than
`http` or `https`, an empty host, an unclosed `[` in an IPv6 literal, or a bracketed IPv6 literal
that is not a valid address.

Rejected by address range:

| Family | Blocked ranges |
|---|---|
| IPv4 | loopback `127.0.0.0/8`, link-local `169.254.0.0/16`, RFC 1918 `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16` |
| IPv6 | loopback `::1`, link-local `fe80::/10`, ULA `fc00::/7` |

A host that is not an IP literal is accepted without a DNS lookup, so a DNS name that resolves
into one of those ranges — which is the normal case on a Docker or Kubernetes network — passes
validation. Use a name rather than a private IP literal.

---

## Rate Limiting

NEF runs a token bucket per caller. A caller is identified by its bearer token when one is
present, and by its peer IP address otherwise — so it is per-credential, not global, and an
unauthenticated flood from one source cannot exhaust another's budget.

| Property | Value |
|----------|-------|
| Algorithm | Token bucket, refilled continuously |
| Key | Bearer token if present, else peer address |
| Sustained rate | 100 requests per second per caller (default) |
| Burst capacity | 200 requests (default), which is also how full a new bucket starts |
| Response when exhausted | `429 Too Many Requests` |
| Retry hint | None. There is no `Retry-After` header; apply exponential backoff |

`GET /health` is exempt — it does not go through the rate limiter at all.

### HTTP/2 Rapid Reset

Separately from the application-level limiter, the connection itself is protected against
HTTP/2 Rapid Reset (CVE-2023-44487) by two mechanisms:

- nghttp2's stream-reset rate limiter, configured with a burst of 1,000 `RST_STREAM` frames
  refilling at 33 per second. Exhausting it makes the library send `GOAWAY` and close the
  connection.
- A manual per-connection counter that closes a connection once it has sent more than 200
  `RST_STREAM` frames carrying `NGHTTP2_CANCEL`.

A `CONTINUATION` flood guard (CVE-2024-28182) caps `CONTINUATION` frames per `HEADERS` sequence at
16, and outstanding unacknowledged `PING` frames are capped at 1,000.

---

## Service Identifiers

Every NEF northbound service, its base path, the specification that governs it, and the page that
documents it.

| Service Name | Base Path | Spec | Documentation Page |
|---|---|---|---|
| Nnef_EventExposure (SBI) | `/nnef-eventexposure/v1/` | TS 29.591 | [nnef-event-exposure.md](nnef-event-exposure.md) |
| Monitoring Event (T8) | `/3gpp-monitoring-event/v1/` | TS 29.122 | [monitoring-event.md](monitoring-event.md) |
| Traffic Influence | `/3gpp-traffic-influence/v1/` | TS 29.522 | [traffic-influence.md](traffic-influence.md) |
| PFD Management (T8) | `/3gpp-pfd-management/v1/` | TS 29.122 | [pfd-management.md](pfd-management.md) |
| Nnef_PFDmanagement (SBI) | `/nnef-pfdmanagement/v1/` | TS 29.551 | [nnef-pfd-management.md](nnef-pfd-management.md) |
| QoS Monitoring | `/3gpp-as-session-with-qos/v1/` | TS 29.122 | [qos-monitoring.md](qos-monitoring.md) |
| BDT Policy Control | `/3gpp-bdt/v1/` | TS 29.122 | [bdt-policy.md](bdt-policy.md) |
| Analytics Exposure | `/3gpp-analyticsexposure/v1/` | TS 29.522 | [analytics-exposure.md](analytics-exposure.md) |
| Health Check | `/health` | — | [operational-endpoints.md](operational-endpoints.md) |
| NF Notification Receiver | `/nef-notify/v1/notify/` | — | [operational-endpoints.md](operational-endpoints.md) |
