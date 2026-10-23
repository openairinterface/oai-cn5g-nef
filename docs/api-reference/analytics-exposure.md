<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Analytics Exposure API (TS 29.522)

## What this service is for

Analytics Exposure is the T8 surface through which an AF registers an interest in network
analytics — UE mobility, communication patterns, abnormal behaviour, network performance — and
retrieves what is registered.

In the 3GPP architecture NEF sits between the AF and the NWDAF: the AF subscribes at NEF, NEF
subscribes at NWDAF over `Nnwdaf_AnalyticsSubscription`, and analytics flow back the same way. The
API shape on this page is that T8 surface.

> **What this NEF actually does.** This implementation has **no NWDAF client**. Every operation
> here — the subscription CRUD and the `/fetch` retrieval — is served **entirely from NEF's own
> local state**. NEF does not contact an NWDAF, does not proxy or relay anything, and delivers no
> analytics notifications. `/fetch` answers with the AF's own registered subscriptions, not with
> analytics computed by the network.
>
> [FEATURE_SET](../FEATURE_SET.md) records analytics exposure as partially supported for exactly
> this reason. Treat the endpoints as a working registry of analytics interest, not as a source of
> analytics data.

Read the [API Overview](overview.md) first for authentication, HTTP/2 (h2c) requirements and the
shared ProblemDetails error format.

---

## The happy path

Register an analytics subscription, retrieve what is registered with `/fetch`, then delete it.
`af-1` is the AF identifier and must match the `sub` claim of the JWT.

**1. Subscribe.** `analyEventsSubs`, `notifUri` and `notifId` are all mandatory.

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "analyEventsSubs": [
      {"analyEvent": "UE_MOBILITY"},
      {"analyEvent": "NETWORK_PERFORMANCE"}
    ],
    "notifUri": "http://af.example.com:9090/notify/analytics",
    "notifId": "af-1-analytics-0001"
  }'
```

**Response `201 Created`** — the body you sent, plus the identifier NEF assigned in `subId`:

```json
{
  "analyEventsSubs": [
    {"analyEvent": "UE_MOBILITY"},
    {"analyEvent": "NETWORK_PERFORMANCE"}
  ],
  "notifUri": "http://af.example.com:9090/notify/analytics",
  "notifId": "af-1-analytics-0001",
  "subId": "e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b"
}
```

There is no `Location` header and no `self` field. Take the identifier from `subId`.

**2. Fetch.** `/fetch` returns the AF's registered subscriptions whose events overlap the ones you
ask about.

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/fetch \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{"analyEventsSubs": [{"analyEvent": "UE_MOBILITY"}]}'
```

```json
{
  "analyEventsSubs": [{"analyEvent": "UE_MOBILITY"}],
  "noNetworkSupportInd": false,
  "analyReports": [
    {
      "subId": "e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b",
      "analyEventsSubs": [
        {"analyEvent": "UE_MOBILITY"},
        {"analyEvent": "NETWORK_PERFORMANCE"}
      ],
      "notifUri": "http://af.example.com:9090/notify/analytics"
    }
  ]
}
```

**3. Delete.**

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions/e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b
```

---

## Base path

```
/3gpp-analyticsexposure/v1/{afId}/subscriptions[/{subId}]
/3gpp-analyticsexposure/v1/{afId}/fetch
```

| Parameter | Type | Description |
|---|---|---|
| `afId` | string | AF identifier; must match the `sub` claim in the JWT. Maximum 256 characters. |
| `subId` | string | The identifier NEF returned in `subId` on the `201`. |

> **Path naming**: the base is `/3gpp-analyticsexposure/v1/`, not `/nnef-analytics/v1/`. The
> alternative spelling is not registered. See the
> [API Overview](overview.md#service-identifiers) for the full service identifier table.

The service name to authorize in an AF's `allowed_apis` list is `nnef-analyticsexposure`.

---

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/{afId}/subscriptions` | Register an analytics subscription |
| `GET` | `/{afId}/subscriptions` | List this AF's analytics subscriptions |
| `GET` | `/{afId}/subscriptions/{subId}` | Read one subscription |
| `PUT` | `/{afId}/subscriptions/{subId}` | Replace a subscription |
| `DELETE` | `/{afId}/subscriptions/{subId}` | Delete a subscription |
| `POST` | `/{afId}/fetch` | Retrieve the registered analytics interest matching a set of events |

`PATCH` is not routed on the subscriptions resource and is answered `405`. To change a subscription
in part, read it, merge locally, and `PUT` the result.

### POST `/{afId}/subscriptions` — register a subscription

Call this to record which analytics an AF wants. NEF validates the body, assigns a `subId`, and
stores the subscription in memory under this AF.

No southbound call is made. The `201` means "NEF has recorded this", not "the network has agreed to
produce these analytics".

**Response `201 Created`**: the body you sent, plus `subId`.

### GET `/{afId}/subscriptions` — list subscriptions

Call this to reconcile after an AF restart. Returns every subscription owned by `{afId}`, each with
its `subId` added, or `[]` when there are none.

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions
```

```json
[
  {
    "analyEventsSubs": [
      {"analyEvent": "UE_MOBILITY"},
      {"analyEvent": "NETWORK_PERFORMANCE"}
    ],
    "notifUri": "http://af.example.com:9090/notify/analytics",
    "notifId": "af-1-analytics-0001",
    "subId": "e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b"
  }
]
```

### GET `/{afId}/subscriptions/{subId}` — read one subscription

Returns the stored body exactly as it was submitted. Note the asymmetry with the collection `GET`:
the single-resource read does **not** add `subId` to the object. If your client keys off that
field, take the identifier from the request URL instead.

### PUT `/{afId}/subscriptions/{subId}` — replace a subscription

Call this to change the event list or redirect `notifUri`. It is a full replacement and is
validated exactly like `POST`: `analyEventsSubs`, `notifUri` and `notifId` must all be present
again.

**Response `200 OK`**: the replacement body, plus `subId`.

```bash
curl --http2-prior-knowledge \
  -X PUT http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions/e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "analyEventsSubs": [{"analyEvent": "UE_MOBILITY"}],
    "notifUri": "http://af.example.com:9090/notify/analytics-v2",
    "notifId": "af-1-analytics-0001"
  }'
```

### DELETE `/{afId}/subscriptions/{subId}` — delete a subscription

**Response `204 No Content`**, empty body. Idempotent: a second call answers `404`.

Every response from this endpoint has an empty body, `403` and `404` included. Branch on the status
code, not on a ProblemDetails payload.

### POST `/{afId}/fetch` — retrieve registered analytics interest

Call this to ask "what is registered for these events?". It is a `POST` because the event list
travels in the body.

NEF walks its own analytics subscriptions for this AF and keeps those with at least one
`analyEventsSubs[].analyEvent` in common with the request. Matching is exact string equality on
`analyEvent`; NEF does not interpret the value.

**Request**: an object with `analyEventsSubs`. Omitting it is a `400`.

```json
{"analyEventsSubs": [{"analyEvent": "UE_MOBILITY"}, {"analyEvent": "UE_COMM"}]}
```

**Response `200 OK`**:

| Field | Type | Description |
|---|---|---|
| `analyEventsSubs` | array | The request's event list, echoed back unchanged. |
| `noNetworkSupportInd` | boolean | `true` when nothing matched, `false` when at least one subscription did. |
| `analyReports` | array | Present only when something matched. One entry per matching subscription. |
| `analyReports[].subId` | string | The matching subscription's identifier. |
| `analyReports[].analyEventsSubs` | array | That subscription's full event list, not only the overlapping part. |
| `analyReports[].notifUri` | string | That subscription's callback URI, when it has one. |

When nothing matches, `analyReports` is absent entirely rather than an empty array:

```json
{
  "analyEventsSubs": [{"analyEvent": "QOS_SUSTAINABILITY"}],
  "noNetworkSupportInd": true
}
```

`noNetworkSupportInd: true` here means "NEF holds no subscription of yours for these events". It is
not a statement about NWDAF, which is never consulted.

Only `POST` is routed on `/fetch`, and only at exactly `/{afId}/fetch`. A `GET` on
`/3gpp-analyticsexposure/v1/{afId}/fetch` is answered **`404 Not Found`, not `405`** — the route
only recognises `fetch` for `POST`, and any other method falls through to the not-found branch.

---

## Request fields

NEF stores the subscription body as submitted rather than through a generated model, so members
beyond those below are accepted, stored and echoed back unchanged. Three are mandatory.

| Field | Type | Required | Description |
|---|---|---|---|
| `analyEventsSubs` | array | **Required** | The analytics events of interest. Must be an array of 1 to 1000 elements. |
| `analyEventsSubs[].analyEvent` | string | Used by `/fetch` | The event name. NEF does not validate it — it is compared for equality by `/fetch` and otherwise passed through. TS 29.522 `AnalyticsEvent` names such as `UE_MOBILITY`, `UE_COMM`, `ABNORMAL_BEHAVIOUR`, `NETWORK_PERFORMANCE` or `QOS_SUSTAINABILITY` are the sensible values to use. |
| `notifUri` | string (URI) | **Required** | AF callback URI. Maximum 2048 characters, and it must pass the callback URI checks below. |
| `notifId` | string | **Required** | Notification correlation identifier chosen by the AF. Maximum 256 characters. |

### Callback URI rules

`notifUri` is rejected with `400` unless it uses the `http` or `https` scheme and names a host that
is neither malformed nor an IP literal in a blocked range. Blocked ranges are IPv4 loopback
`127.0.0.0/8`, link-local `169.254.0.0/16`, RFC 1918 (`10.0.0.0/8`, `172.16.0.0/12`,
`192.168.0.0/16`), IPv6 loopback `::1`, link-local `fe80::/10` and ULA `fc00::/7`. A DNS name is
accepted without a lookup, so in a container deployment give the AF's service name rather than its
pod IP.

The URI is validated and stored even though this release sends nothing to it.

---

## Notifications

This release delivers no analytics notifications.

`notifUri` and `notifId` are mandatory, validated and stored, and `PUT` registers `notifUri` as the
subscription's notification URI. But there is no NWDAF client to produce analytics and no inbound
mapping that would route anything to an analytics subscription, so nothing is ever POSTed to the
AF's callback.

Do not build an AF flow that waits for an analytics notification from this NEF. Use `/fetch`, and
understand that it returns registered interest rather than analytics results.

---

## Lifetime

Analytics subscriptions live in NEF's process memory until the AF deletes them. There is no expiry
timer: NEF's subscription expiry sweep acts only on monitoring subscriptions that carry a
`monitorExpireTime`, so an analytics subscription persists indefinitely.

A NEF restart loses every analytics subscription. After a restart the collection `GET` returns `[]`
and `/fetch` answers `noNetworkSupportInd: true` for everything; the AF must register again.

---

## Error responses

| Status | Title | What happened | What to do |
|---|---|---|---|
| `400` | Bad Request | Body is not valid JSON (`detail` is `"Missing or invalid request payload"`); `analyEventsSubs`, `notifUri` or `notifId` is missing on a subscription write; `analyEventsSubs` is missing on `/fetch`; `notifUri` failed the callback URI checks. | Fix the body. The JSON-parse case gives a generic detail, so validate your serializer output before blaming the field list. |
| `403` | Forbidden | Authorization failed: no or invalid JWT, `{afId}` not matching the `sub` claim, AF absent from the whitelist, or its `allowed_apis` not listing `nnef-analyticsexposure`. On a resource endpoint it can also mean the subscription belongs to a different AF. | Check the token and the `{afId}` in the path. NEF never answers `401`, so `403` is the single authentication-and-authorization outcome. On `DELETE` the body is empty. |
| `404` | Not Found | No subscription with that `subId` for this AF; or a method other than `POST` was used on `/fetch`; or the path segment after `{afId}` is neither `subscriptions` nor `fetch`. | Re-list with the collection `GET`. If you expected `405` from `/fetch`, this is the code you get instead — check the method. |
| `405` | Method Not Allowed | The method is not routed on the subscriptions resource — `PATCH` anywhere, `POST` on a `{subId}`, or `PUT`/`DELETE` on the collection. | Read-modify-`PUT` instead of `PATCH`. `POST` belongs on the collection, `PUT` and `DELETE` on a `{subId}`. |
| `422` | Unprocessable Entity | A length or shape limit was exceeded: `{afId}` over 256 characters, `notifUri` over 2048, `notifId` over 256, or `analyEventsSubs` not an array, empty, or longer than 1000 elements. | Correct the field named in `detail`. Retrying the same body will fail again. |
| `429` | Too Many Requests | The token-bucket rate limit for this caller was reached. The bucket is keyed on the bearer token, or on the peer address when no token is presented. | Back off exponentially. `/fetch` polling loops are the usual cause; widen the interval. |
| `503` | Service Unavailable | NEF is draining for shutdown (`detail` is `"Server is draining"`), or the request dispatcher queue is full (`"Server is overloaded, please retry later"`). | Retry later, or against another NEF instance. Nothing was stored. |

This service makes no southbound calls, so it does not produce `502` or `504`.

A body larger than 1 MiB produces no status code at all: the HTTP/2 layer resets the stream while
the body is still arriving, so the client sees `RST_STREAM` rather than a `4xx`.

**`400` on a missing field**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "analyEventsSubs, notifUri, and notifId are required"
}
```

**`400` from `/fetch` without an event list**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Missing analyEventsSubs in request body"
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

- [API Overview](overview.md) — authentication, HTTP/2, shared error format
- [Nnef_EventExposure SBI API](nnef-event-exposure.md) — the SBI a NWDAF would use towards NEF
- [Monitoring Event API](monitoring-event.md) — UE event monitoring for external AFs, which is
  backed by real AMF and SMF event exposure
- [Feature set](../FEATURE_SET.md) — where analytics exposure sits against TS 23.501 clause 6.2.5
