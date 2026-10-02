<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Nnef_EventExposure SBI API (TS 29.591)

## What this service is for

`Nnef_EventExposure` is the service-based interface a 5GC NF — in practice the NWDAF — uses to
subscribe to NEF for events and data that originate outside the 3GPP network: data an AF has
exposed, or observations NEF has collected on the AF's behalf.

It is the inward-facing counterpart of the T8 monitoring interface. Where
[Monitoring Event](monitoring-event.md) lets an external AF ask the network about a UE, this
interface lets an internal NF ask NEF about the application side, so that NWDAF can fold
application-level input into its analytics.

> **This is an SBI, not a T8 API.** It is consumed by network functions inside the operator's
> core, addressed by their NF instance identity in the bearer token rather than by a path
> parameter. An external Application Function must use the
> [Monitoring Event API](monitoring-event.md) instead.

| Dimension | Nnef_EventExposure (this page) | Monitoring Event (T8) |
|---|---|---|
| Caller | Internal 5GC NF, principally NWDAF | External AF / SCS-AS |
| Spec | TS 29.591 | TS 29.122 |
| Base path | `/nnef-eventexposure/v1/` | `/3gpp-monitoring-event/v1/` |
| Caller identity | The `sub` claim of the bearer JWT | The `{scsAsId}` path parameter, checked against `sub` |

> **Implementation status**: this release serves subscription CRUD and expires subscriptions on
> their monitoring duration. It does **not** deliver Nnef_EventExposure notifications — see
> [Notifications](#notifications) before you design a consumer around it.

Read the [API Overview](overview.md) first for authentication, HTTP/2 (h2c) requirements and the
shared ProblemDetails error format.

---

## The happy path

Subscribe, read the subscription back, then unsubscribe.

**1. Subscribe.** `eventsSubs`, `notifUri` and `notifId` are all mandatory.

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/nnef-eventexposure/v1/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-nf-jwt-token>" \
  -d '{
    "eventsSubs": [
      {"event": "UE_MOBILITY"},
      {"event": "UE_COMM"}
    ],
    "notifUri": "http://oai-nwdaf:8080/nnwdaf-datamanagement/v1/notify/nef-events",
    "notifId": "nwdaf-notif-0001",
    "eventsRepInfo": {
      "notifMethod": "PERIODIC",
      "repPeriod": 60,
      "maxReportNbr": 500,
      "monDur": "2026-05-01T00:00:00Z"
    }
  }'
```

**Response `201 Created`**, with a `Location` header equal to the `self` field:

```json
{
  "eventsSubs": [
    {"event": "UE_MOBILITY"},
    {"event": "UE_COMM"}
  ],
  "notifUri": "http://oai-nwdaf:8080/nnwdaf-datamanagement/v1/notify/nef-events",
  "notifId": "nwdaf-notif-0001",
  "eventsRepInfo": {
    "notifMethod": "PERIODIC",
    "repPeriod": 60,
    "maxReportNbr": 500,
    "monDur": "2026-05-01T00:00:00Z"
  },
  "subscriptionId": "b7e21c40-9f3a-4c85-8d2e-1a6f05c93b77",
  "self": "/nnef-eventexposure/v1/subscriptions/b7e21c40-9f3a-4c85-8d2e-1a6f05c93b77"
}
```

**2. Read it back.**

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-nf-jwt-token>" \
  http://oai-nef:8080/nnef-eventexposure/v1/subscriptions/b7e21c40-9f3a-4c85-8d2e-1a6f05c93b77
```

**3. Unsubscribe.**

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-nf-jwt-token>" \
  http://oai-nef:8080/nnef-eventexposure/v1/subscriptions/b7e21c40-9f3a-4c85-8d2e-1a6f05c93b77
```

---

## Base path

```
/nnef-eventexposure/v1/subscriptions[/{subscriptionId}]
```

There is no `{scsAsId}` or `{afId}` in the path. The consuming NF is identified by the `sub` claim
of its bearer token, which NEF then checks against the same whitelist and `allowed_apis` machinery
the T8 APIs use. The service name to authorize is `nnef-eventexposure`.

Any resource name other than the literal segment `subscriptions` is answered `404`, not `405`.

---

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/nnef-eventexposure/v1/subscriptions` | Create a subscription |
| `GET` | `/nnef-eventexposure/v1/subscriptions/{subscriptionId}` | Read one subscription |
| `PUT` | `/nnef-eventexposure/v1/subscriptions/{subscriptionId}` | Replace a subscription |
| `DELETE` | `/nnef-eventexposure/v1/subscriptions/{subscriptionId}` | Delete a subscription |

**There is no collection `GET`.** A `GET` on `/nnef-eventexposure/v1/subscriptions` with no
`{subscriptionId}` is answered `405 Method Not Allowed`, not an empty list. A consuming NF has to
remember the `subscriptionId` values it was given; NEF offers no way to enumerate them.

### POST — create a subscription

Call this when the consuming NF starts and needs NEF-side events feeding its analytics or data
collection.

NEF parses the body into the generated `NefEventExposureSubsc` model, checks that every
`eventsSubs[].event` resolves to a known event, that `notifUri` and `notifId` are non-empty, and
that `notifUri` passes the callback URI checks. It then assigns a `subscriptionId` and stores the
subscription in memory.

**Response `201 Created`** with the stored subscription plus `subscriptionId` and `self`, and a
`Location` header carrying the same value as `self`.

### GET — read one subscription

Call this to confirm what NEF holds, typically before a `PUT`. Returns the stored subscription with
`subscriptionId` and `self`, or `404` if it has been deleted or has passed its `monDur`.

### PUT — replace a subscription

Call this to change the reported events, redirect `notifUri`, or push `monDur` further out before
the current one lapses.

`PUT` is a full replacement and is validated exactly like `POST`: `eventsSubs`, `notifUri` and
`notifId` must all be present again. There is no `PATCH` on this interface.

```bash
curl --http2-prior-knowledge \
  -X PUT http://oai-nef:8080/nnef-eventexposure/v1/subscriptions/b7e21c40-9f3a-4c85-8d2e-1a6f05c93b77 \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-nf-jwt-token>" \
  -d '{
    "eventsSubs": [
      {"event": "UE_MOBILITY"},
      {"event": "UE_COMM"},
      {"event": "EXCEPTIONS"}
    ],
    "notifUri": "http://oai-nwdaf:8080/nnwdaf-datamanagement/v1/notify/nef-events",
    "notifId": "nwdaf-notif-0001",
    "eventsRepInfo": {
      "notifMethod": "PERIODIC",
      "repPeriod": 120,
      "maxReportNbr": 1000,
      "monDur": "2026-06-01T00:00:00Z"
    }
  }'
```

**Response `200 OK`** with the replacement subscription, `subscriptionId` and `self`.

### DELETE — delete a subscription

Call this when the consuming NF no longer needs the events, and on graceful shutdown so NEF does
not retain dead subscriptions until its own restart.

**Response `204 No Content`**, empty body. It is idempotent: a second call answers `404`.

Every response from this endpoint has an empty body — `403` and `404` included. Branch on the
status code, not on a ProblemDetails payload.

---

## Request fields

| Field | Type | Required | Description |
|---|---|---|---|
| `eventsSubs` | array | **Required**, non-empty | The events to subscribe to. |
| `eventsSubs[].event` | string | **Required** | Event name; see [Event values](#event-values). |
| `eventsSubs[].eventFilter` | object | Optional | Event-specific filter (`EventFilter`, TS 29.591). |
| `notifUri` | string (URI) | **Required** | Callback URI on the consuming NF. Must be non-empty and pass the callback URI checks below. |
| `notifId` | string | **Required** | Notification correlation identifier chosen by the consuming NF. Must be non-empty. |
| `eventsRepInfo` | object | Optional | `ReportingInformation`: how and for how long to report. |
| `eventsRepInfo.notifMethod` | string | Optional | `PERIODIC` or `THRESHOLD`. |
| `eventsRepInfo.repPeriod` | integer | Optional | Reporting period in seconds, for `PERIODIC`. |
| `eventsRepInfo.maxReportNbr` | integer | Optional | Maximum number of reports. |
| `eventsRepInfo.monDur` | string | Optional | Monitoring duration. NEF reads this as an **absolute RFC 3339 timestamp**, not a length of time; see [Lifetime](#lifetime). |
| `eventsRepInfo.immRep` | boolean | Optional | Request an immediate report on subscription. |
| `eventsRepInfo.sampRatio` | integer | Optional | Sampling ratio, 1–100 percent. |
| `eventsRepInfo.grpRepTime` | integer | Optional | Group reporting guard time in seconds. |
| `eventsRepInfo.notifFlag` | string | Optional | Notification flag (`ACTIVATE`, `DEACTIVATE`, `RETRIEVAL`). |
| `eventsRepInfo.partitionCriteria` | array | Optional | Partitioning criteria for the reported data. |
| `dataAccProfId` | string | Optional | Data access profile identifier governing what the consumer may see. |
| `eventNotifs` | array | Optional | Pre-existing event notifications, per TS 29.591. |
| `suppFeat` | string | Optional | Hex feature bitmask (TS 29.500 clause 6.6). |

### Event values

`eventsSubs[].event` is a closed enumeration from the generated TS 29.591 model. The accepted
values are:

`SVC_EXPERIENCE`, `UE_MOBILITY`, `UE_COMM`, `EXCEPTIONS`, `USER_DATA_CONGESTION`, `PERF_DATA`,
`DISPERSION`, `COLLECTIVE_BEHAVIOUR`, `MS_QOE_METRICS`, `MS_CONSUMPTION`,
`MS_NET_ASSIST_INVOCATION`, `MS_DYN_POLICY_INVOCATION`, `MS_ACCESS_ACTIVITY`.

Validate the event name on the client before sending it. A value outside this list is rejected
inside the generated deserializer rather than by a NEF handler, so it does not produce a clean
ProblemDetails response the way a missing field does.

### Callback URI rules

`notifUri` is rejected with `400` unless it uses the `http` or `https` scheme and names a host that
is neither malformed nor an IP literal in a blocked range. Blocked ranges are IPv4 loopback
`127.0.0.0/8`, link-local `169.254.0.0/16`, RFC 1918 (`10.0.0.0/8`, `172.16.0.0/12`,
`192.168.0.0/16`), IPv6 loopback `::1`, link-local `fe80::/10` and ULA `fc00::/7`.

This applies to NF consumers as well as AFs, so in a container deployment give the consuming NF's
service name rather than its pod IP.

---

## Notifications

This release does not deliver Nnef_EventExposure notifications.

NEF accepts the subscription, validates `notifUri` and `notifId`, stores them and returns them on
`GET`, but nothing in the event or notification path reads the Nnef_EventExposure store. The only
code that consults it is the expiry sweep. A consuming NF will therefore never receive a POST on
its `notifUri` as a result of a subscription made here.

Plan for this: a NWDAF integration built against this NEF has to obtain the corresponding data by
another route, and should not block on a first notification arriving.

---

## Lifetime

Subscriptions live in NEF's process memory. They end in one of three ways.

**Explicit `DELETE`.** The normal case.

**`monDur` elapsing.** If `eventsRepInfo.monDur` is set, a periodic sweep parses it as an absolute
RFC 3339 instant and erases the subscription once that instant is in the past. The removal is
silent: no notification is sent and no southbound call is made. Afterwards `GET`, `PUT` and
`DELETE` on that `subscriptionId` all answer `404`.

Note the interpretation: `monDur` is treated as a *deadline*, not as a duration in seconds. Send
`"2026-05-01T00:00:00Z"`, not `3600`. A value that does not parse as RFC 3339 is ignored and the
subscription never expires.

**NEF restarting.** Every subscription is lost, and with no collection `GET` there is no way to
discover that from the API. A consuming NF should treat a `404` on a subscription it believes it
holds as "NEF restarted" and subscribe again.

To keep a long-running subscription alive, either `PUT` a later `monDur` before the current one
lapses, or omit `monDur` entirely and rely on an explicit `DELETE`.

---

## Error responses

| Status | Title | What happened | What to do |
|---|---|---|---|
| `400` | Bad Request | Body is not valid JSON; `eventsSubs`, `notifUri` or `notifId` is missing; `eventsSubs` is an empty array; `notifUri` or `notifId` is an empty string; `notifUri` failed the callback URI checks; a member has the wrong JSON type for the model. | Fix the body. `detail` names the offending member. Do not retry unchanged. |
| `403` | Forbidden | Authorization failed: no or invalid bearer token, no `sub` claim to extract, the NF identity absent from the whitelist, or its `allowed_apis` not listing `nnef-eventexposure`. `detail` is `"NF not authorized for this Nnef service"`. | Check the NF's token and its whitelist entry. NEF never answers `401`, so `403` is the single authentication-and-authorization outcome. On `DELETE` the body is empty. |
| `404` | Not Found | No subscription with that id — deleted, expired on `monDur`, or lost to a NEF restart. It is also the answer when the path segment after `/v1/` is not `subscriptions`. | Re-subscribe with `POST`. Since there is no collection `GET`, a `404` is the only signal that NEF's state was reset. |
| `405` | Method Not Allowed | The method is not routed on that path. The common case is `GET` on the collection: there is no list operation. `POST` on a specific `{subscriptionId}` is also `405`. | Use `POST` on the collection and `GET`/`PUT`/`DELETE` on a `{subscriptionId}`. |
| `422` | Unprocessable Entity | The body parsed and matched the model's types but failed the generated `validate()`. | Correct the field named in `detail`. Retrying the same body will fail again. |
| `429` | Too Many Requests | The token-bucket rate limit for this caller was reached. The bucket is keyed on the bearer token, or on the peer address when no token is presented. | Back off exponentially. A consuming NF that re-subscribes in a tight loop after a `404` will hit this. |
| `503` | Service Unavailable | NEF is draining for shutdown (`detail` is `"Server is draining"`), or the request dispatcher queue is full (`"Server is overloaded, please retry later"`). | Retry later, or against another NEF instance. Nothing was created. |

This service makes no southbound calls, so it does not produce `502` or `504`.

A body larger than 1 MiB produces no status code at all: the HTTP/2 layer resets the stream while
the body is still arriving, so the client sees `RST_STREAM` rather than a `4xx`.

**`400` on a missing field**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "notifId is required and must be a non-empty string"
}
```

**`400` on an empty `eventsSubs`**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "eventsSubs is required and must be a non-empty array"
}
```

**`403` on an authorization failure**

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "NF not authorized for this Nnef service"
}
```

Note the wording: the Nnef services answer with this NF-specific detail, where the T8 services
answer `"AF not authorized for this service"`. The status is `403` in both cases.

---

## Related pages

- [API Overview](overview.md) — authentication, HTTP/2, shared error format
- [Monitoring Event API](monitoring-event.md) — the T8 equivalent for external AFs
- [Analytics Exposure API](analytics-exposure.md) — the T8 analytics surface, also served entirely
  from NEF-local state
- [Operational Endpoints](operational-endpoints.md) — health check and the inbound NF notification
  receiver
