<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Traffic Influence API (TS 29.522)

## What this service is for

An AF uses Traffic Influence (TI) to ask the network to steer a selected traffic stream towards a
particular DNAI — an edge data centre, a local breakout, a CDN node — instead of the default path
out of the UPF.

You describe *which* traffic (by SUPI, external group id, DNN, S-NSSAI, application id or IP flow
filters) and *where* it should go (a list of `trafficRoutes`), and NEF turns that into an
`Npcf_PolicyAuthorization` application session on the PCF, which becomes PCC rules on the PDU
sessions via SMF. The same subscription also records the influence data in the UDR so it survives
as subscriber data.

The typical use is edge computing: an AF that has brought up an application instance in a
particular edge site asks for its users' traffic to be routed there, and asks to be told when the
DNAI actually changes so it can relocate application state.

This page is the complete reference for the northbound T8 API. Read the
[API Reference Overview](overview.md) first for authentication, HTTP/2 (h2c) requirements, and the
ProblemDetails error format shared by all NEF APIs.

---

## The happy path

Create a subscription, read it back, then delete it. `my-af-1` is the AF identifier and must match
the `sub` claim of the presented JWT.

**1. Create.** NEF validates the body, stores it, creates the PCF application session and writes
the influence data to UDR, then answers `201` with the body it stored plus the identifier it
assigned in `afTransId`.

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "afAppId": "video-edge-app",
           "dnn": "internet",
           "snssai": {"sst": 1, "sd": "010203"},
           "supi": "imsi-208950000000001",
           "notificationDestination": "http://af.example.com:9090/notify/traffic",
           "trafficRoutes": [{"dnai": "edge-dc-north", "routeProfId": "low-latency"}],
           "subscribedEvents": ["UP_PATH_CHANGE"],
           "dnaiChgType": "EARLY"
         }' \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions
```

```json
{
  "afAppId": "video-edge-app",
  "dnn": "internet",
  "snssai": {"sst": 1, "sd": "010203"},
  "supi": "imsi-208950000000001",
  "notificationDestination": "http://af.example.com:9090/notify/traffic",
  "trafficRoutes": [{"dnai": "edge-dc-north", "routeProfId": "low-latency"}],
  "subscribedEvents": ["UP_PATH_CHANGE"],
  "dnaiChgType": "EARLY",
  "afTransId": "c3d4e5f6-a7b8-9012-cdef-234567890abc"
}
```

There is no `Location` header and no `self` field on this response. Take the resource identifier
from `afTransId` and build the subsequent URLs yourself.

**2. Read it back.**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

**3. Delete when the application instance goes away.** NEF tears down the PCF application session
and the UDR influence data, then answers `204`.

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

---

## Base path

```
/3gpp-traffic-influence/v1/{afId}/subscriptions
```

`{afId}` is the AF identifier of the calling application. It must match the `sub` claim of the
presented JWT. See the [Overview — AF/SCS-AS ID](overview.md#af--scs-as-id-path-parameter)
section for the validation rules.

Any other resource name under `/{afId}/` — anything that is not the literal segment
`subscriptions` — is answered `404`, not `405`.

---

## Request fields

The body is a TS 29.522 traffic influence resource. NEF parses it into the generated
`TrafficInfluData` model and validates it; unknown members are accepted and echoed back unchanged.

At least one of `afAppId`, `trafficFilters` or `ethTrafficFilters` must be present, on both `POST`
and `PUT`. Without one of them NEF cannot tell PCF which traffic the policy applies to, and answers
`400`.

| Field | Type | Description |
|---|---|---|
| `afAppId` | string | Application identifier that labels the policy in PCF. Maximum 256 characters. |
| `trafficFilters` | array of `FlowInfo` | IP flow descriptors. Each item needs `flowId` (integer) and normally `flowDescriptions` (array of IPFilterRule strings). |
| `ethTrafficFilters` | array | Ethernet flow descriptors, as the Ethernet-PDU-session alternative to `trafficFilters`. |
| `trafficRoutes` | array of `RouteToLocation` | Where the traffic should go. Each item requires `dnai`; `routeProfId` or `routeInfo` may accompany it. |
| `dnn` | string | Data Network Name the policy applies to, for example `"internet"`. Maximum 100 characters. |
| `snssai` | object | S-NSSAI of the target slice: `{"sst": 1, "sd": "010203"}`. `sst` is required; `sd` defaults to `FFFFFF` when omitted. |
| `supi` | string | SUPI of a single target UE, for example `"imsi-208950000000001"`. |
| `interGroupId` | string | External group id (TS 23.003) when the policy targets a group of UEs rather than one SUPI. |
| `notificationDestination` | string (URI) | AF callback URI for traffic influence notifications. Maximum 2048 characters, and it must pass the callback URI checks below. |
| `subscribedEvents` | array | Events the AF wants reported. `UP_PATH_CHANGE` is the only value the generated model accepts. |
| `dnaiChgType` | string | When the UP path change is reported: `EARLY`, `EARLY_LATE` or `LATE`. |
| `upPathChgNotifUri` | string (URI) | Separate URI for user-plane path change notifications, when the AF wants them apart from `notificationDestination`. |
| `upPathChgNotifCorreId` | string | Correlation id echoed on user-plane path change notifications. |
| `appReloInd` | boolean | Whether the application can be relocated once a UE's traffic has started. |
| `tempValidities` | array | Time windows (`startTime`, `stopTime`, RFC 3339) during which the policy should apply. |
| `validStartTime` / `validEndTime` | string | Single validity window, as the alternative to `tempValidities`. |
| `nwAreaInfo` | object | Network area (TAI/cell/NG-RAN node lists) scoping the request. |
| `traffCorreInd` | boolean | Request that traffic of the UEs in the group be correlated. |
| `maxAllowedUpLat` | integer | Maximum acceptable user-plane latency in milliseconds. |
| `supportedFeatures` | string | Hex feature bitmask (TS 29.500 clause 6.6). |

`tempValidities`, `validStartTime` and `validEndTime` are forwarded to PCF as part of the policy.
NEF itself does **not** act on them: see [Lifetime](#lifetime) below.

### Callback URI rules

`notificationDestination` is rejected with `400` unless it uses the `http` or `https` scheme and
names a host that is neither an IP literal in a blocked range nor a malformed authority. Blocked
ranges are IPv4 loopback `127.0.0.0/8`, link-local `169.254.0.0/16`, RFC 1918
(`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`), IPv6 loopback `::1`, link-local `fe80::/10` and
ULA `fc00::/7`. A DNS name is accepted without a lookup, so in a container deployment give the AF's
service name rather than its pod IP.

---

## Endpoints

### POST `/{afId}/subscriptions` — create a subscription

Call this when an AF wants a new steering policy to take effect. This is the only operation that
creates PCF and UDR state.

NEF validates and stores the request, resolves the PCF and UDR endpoints, creates the PCF
application session, then writes the influence data to UDR. If PCF fails, the whole request fails
with `502` and the local record is rolled back — nothing is left half-created. If only the UDR write
fails, it is logged and the AF still receives `201`; the policy is active but not recorded as
subscriber data.

**Response `201 Created`**: the body you sent, plus `afTransId`.

A fuller example, using flow filters instead of an application identifier:

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "afAppId": "video-edge-app",
           "dnn": "internet",
           "snssai": {"sst": 1, "sd": "010203"},
           "interGroupId": "extgroupid-208950000000001",
           "notificationDestination": "http://af.example.com:9090/notify/traffic",
           "trafficFilters": [
             {
               "flowId": 1,
               "flowDescriptions": [
                 "permit out 6 from 203.0.113.10 80 to assigned",
                 "permit in 6 from assigned to 203.0.113.10 80"
               ]
             }
           ],
           "trafficRoutes": [
             {"dnai": "edge-dc-north", "routeProfId": "low-latency"}
           ],
           "appReloInd": true,
           "tempValidities": [
             {"startTime": "2026-04-25T00:00:00Z", "stopTime": "2026-04-26T00:00:00Z"}
           ]
         }' \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions
```

---

### GET `/{afId}/subscriptions` — list your subscriptions

Call this to reconcile after an AF restart, or to find out what is still active. NEF holds
subscriptions in memory only, so this is also how you discover that a NEF restart has cleared them.

Only subscriptions owned by `{afId}` are returned. `200 OK` with an array; `[]` when there are
none.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions
```

```json
[
  {
    "afAppId": "video-edge-app",
    "dnn": "internet",
    "snssai": {"sst": 1, "sd": "010203"},
    "supi": "imsi-208950000000001",
    "trafficRoutes": [{"dnai": "edge-dc-north", "routeProfId": "low-latency"}],
    "afTransId": "c3d4e5f6-a7b8-9012-cdef-234567890abc"
  }
]
```

---

### GET `/{afId}/subscriptions/{afTransId}` — read one subscription

Call this to confirm what NEF currently holds for a policy before changing it. The response is the
stored body with `afTransId` added — the same shape as the `201`.

This read is served from NEF's in-memory state. It does not query PCF, so it tells you what NEF
asked for, not what PCF is currently enforcing.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

---

### PUT `/{afId}/subscriptions/{afTransId}` — replace a subscription

Call this when several parts of the policy change at once, or when the AF keeps the authoritative
copy of the policy and wants to push it wholesale.

The stored record is replaced by the body you send. Anything you omit is gone — including
`notificationDestination`, so re-send it unless you intend to stop receiving notifications. The
same "at least one of `afAppId`, `trafficFilters`, `ethTrafficFilters`" rule applies. NEF sends the
new body to PCF as a policy authorization update and only then overwrites its own copy, so a PCF
failure leaves the previous policy in place.

**Response `200 OK`**: the body as sent. Unlike `POST` and `PATCH`, the `PUT` response does not
carry `afTransId` — you already know it, it is in the URL.

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "afAppId": "video-edge-app",
           "dnn": "internet",
           "snssai": {"sst": 1, "sd": "010203"},
           "supi": "imsi-208950000000001",
           "notificationDestination": "http://af.example.com:9090/notify/traffic",
           "trafficRoutes": [{"dnai": "edge-dc-south", "routeProfId": "ultra-low-latency"}]
         }' \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

---

### PATCH `/{afId}/subscriptions/{afTransId}` — change part of a subscription

Call this for the common single-field edit: move the traffic to a different DNAI, point the
notifications somewhere else, extend a validity window. It avoids the risk of dropping a field by
omission that `PUT` carries.

Semantics are JSON Merge Patch (RFC 7396): members present in the body replace the stored ones,
members set to `null` are removed, everything else is untouched. NEF merges the patch into its copy
first and sends the **merged result** to PCF, so PCF always sees a complete policy.

Send `Content-Type: application/merge-patch+json`.

**Response `200 OK`**: the full merged resource, with `afTransId`.

```bash
curl --http2-prior-knowledge \
     -X PATCH \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/merge-patch+json" \
     -d '{
           "trafficRoutes": [{"dnai": "edge-dc-south", "routeProfId": "ultra-low-latency"}],
           "notificationDestination": "http://af.example.com:9090/notify/traffic-v2"
         }' \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

---

### DELETE `/{afId}/subscriptions/{afTransId}` — delete a subscription

Call this when the application instance behind the policy is being torn down. Traffic reverts to
the default path and notifications stop.

The southbound teardown is best-effort: NEF deletes the PCF application session and the UDR
influence data, logs either failure, and removes its own state and answers `204` regardless. A
`204` therefore means "NEF no longer holds this policy", not "PCF has confirmed the removal". The
operation is idempotent — repeat it safely; a second call answers `404`.

Note that the error responses on this endpoint have **empty bodies**: a `403` or `404` from
`DELETE` carries no ProblemDetails. Branch on the status code, not on the body.

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

---

## Notifications

If `notificationDestination` is set, NEF forwards traffic influence events to it.

The return path is: PCF POSTs an `Npcf_PolicyAuthorization` notification to NEF's inbound sink at
`/nef-notify/v1/notify/{appSessionId}`; NEF maps that `appSessionId` back to the `afTransId`,
rewrites the payload into the TS 29.522 §8.3.2 notification shape, and POSTs it to
`notificationDestination`.

NEF forwards the AF's request body to PCF unchanged and does not insert a notification URI of its
own into the application session, so the PCF deployment has to be configured to call back on that
path for notifications to arrive at all.

A DNAI change report becomes:

```json
{
  "subscription": "c3d4e5f6-a7b8-9012-cdef-234567890abc",
  "trafficInfluenceNotifs": [
    {
      "dnaiChgType": "EARLY",
      "sourceDnai": "edge-dc-north",
      "targetDnai": "edge-dc-south",
      "timeStamp": "2026-04-25T12:00:00Z"
    }
  ]
}
```

A PCF usage report becomes an item carrying `usageReport`; any other PCF event is passed through as
`{"event": "<PCF event name>"}`. Notification delivery is fire-and-forget: NEF queues the POST and
does not retry it, and drops it with an error log if the notification queue (1000 entries) is full.

---

## Lifetime

Traffic influence subscriptions live in NEF's process memory and persist until one of the
following:

- the AF deletes them;
- the NEF process restarts, which loses every subscription (there is no persistence layer for TI
  state, so an AF must re-create its policies after a NEF restart).

NEF does **not** expire TI subscriptions on `tempValidities`, `validStartTime` or `validEndTime`.
Those fields are forwarded to PCF, which enforces the window; NEF's expiry timer applies only to
monitoring subscriptions that carry a `monitorExpireTime`. A TI subscription whose validity window
has passed therefore still answers `GET` and still needs an explicit `DELETE`.

---

## Southbound behaviour

| AF operation | NEF → PCF (`Npcf_PolicyAuthorization`) | NEF → UDR | On southbound failure |
|---|---|---|---|
| `POST` | create application session | `PUT` influence data | PCF failure: `502` and full rollback. UDR failure: logged, AF still gets `201`. |
| `PUT` | update application session | — | `502` (or `504` on timeout); the stored policy is left unchanged. |
| `PATCH` | update application session with the merged body | — | `502` (or `504` on timeout); the stored policy is left unchanged. |
| `DELETE` | delete application session | `DELETE` influence data | Both logged only; the AF still gets `204`. |

NEF keeps the mapping between the T8 `afTransId` and the PCF `appSessionId` internally, and uses it
to route PCF notifications back to the right AF. AFs never address PCF directly.

Traffic Influence is the FATAL-502 family: any PCF error on `POST`, `PUT` or `PATCH` fails the
northbound request. This differs from QoS Monitoring, which answers `500` for the equivalent PCF
failure. If you handle both services in one client, do not share the error branch.

---

## Error responses

| Status | Title | What happened | What to do |
|---|---|---|---|
| `400` | Bad Request | Body is not valid JSON; a member has the wrong JSON type for the model; none of `afAppId` / `trafficFilters` / `ethTrafficFilters` is present; `notificationDestination` failed the callback URI checks. | Fix the body. `detail` names the offending member. Do not retry unchanged. |
| `403` | Forbidden | Authorization failed: no or invalid JWT, `{afId}` not matching the `sub` claim, AF absent from the whitelist, or the AF's `allowed_apis` not listing `nnef-trafficinfluence`. On the resource endpoints it can also mean the subscription belongs to a different AF. | Check the token and the `{afId}` in the path. NEF never answers `401`, so treat `403` as the single authentication-and-authorization outcome. On `DELETE` the body is empty. |
| `404` | Not Found | The `afTransId` does not exist, or the path segment after `{afId}` is not `subscriptions`. | Re-list with `GET /{afId}/subscriptions` and re-create if the subscription is gone. `404` after a NEF restart is expected: re-create. |
| `405` | Method Not Allowed | The path is a valid `subscriptions` resource but the method is not supported there — for example `POST` on `/{afTransId}`, or `PATCH` on the collection. | Correct the method or the URL. Collection accepts `GET` and `POST`; the resource accepts `GET`, `PUT`, `PATCH`, `DELETE`. |
| `422` | Unprocessable Entity | The body parsed and matched the model's types but failed the generated `validate()`, or a length limit was exceeded: `afId` or `afAppId` over 256 characters, `dnn` over 100, `notificationDestination` over 2048. | Shorten or correct the named field. Retrying the same body will fail again. |
| `429` | Too Many Requests | The token-bucket rate limit for this caller was reached. The bucket is keyed on the bearer token, or on the peer address when no token is presented. | Back off exponentially and retry. Spread bulk provisioning over time rather than bursting. |
| `502` | Bad Gateway | PCF could not be discovered or rejected the application session create/update, or the stored PCF policy identifier is missing for this subscription. A UDR failure never produces `502`. | The policy was not applied. Retry after a short delay; if it persists, check PCF reachability and NRF registration. On `POST` nothing was created, so a retry is safe. |
| `503` | Service Unavailable | Either NEF is draining for shutdown (`detail` is `"Server is draining"`), or the request dispatcher queue is full (`"Server is overloaded, please retry later"`). | Retry elsewhere or later. Draining means this NEF instance is going away — send the retry to another instance if you have one. |
| `504` | Gateway Timeout | The PCF call timed out (a southbound `408` maps to `504`). | The outcome at PCF is unknown. Read the subscription back with `GET` before retrying a `PUT` or `PATCH`. |

A request body larger than 1 MiB is not an error response at all: the HTTP/2 layer resets the
stream while the body is still arriving, so the client sees `RST_STREAM` with no status code and no
ProblemDetails. See the [overview](overview.md) for the transport-level limits.

**`502` on a PCF failure**

```json
{
  "type": "about:blank",
  "title": "Bad Gateway",
  "status": 502,
  "detail": "Failed to create policy authorization in PCF"
}
```

**`504` on a PCF timeout.** The title in this release is still `"Bad Gateway"` — match on `status`,
not on `title`:

```json
{
  "type": "about:blank",
  "title": "Bad Gateway",
  "status": 504,
  "detail": "Failed to update policy authorization in PCF"
}
```

**`403` on an authorization failure**

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF not authorized for this service"
}
```

---

## Related pages

- [API Reference Overview](overview.md) — authentication, HTTP/2, shared error format
- [QoS Monitoring API](qos-monitoring.md) — per-flow guaranteed QoS, the other PCF-backed service
- [BDT Policy API](bdt-policy.md) — negotiating background transfer windows with PCF
- [Architecture — the request path](../ARCHITECTURE.md#2-the-request-path) — how a request travels
  through NEF. A per-service end-to-end sequence diagram has not been written.
