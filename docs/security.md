# Security Guide

## 1. Security Architecture Overview

OAI NEF provides a layered security model for authenticating and authorizing external Application Functions (AFs). When an AF sends a request to the NEF northbound APIs, NEF first establishes the AF's identity through either a JWT Bearer token or a per-AF pre-shared API key. Once the identity is known, NEF checks it against an in-memory AF whitelist to determine whether the request is authorized for the requested service. Both mechanisms are optional and independently configurable; when neither is configured, the `insecure_dev_mode` flag determines whether requests are passed or rejected.

By default, `insecure_dev_mode` is `false`, which means that if no JWT secret is set and no API keys are configured, NEF rejects all requests with `401 Unauthorized`. This is the safe, fail-closed default suitable for any shared or production environment. Setting `insecure_dev_mode: true` switches NEF to a fail-open mode intended strictly for local development. In that mode, unauthenticated requests are accepted and the AF ID is inferred from the URL path parameter rather than a validated token.

```mermaid
flowchart TD
    A([Incoming Request]) --> B[Extract Authorization header]
    B --> C{jwt_secret configured?}
    C -- Yes --> D[Validate HMAC-SHA256 signature]
    D -- Invalid --> E([401 Unauthorized])
    D -- Valid --> F[Extract 'sub' claim as AF ID]
    C -- No --> G{API key configured for AF?}
    G -- Yes --> H[Validate X-API-Key header]
    H -- Invalid --> I([401 Unauthorized])
    H -- Valid --> J[AF ID from api_key entry]
    G -- No --> K{insecure_dev_mode enabled?}
    K -- No --> L([401 Unauthorized])
    K -- Yes --> M[AF ID from URL path parameter]
    F --> N[Look up AF ID in whitelist]
    J --> N
    M --> N
    N --> O{AF found in whitelist?}
    O -- Not found and whitelist non-empty --> P([403 Forbidden])
    O -- Found --> Q{API in allowed_apis?}
    Q -- Not in allowed_apis and list non-empty --> R([403 Forbidden])
    Q -- Allowed --> S([Handle Request])
    O -- Whitelist empty and insecure_dev_mode --> S
```

---

## 2. AF Identity & Authorization Model

Every NEF API request is associated with an **AF identity** (`af_id`), which is a string that uniquely identifies the calling Application Function. The source of this identity depends on the configured authentication method:

- **JWT enabled**: the `sub` (subject) claim of the validated JWT is used as the AF ID.
- **API key enabled (no JWT)**: the AF ID comes from the whitelist entry whose `api_key` matches the `X-API-Key` header value.
- **No auth configured + `insecure_dev_mode: true`**: the AF ID is taken directly from the `{scsAsId}` or `{afId}` path parameter in the URL. No signature or secret is verified.

NEF maintains an in-memory list of authorized AFs as `nef_af_profile` objects (see `src/nef_app/nef_af_profile.hpp`). Each profile records the AF's ID, optional API key, and the set of NEF services (`allowed_apis`) the AF is permitted to call. On every incoming request, NEF:

1. Resolves the AF ID using the method above.
2. Looks up the AF ID in the whitelist.
3. If the whitelist is non-empty and the AF ID is not found, returns `403 Forbidden`.
4. If the AF is found, checks that the requested API service name appears in `allowed_apis`.
5. If `allowed_apis` is empty for that AF entry, all services are permitted for that AF.

---

## 3. JWT Bearer Token Authentication

### Token Format

NEF uses standard JSON Web Tokens (JWT, RFC 7519) signed with HMAC-SHA256 (`"alg": "HS256"`). The `sub` claim carries the AF identifier string that NEF uses to perform whitelist lookups. No other claims are required by NEF, but standard claims such as `iat` (issued-at) and `exp` (expiry) are accepted and forwarded transparently — NEF does not validate `exp`.

### Enabling JWT

Set `nef.security.jwt_secret` to a non-empty string in `etc/config.yaml`:

```yaml
nef:
  security:
    jwt_secret: "replace-with-a-strong-random-secret-32bytes+"
    insecure_dev_mode: false
```

When `jwt_secret` is non-empty, JWT validation is active for all incoming requests. An empty `jwt_secret` disables JWT validation entirely.

### Token Validation Steps

1. Extract the value of the `Authorization` HTTP header.
2. Strip the `Bearer ` prefix to obtain the raw JWT string.
3. Split the JWT into `header.payload.signature` parts.
4. Recompute HMAC-SHA256 over `header.payload` using the configured `jwt_secret`.
5. Compare the recomputed signature against the provided signature (constant-time comparison).
6. If the signatures do not match, return `401 Unauthorized`.
7. Base64url-decode the payload and extract the `sub` claim.
8. If `sub` is absent or empty, return `401 Unauthorized`.
9. Use the `sub` value as the AF ID for whitelist lookup.

### Example Authorization Header

```
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJteS1hZi0xIiwiaWF0IjoxNzE0MDAwMDAwLCJleHAiOjE3MTQwODY0MDB9.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c
```

### Example JWT Payload (decoded)

```json
{
  "sub": "my-af-1",
  "iat": 1714000000,
  "exp": 1714086400
}
```

### Curl Example

```bash
# Obtain or pre-generate a JWT signed with your jwt_secret, then:
curl -s --http2-prior-knowledge \
  -H "Authorization: Bearer ${JWT_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"eventType":"loss_of_connectivity","notificationURI":"http://af.example.com/notify","monitoringType":"SINGLE_UE","supi":"imsi-208950000000001"}' \
  http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions
```

---

## 4. API Key Authentication

### Overview

NEF supports per-AF pre-shared API keys as an alternative (or complement) to JWT. Each AF whitelist entry may include an `api_key` field. When present, NEF checks the `X-API-Key` HTTP header on every incoming request against the configured key.

### Configuration

```yaml
nef:
  af_whitelist:
    - af_id: "my-af-1"
      api_key: "secret-key-abc"
      allowed_apis:
        - 3gpp-monitoring-event
        - 3gpp-traffic-influence
    - af_id: "my-af-2"
      # no api_key = key not checked for this AF
      # no allowed_apis = all APIs allowed for this AF
```

### Request Header

```
X-API-Key: secret-key-abc
```

### Curl Example

```bash
curl -s --http2-prior-knowledge \
  -H "X-API-Key: secret-key-abc" \
  -H "Content-Type: application/json" \
  -d '{"eventType":"ue_reachability_for_data","notificationURI":"http://af.example.com/notify","monitoringType":"SINGLE_UE","supi":"imsi-208950000000001"}' \
  http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions
```

### Interaction with JWT

JWT and API key authentication can be used together. When both are configured:

- JWT takes **precedence for identity**: if a valid JWT is present, the `sub` claim becomes the AF ID regardless of the `X-API-Key` header.
- API key validation occurs only when no JWT secret is configured, or when the request does not carry an `Authorization` header.

---

## 5. AF Whitelist

### Structure

The AF whitelist is configured under `nef.af_whitelist` in `etc/config.yaml` as a list of AF profile objects. Each object has the following fields:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `af_id` | string | yes | AF identifier; exact string match against the identity resolved from the token or path |
| `api_key` | string | no | Pre-shared key; if set, `X-API-Key` header must match this value |
| `allowed_apis` | list of strings | no | Service names the AF is allowed to call; empty list means all services are permitted |

### Matching Rules

- `af_id` matching is **exact string comparison** — no wildcards, no regex, no case-folding.
- `allowed_apis` values correspond to the API service identifier prefix (e.g., `3gpp-monitoring-event`, `3gpp-traffic-influence`, `3gpp-pfd-management`, `nnef-pfdmanagement`, `3gpp-as-session-with-qos`, `3gpp-bdt`, `3gpp-analyticsexposure`, `nnef-eventexposure`).

### Empty Whitelist Behavior

| `af_whitelist` | `insecure_dev_mode` | Result |
|----------------|---------------------|--------|
| empty | `false` | **All requests rejected** with `401 Unauthorized` (safe default) |
| empty | `true` | **All AFs permitted** for all services (development mode only) |
| non-empty | either | Only listed AFs are authorized; unrecognized AF IDs receive `403 Forbidden` |

### Example: Two AFs with Different Restrictions

```yaml
nef:
  security:
    jwt_secret: "production-secret-minimum-32-bytes-long"
    insecure_dev_mode: false
  af_whitelist:
    # Unrestricted: allowed to call any NEF service
    - af_id: "platform-af"
      # no api_key = verified by JWT only
      # no allowed_apis = all services permitted

    # Restricted: monitoring events only
    - af_id: "monitoring-af"
      api_key: "monitor-key-xyz"
      allowed_apis:
        - 3gpp-monitoring-event
```

---

## 6. `insecure_dev_mode` Warning

> **⚠️ WARNING: Never use `insecure_dev_mode: true` in production deployments.**
>
> When `insecure_dev_mode: true` is set AND no JWT secret or API keys are configured, ALL requests pass authentication checks regardless of origin. The AF identity is taken directly from the URL path parameter without any cryptographic verification — any caller can claim any AF ID. This setting is intended for local development and testing only. Production deployments MUST set `insecure_dev_mode: false` and configure JWT and/or API keys.

The `insecure_dev_mode` flag exists solely to enable a frictionless first-run experience when bringing up a local NEF instance. It has no role in a secured deployment. Leaving it enabled in any shared, staged, or production environment constitutes a critical security misconfiguration that exposes all 5GC subscriber data and policy controls to unauthenticated callers.

---

## 7. HTTP/2 Security Hardening

The NEF HTTP/2 server (`src/api-server/http2-server.cpp`, nghttp2 v1.68.1) includes built-in mitigations against known HTTP/2 protocol-level attacks:

### CVE-2023-44487 — HTTP/2 Rapid Reset Attack

**Threat**: An attacker opens and immediately resets large numbers of HTTP/2 streams to exhaust server resources without completing any requests.

**Mitigation**: A token-bucket rate limiter is applied to incoming stream creation:
- Burst capacity: **1000 tokens** (allows a short burst of concurrent streams)
- Refill rate: **33 tokens/second**

Streams that exceed the rate limit are rejected before handler dispatch. The connection is not terminated, but the excess streams receive `REFUSED_STREAM`.

### CVE-2024-28182 — CONTINUATION Flood

**Threat**: An attacker sends a HEADERS frame followed by an unbounded sequence of CONTINUATION frames, causing the server to buffer header data indefinitely.

**Mitigation**: NEF enforces a hard cap of **16 CONTINUATION frames** per HEADERS sequence. If the limit is exceeded, the connection is closed with a `PROTOCOL_ERROR` GOAWAY frame.

### RST_STREAM Abuse

**Threat**: An attacker issues large numbers of RST_STREAM frames to waste server processing cycles without completing stream handshakes.

**Mitigation**: NEF tracks the RST_STREAM count per connection. After **200 RST_STREAM frames** on a single connection, the connection is forcibly closed.

---

## 8. Known Limitations

### No TLS

NEF operates on HTTP/2 cleartext (`h2c`) only. There is no TLS termination at the NEF application layer. For production deployments, place NEF behind a TLS-terminating reverse proxy such as [Envoy](https://www.envoyproxy.io/) or [nginx](https://nginx.org/) that handles HTTPS externally and forwards `h2c` to NEF internally.

### No Mutual TLS (mTLS)

Client certificate authentication is not supported. AF identity relies entirely on JWT or API key mechanisms described above.

### JWT Secret Rotation

Changing `nef.security.jwt_secret` in `config.yaml` requires a **full NEF restart**. There is no hot-reload mechanism for configuration. Plan a maintenance window for JWT secret rotation; outstanding tokens signed with the old secret will be immediately rejected after restart.

### AF ID Matching

AF ID comparison uses exact string equality only. There is no support for wildcard patterns, prefix matching, or regular expressions in the whitelist. Each AF ID must be listed individually.

---

## 9. Production Security Checklist

Before exposing NEF to any network traffic from real AFs or in a shared infrastructure environment, verify all of the following:

- [ ] `insecure_dev_mode: false` is set in `etc/config.yaml`
- [ ] `jwt_secret` is set to a strong random secret of at least 32 bytes (e.g., generated with `openssl rand -base64 32`)
- [ ] `af_whitelist` is populated with all authorized AF identifiers; no entries are missing
- [ ] `allowed_apis` is restricted to the minimum set of services required per AF
- [ ] NEF is deployed behind a TLS-terminating reverse proxy (Envoy, nginx, or equivalent)
- [ ] The `/nef-notify/v1/notify/` callback path is accessible only to internal 5GC NFs (AMF, SMF, PCF) via network policy or firewall rules; it must not be reachable by external AFs
- [ ] `jwt_secret` rotation is scheduled periodically and the restart procedure is documented in your runbook
