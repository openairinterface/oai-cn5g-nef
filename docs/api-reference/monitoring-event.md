<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Monitoring Event API (TS 29.122)

## What this API is for

An AF / SCS-AS uses the Monitoring Event API to be told about UE-level events —
a UE becoming unreachable, moving to a new cell, changing roaming status — without
holding an interface to the AMF and without polling.

The AF creates one subscription per event type it cares about and hands NEF a callback
URI. NEF proxies the subscription southbound as a `Namf_EventExposure` subscription
(TS 29.518), keeps the T8-to-AMF identifier mapping, and translates each AMF event report
into a T8 `MonitoringEventNotification` before posting it to the AF's callback.

The AF never talks to AMF. The T8 resource is the only thing it sees.

Read the [API Reference Overview](overview.md) first for authentication, HTTP/2
requirements, and the error format shared by all NEF APIs.

---

## The happy path

Four steps: subscribe, read back, receive notifications, unsubscribe.

**1. Subscribe.** `monitoringType` and `notificationDestination` are the two fields
NEF insists on.

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "monitoringType": "LOSS_OF_CONNECTIVITY",
           "notificationDestination": "http://my-af.example.com:9090/notify/monitoring",
           "externalId": "user-1@example.com",
           "monitorExpireTime": "2026-05-01T00:00:00Z"
         }' \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions
```

NEF answers `201 Created`. The body is the request body echoed back with one field
added — `subId`, the subscription identifier you use in every later request:

```json
{
  "monitoringType": "LOSS_OF_CONNECTIVITY",
  "notificationDestination": "http://my-af.example.com:9090/notify/monitoring",
  "externalId": "user-1@example.com",
  "monitorExpireTime": "2026-05-01T00:00:00Z",
  "subId": "3"
}
```

**2. Read it back.**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/3
```

**3. Receive a notification.** When AMF reports an event, NEF posts a
`MonitoringEventNotification` to `notificationDestination` (see
[Notification payload](#notification-payload) — and the correlation limitation
described there, which affects this build).

**4. Unsubscribe.**

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/3
```

`204 No Content`, with a genuinely empty body.

---

## Base path

```
/3gpp-monitoring-event/v1/{scsAsId}/subscriptions[/{subId}]
```

`{scsAsId}` is the SCS/AS identifier of the calling AF. It is matched against the `sub`
claim of the presented JWT, or against the configured AF whitelist entry. See
[Overview — AF/SCS-AS ID](overview.md#af--scs-as-id-path-parameter).

`{subId}` is the value NEF returned as `subId` at creation time. It is a lowercase
hexadecimal rendering of a per-process counter (`1`, `2`, … `a`, `b`, …), not a UUID, and
it is **not** stable across a NEF restart — all subscription state is in memory.

---

## Endpoints

| Method | Path | What it does |
|---|---|---|
| `POST` | `/{scsAsId}/subscriptions` | Create a subscription and register it with AMF |
| `GET` | `/{scsAsId}/subscriptions` | List this SCS/AS's subscriptions |
| `GET` | `/{scsAsId}/subscriptions/{subId}` | Read one subscription |
| `PUT` | `/{scsAsId}/subscriptions/{subId}` | Replace the stored subscription (local only) |
| `DELETE` | `/{scsAsId}/subscriptions/{subId}` | Delete the subscription and unsubscribe from AMF |

Anything else on this base path — `PATCH`, a `PUT` or `DELETE` without a `{subId}` —
is answered `405 Method Not Allowed` with the detail
`"HTTP method is not supported for this resource"`.

---

## Monitoring types

The value goes in the `monitoringType` field of the request body. NEF validates it against
a fixed set; anything outside the set is rejected with `422 Unprocessable Entity`, not 400.

| `monitoringType` | Meaning |
|---|---|
| `LOSS_OF_CONNECTIVITY` | The UE is no longer reachable by the network. |
| `UE_REACHABILITY` | The UE has become reachable (for SMS or for data — the AMF report carries which). |
| `LOCATION_REPORTING` | The UE's location changed (cell ID, TAI, or geographic area). |
| `CHANGE_OF_IMSI_IMEI_ASSOCIATION` | The IMSI–IMEI pairing changed (SIM swap or device swap). |
| `ROAMING_STATUS` | The UE entered or left a roaming network. |
| `COMMUNICATION_FAILURE` | A communication failure was reported for the UE. |
| `AVAILABILITY_AFTER_DDN_FAILURE` | The UE is reachable again after a Downlink Data Notification failure. |
| `NUMBER_OF_UES_IN_AN_AREA` | Count of UEs present in a given area. |
| `PDN_CONNECTIVITY_STATUS` | PDU session established or released. |
| `DOWNLINK_DATA_DELIVERY_STATUS` | Downlink data delivery status changed. |
| `API_SUPPORT_CAPABILITY` | Which monitoring capabilities the network supports. |
| `NUM_OF_REGD_UES` | Number of registered UEs. |
| `NUM_OF_ESTD_PDU_SESSIONS` | Number of established PDU sessions. |
| `AREA_OF_INTEREST` | UE presence in an area of interest. |

Only `monitoringType` itself is enum-checked. NEF does not enforce which further fields a
given monitoring type requires, so a body can be accepted here and still be rejected by AMF —
which surfaces as a `502`, see [Error responses](#error-responses).

---

## POST — create a subscription

Call this when the AF wants to start receiving reports for an event type. NEF stores the
subscription, then creates the matching `Namf_EventExposure` subscription on AMF. If AMF
refuses, the local record is rolled back and the request fails, so a `201` means both
sides exist.

**Request body**

| Field | Type | Required | Description |
|---|---|---|---|
| `monitoringType` | string (enum) | Required | One of the values in the table above. Missing → `400`; present but unknown → `422`. |
| `notificationDestination` | string (URI) | Required | Where NEF POSTs event notifications. Must be reachable from the NEF container, must pass the SSRF callback check, and must be at most 2048 characters. |
| `monitorExpireTime` | string (date-time) | Optional | Absolute expiry. An unparseable value is rejected with `400` on `POST`. |
| any other T8 field | — | Optional | `externalId`, `msisdn`, `externalGroupId`, `maximumNumberOfReports`, `maximumDetectionTime` and the rest are stored verbatim and forwarded to AMF unchanged. NEF neither validates nor acts on them. |

The whole body is passed through to AMF with one field added by NEF
(`eventNotifyUri`), so anything the AMF needs and NEF does not know about can be
included.

**Response — 201 Created**

Content type `application/json`. The body is the request body plus `subId`. There is **no**
`self` field and **no** `Location` header on this API; the QoS Monitoring and
Nnef_PFDmanagement subscription resources have those, this one does not.

**Rollback on AMF failure.** If the AMF call fails, or returns 2xx with no usable
`subscriptionId`, NEF deletes the local subscription and releases the AF profile slot
before answering. There is no orphaned T8 resource to clean up.

---

## GET — list subscriptions

Call this to enumerate what this SCS/AS currently has registered — for instance after a
restart of the AF, to rediscover subscription ids.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions
```

**Response — 200 OK.** An array of stored subscription bodies, each with its `subId`
added. Subscriptions belonging to other SCS/AS identities are filtered out. An empty
result is `[]`.

```json
[
  {
    "monitoringType": "LOSS_OF_CONNECTIVITY",
    "notificationDestination": "http://my-af.example.com:9090/notify/monitoring",
    "externalId": "user-1@example.com",
    "monitorExpireTime": "2026-05-01T00:00:00Z",
    "subId": "3"
  },
  {
    "monitoringType": "UE_REACHABILITY",
    "notificationDestination": "http://my-af.example.com:9090/notify/reachability",
    "externalId": "user-2@example.com",
    "subId": "4"
  }
]
```

The listing is served entirely from NEF's in-memory state; AMF is not consulted.

---

## GET — read one subscription

Call this to confirm what NEF currently holds for a subscription, for example after a `PUT`.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/3
```

**Response — 200 OK.** The stored subscription body.

Two quirks of the item read that the list does not share:

- The item response does **not** carry `subId`. Only the list adds it. Keep the id you
  got from the `201`.
- A subscription that does not exist is answered `404` with the JSON body `null` and a
  content type of `application/json` — not a Problem Details document. Treat any `404`
  on this path as "gone", whatever the body says.

An `{subId}` that exists but belongs to another SCS/AS is `403` with a Problem Details
body, also sent as `application/json`.

---

## PUT — replace a subscription

Call this to change a stored subscription in place — most usefully to point
`notificationDestination` at a new callback, or to extend `monitorExpireTime`.

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "monitoringType": "LOSS_OF_CONNECTIVITY",
           "notificationDestination": "http://my-af.example.com:9090/notify/monitoring-v2",
           "externalId": "user-1@example.com",
           "monitorExpireTime": "2026-06-01T00:00:00Z"
         }' \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/3
```

**Response — 200 OK.** The replaced body with `subId` added.

This is a true replacement: the supplied body becomes the stored subscription data in
full, so send every field you want to keep. Two things to know before using it:

- **`notificationDestination` is the only field validated.** It must be present and a
  string (otherwise `400`) and must pass the SSRF callback check (otherwise `400`).
  `monitoringType` is not re-checked against the enum here, unlike on `POST`.
- **The change is local only.** NEF does not send an `Namf_EventExposure` update to AMF
  on `PUT`. AMF keeps reporting under the subscription created by the original `POST`.
  A new callback URI takes effect because NEF resolves it at delivery time, but a changed
  `monitoringType` does not reach the network. To change what the network reports on,
  `DELETE` the subscription and `POST` a new one.

A `monitorExpireTime` that fails to parse is silently ignored on `PUT`, where `POST`
would have answered `400`.

---

## DELETE — remove a subscription

Call this when the AF no longer wants the reports. NEF unsubscribes from AMF and removes
the local record.

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/3
```

**Response — 204 No Content.**

The AMF unsubscribe is best-effort: local cleanup happens and `204` is returned whether or
not AMF accepted the `DELETE`. A subscription created without a usable AMF subscription id
skips the southbound call entirely.

**Body exception on this method.** DELETE responses go through an empty-body sink, so every
status on this path is a bare status line with no body and no content type — `204` on
success, but also `403` on an authorization or ownership failure and `404` on a missing
subscription. Do not parse a body from a failed DELETE; there is none. The one exception is
a `503`, which is produced before that sink is reached and does carry Problem Details.

---

## Notification payload

When AMF reports an event, NEF maps the `Namf_EventExposure` notification to a T8
`MonitoringEventNotification` and POSTs it to the AF's `notificationDestination`:

```json
{
  "subscription": "3",
  "monitoringEventReports": [
    {
      "monitoringType": "LOCATION_REPORTING",
      "locationInfo": { "cellId": "0x0000101" },
      "supi": "imsi-208950000000001",
      "timeStamp": "2026-04-25T14:22:00Z"
    }
  ]
}
```

| Field | Description |
|---|---|
| `subscription` | The NEF subscription id (`subId`) this report belongs to. It is the bare identifier, not a URI. |
| `monitoringEventReports` | One entry per report in the AMF `reportList`. |
| `…[*].monitoringType` | The AMF report type translated to the T8 name: `LOCATION_REPORT` → `LOCATION_REPORTING`, `UE_REACHABILITY_FOR_SMS` and `UE_REACHABILITY_FOR_DATA` → `UE_REACHABILITY` (with `reachabilityForSms` / `reachabilityForData` set to `true`), `PDU_SESSION_STATUS` → `PDU_SESSION_STATUS`. Any other AMF type is forwarded under its AMF name. |
| `…[*].locationInfo` | The AMF `state` object, for `LOCATION_REPORT` only. For an unrecognised AMF type the same object is carried as `stateInfo` instead. |
| `…[*].supi`, `gpsi`, `timeStamp` | Copied from the AMF report when present. |

The AF callback should answer `2xx`. NEF attempts delivery up to three times with backoff
behind a circuit breaker; after that the notification is dropped. If the internal
notification queue is full, delivery is dropped as well.

If the AMF notification has no `reportList` array, mapping fails and NEF forwards the raw
AMF payload unchanged, so an AF callback must tolerate a body that is not in the shape above.

**Current implementation limitation — inbound correlation.** NEF advertises a single fixed
callback to AMF, `<nef-url>/nef-notify/v1/notify/amf`, for every monitoring subscription,
while the inbound correlation map is keyed by the AMF-assigned subscription id. An AMF that
posts to the advertised URI unchanged therefore arrives with the correlation key `amf`,
which matches no entry, and NEF answers that notification `404 Not Found` with the detail
`"No subscription found for notification id: amf"` instead of forwarding it. Notification
delivery on this API is not exercised end to end by this build.

---

## Subscription expiry

If `monitorExpireTime` is set, a periodic tick inside NEF compares it against the current
clock. When it passes, NEF unsubscribes from AMF using the stored AMF subscription id,
drops the correlation entry, and removes the local record. Notifications stop, and any
later `GET`, `PUT` or `DELETE` on that `subId` is a `404`.

**AF guidance:** treat a `404` on an id that previously worked as "expired or lost — create
a new subscription". Do not treat it as a permanent error. Subscription state is in memory
only, so a NEF restart produces exactly the same `404` for every id.

`maximumNumberOfReports` is stored and forwarded to AMF, but NEF itself does not count
reports and does not delete a subscription when a report budget is reached. Do not rely on
it to bound a subscription's lifetime — set `monitorExpireTime`, or delete explicitly.

---

## Southbound behaviour

When an AF creates a subscription, NEF:

1. Stores the T8 subscription record in memory and associates it with the SCS/AS profile.
2. Discovers the AMF via NRF, or uses the statically configured address.
3. `POST`s to `{amf}/namf-evts/v1/subscriptions`, sending the AF's body verbatim plus
   `eventNotifyUri`.
4. Reads the AMF subscription id out of the response (`subscriptionId`, at the top level or
   under `eventsSubscription`) and stores the AMF-id → T8-id mapping.

If step 2, 3 or 4 fails, step 1 is undone and the AF gets a `502` (or `504` on a southbound
timeout).

`DELETE` reverses step 4 and step 1, and issues a best-effort `DELETE` to
`{amf}/namf-evts/v1/subscriptions/{amfSubId}`. `PUT` touches none of this.

---

## Error responses

| Status | What happened | What the client should do |
|---|---|---|
| `400 Bad Request` | The body is not JSON; `monitoringType` or `notificationDestination` is absent; `notificationDestination` is not a string or fails the SSRF callback check; `monitorExpireTime` (on `POST`) cannot be parsed. | Fix the body. Do not retry unchanged. A body over 1 MiB is not a `400` — the HTTP/2 layer resets the stream instead; see the [overview](overview.md). |
| `403 Forbidden` | Missing, invalid or expired JWT; `scsAsId` does not match the token's `sub`; the AF is not whitelisted; `allowed_apis` does not include `nnef-eventexposure`; or the `{subId}` belongs to another SCS/AS. | Check the token and the AF's `allowed_apis` configuration. NEF never emits `401` — every authorization failure is this `403`, with the detail `"AF not authorized for this service"`. Retrying will not help until configuration or token changes. |
| `404 Not Found` | The `{subId}` does not exist, has expired, or was lost in a restart. | Create a new subscription. Do not retry the same id. |
| `405 Method Not Allowed` | The method is not supported on that path — a `PATCH`, or a `PUT`/`DELETE` with no `{subId}`. | Use one of the methods in the endpoint table. |
| `422 Unprocessable Entity` | `monitoringType` is present but not in the enum; `scsAsId` is empty or over 256 characters; `notificationDestination` is empty or over 2048 characters. | Correct the offending value. The `detail` names the field. Do not retry unchanged. |
| `429 Too Many Requests` | The token-bucket rate limiter rejected the request before any handling. | Back off exponentially and retry. There is no `Retry-After` header. |
| `502 Bad Gateway` | AMF rejected the subscription, was unreachable, or returned 2xx with no usable `subscriptionId`. No local state was kept. | Safe to retry — nothing was created. If it persists, the body AMF rejected is the first place to look: NEF does not validate the non-`monitoringType` fields it forwards. |
| `503 Service Unavailable` | NEF is draining for shutdown (`"Server is draining"`), or the dispatcher queue is full or stopped (`"Server is overloaded, please retry later"`). | Retry after a short delay, or against another NEF instance. Nothing was created. |
| `504 Gateway Timeout` | The AMF call timed out. The local state was rolled back exactly as for `502`. | Retry. Note that the Problem Details `title` on this response reads `"Bad Gateway"` even though the status is `504`; trust the status, not the title. |

**Example — 400, missing required fields**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "monitoringType and notificationDestination are required"
}
```

**Example — 422, unknown monitoring type**

```json
{
  "type": "about:blank",
  "title": "Unprocessable Entity",
  "status": 422,
  "detail": "monitoringType: invalid value 'loss_of_connectivity'"
}
```

**Example — 403**

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF not authorized for this service"
}
```

**Example — 502, AMF refused the subscription**

```json
{
  "type": "about:blank",
  "title": "Bad Gateway",
  "status": 502,
  "detail": "Failed to create AMF monitoring subscription"
}
```

Error bodies on this API are sent with the content type `application/json` on the
`GET` and `PUT` paths (those share a sink with the success responses) and with
`application/problem+json` on `POST`. `DELETE` sends no body at all. Parse by status code
first and by content type second.

---

## Related pages

- [API Overview](overview.md) — authentication, error format, HTTP/2 requirements
- [Architecture](../ARCHITECTURE.md#2-the-request-path) — how a request travels through
  dispatch, the southbound call, and the deferred response
- [QoS Monitoring API](qos-monitoring.md) — per-flow QoS subscriptions and user-plane events
- [Nnef_PFDmanagement API](nnef-pfd-management.md) — the SBI-side PFD service
