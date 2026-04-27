# Monitoring Event API (TS 29.122)

This page is the complete reference for the NEF Monitoring Event northbound API. Read the
[API Reference Overview](overview.md) first for authentication, HTTP/2 requirements, and error
format conventions that apply to all NEF APIs.

---

## Overview

The Monitoring Event API exposes an **event subscription** interface for external Application
Functions (AFs) and SCS/AS entities. An AF subscribes to one or more UE event types; when the
network detects such an event (for example, a UE losing connectivity), NEF delivers an
asynchronous notification to the AF's registered callback URI.

This API is defined by **3GPP TS 29.122** (T8 reference point) and covers the following
interactions:

- AF → NEF: CRUD operations on event subscriptions (this document).
- NEF → AMF: NEF proxies the subscription southbound as a `Namf_EventExposure` subscription
  (TS 29.518). AFs do not interact with AMF directly.
- AMF → NEF: Event reports are delivered to NEF's internal notification receiver
  (`POST /nef-notify/v1/notify/{nf_sub_id}`), which translates them into T8 notifications and
  forwards them to the AF's `notificationURI`.

---

## Supported Event Types

| `eventType` Value | Description |
|---|---|
| `loss_of_connectivity` | UE lost network connectivity (radio or core detach). |
| `ue_reachability_for_sms` | UE has become reachable for SMS delivery. |
| `ue_reachability_for_data` | UE has become reachable for data (e.g. after idle-mode paging). |
| `location_reporting` | UE location change reported (cell ID, TAI, or geographic area). |
| `change_of_imsi_imei_association` | IMSI-IMEI pairing has changed (SIM swap or device swap). |
| `roaming_status` | UE roaming status changed (entered or left a roaming network). |
| `availability_after_ddn_failure` | UE is reachable again after a Downlink Data Notification failure. |

Pass one of these string values in the `eventType` field of every subscription request body.
Submitting an unknown value returns `400 Bad Request`.

---

## Base Path

```
/3gpp-monitoring-event/v1/{scsAsId}/subscriptions
```

`{scsAsId}` is the SCS/AS identifier of the calling AF. It must match the `sub` claim of the
presented JWT. See the [Overview — AF/SCS-AS ID](overview.md#af--scs-as-id-path-parameter)
section for the validation rules.

---

## Endpoints

### POST `/{scsAsId}/subscriptions` — Create Subscription

Create a new monitoring event subscription. NEF registers a corresponding
`Namf_EventExposure` subscription with AMF after persisting the T8 record.

**Request Body Fields**

| Field | Type | Required | Description |
|---|---|---|---|
| `eventType` | string (enum) | Required | One of the event type values listed in the table above. |
| `notificationURI` | string (URI) | Required | HTTP URI where NEF will POST event notifications. Must be reachable from the NEF container. |
| `monitoringType` | string (enum) | Optional | `SINGLE_UE` (default) or `ALL_UE`. When `ALL_UE`, `supi` must be omitted. |
| `supi` | string | Required when `monitoringType` is `SINGLE_UE` | UE subscriber identifier, e.g. `imsi-208950000000001`. |
| `monitorExpireTime` | string (ISO 8601 datetime) | Optional | Subscription expiry time. If omitted, the subscription persists until explicitly deleted. |
| `maximumNumberOfReports` | integer | Optional | Maximum number of event reports to deliver before auto-deleting the subscription. |

**Response — 201 Created**

| Field | Type | Description |
|---|---|---|
| `self` | string (URI) | Canonical resource URI for this subscription. |
| `subscriptionId` | string (UUID v4) | Subscription identifier. |
| `eventType` | string | Echo of the requested event type. |
| `notificationURI` | string | Echo of the notification URI. |
| `monitoringType` | string | Effective monitoring type. |
| `supi` | string | Echo of the UE SUPI (if supplied). |
| `monitorExpireTime` | string | Echo of the expiry time (if supplied). |
| `maximumNumberOfReports` | integer | Echo of the report limit (if supplied). |

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "eventType": "loss_of_connectivity",
           "notificationURI": "http://my-af.example.com:9090/notify/monitoring",
           "monitoringType": "SINGLE_UE",
           "supi": "imsi-208950000000001",
           "monitorExpireTime": "2026-05-01T00:00:00Z",
           "maximumNumberOfReports": 10
         }' \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions
```

**Response (201 Created)**

```json
{
  "self": "/3gpp-monitoring-event/v1/my-af-1/subscriptions/a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "subscriptionId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "eventType": "loss_of_connectivity",
  "notificationURI": "http://my-af.example.com:9090/notify/monitoring",
  "monitoringType": "SINGLE_UE",
  "supi": "imsi-208950000000001",
  "monitorExpireTime": "2026-05-01T00:00:00Z",
  "maximumNumberOfReports": 10
}
```

---

### GET `/{scsAsId}/subscriptions` — List All Subscriptions

Retrieve all active monitoring event subscriptions owned by the calling AF.

**Response — 200 OK**

An array of subscription objects (same schema as the 201 response above). Returns an empty
array `[]` if no subscriptions exist.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions
```

**Response (200 OK)**

```json
[
  {
    "self": "/3gpp-monitoring-event/v1/my-af-1/subscriptions/a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "subscriptionId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "eventType": "loss_of_connectivity",
    "notificationURI": "http://my-af.example.com:9090/notify/monitoring",
    "monitoringType": "SINGLE_UE",
    "supi": "imsi-208950000000001",
    "monitorExpireTime": "2026-05-01T00:00:00Z",
    "maximumNumberOfReports": 10
  },
  {
    "self": "/3gpp-monitoring-event/v1/my-af-1/subscriptions/b9c8d7e6-f5a4-3210-fedc-ba9876543210",
    "subscriptionId": "b9c8d7e6-f5a4-3210-fedc-ba9876543210",
    "eventType": "ue_reachability_for_data",
    "notificationURI": "http://my-af.example.com:9090/notify/reachability",
    "monitoringType": "SINGLE_UE",
    "supi": "imsi-208950000000002"
  }
]
```

---

### GET `/{scsAsId}/subscriptions/{subscriptionId}` — Get One Subscription

Retrieve a single subscription by its identifier.

**Path Parameters**

| Parameter | Description |
|---|---|
| `scsAsId` | SCS/AS identifier of the calling AF. |
| `subscriptionId` | UUID v4 subscription identifier returned at creation time. |

**Response — 200 OK**: Single subscription object.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/a1b2c3d4-e5f6-7890-abcd-ef1234567890
```

**Response (200 OK)**

```json
{
  "self": "/3gpp-monitoring-event/v1/my-af-1/subscriptions/a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "subscriptionId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "eventType": "loss_of_connectivity",
  "notificationURI": "http://my-af.example.com:9090/notify/monitoring",
  "monitoringType": "SINGLE_UE",
  "supi": "imsi-208950000000001",
  "monitorExpireTime": "2026-05-01T00:00:00Z",
  "maximumNumberOfReports": 10
}
```

---

### PUT `/{scsAsId}/subscriptions/{subscriptionId}` — Update Subscription (Full Replace)

Replace the entire subscription resource. All modifiable fields must be included; omitted fields
revert to their defaults (not to their current values). NEF propagates the change to AMF via
`Namf_EventExposure` subscription update.

**Request Body**: Same schema as `POST`. All fields follow the same required/optional rules.

**Response — 200 OK**: Updated subscription object.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "eventType": "loss_of_connectivity",
           "notificationURI": "http://my-af.example.com:9090/notify/monitoring-v2",
           "monitoringType": "SINGLE_UE",
           "supi": "imsi-208950000000001",
           "monitorExpireTime": "2026-06-01T00:00:00Z",
           "maximumNumberOfReports": 50
         }' \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/a1b2c3d4-e5f6-7890-abcd-ef1234567890
```

**Response (200 OK)**

```json
{
  "self": "/3gpp-monitoring-event/v1/my-af-1/subscriptions/a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "subscriptionId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "eventType": "loss_of_connectivity",
  "notificationURI": "http://my-af.example.com:9090/notify/monitoring-v2",
  "monitoringType": "SINGLE_UE",
  "supi": "imsi-208950000000001",
  "monitorExpireTime": "2026-06-01T00:00:00Z",
  "maximumNumberOfReports": 50
}
```

---

### DELETE `/{scsAsId}/subscriptions/{subscriptionId}` — Delete Subscription

Delete a subscription. NEF immediately unsubscribes from AMF and stops delivering notifications.

**Response — 204 No Content** (empty body).

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-monitoring-event/v1/my-af-1/subscriptions/a1b2c3d4-e5f6-7890-abcd-ef1234567890
```

A successful delete returns HTTP `204` with no response body.

---

## Notification Payload Format

When a monitored event occurs, NEF makes a `POST` request to the AF's `notificationURI` with
the following JSON body:

```json
{
  "subscriptionId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "monitoringEventReports": [
    {
      "monitoringType": "LOSS_OF_CONNECTIVITY",
      "supi": "imsi-208950000000001",
      "timeStamp": "2026-04-25T14:22:00Z"
    }
  ]
}
```

| Field | Type | Description |
|---|---|---|
| `subscriptionId` | string | Identifies which subscription triggered the notification. |
| `monitoringEventReports` | array | One or more event report objects (batching is implementation-defined). |
| `monitoringEventReports[*].monitoringType` | string | Upper-case event type string (e.g., `LOSS_OF_CONNECTIVITY`). |
| `monitoringEventReports[*].supi` | string | The SUPI of the UE involved in the event. |
| `monitoringEventReports[*].timeStamp` | string (ISO 8601) | UTC timestamp of the event as reported by AMF. |

The AF's callback endpoint must return `200 OK` or `204 No Content`. NEF does not retry
notifications for non-2xx responses in this release.

---

## Subscription Expiry Semantics

If `monitorExpireTime` is set on a subscription, NEF's `task_manager` module runs periodic
checks against the current clock. When a subscription's expiry time is reached:

1. NEF unsubscribes from AMF (`Namf_EventExposure` subscription DELETE).
2. The subscription record is deleted from NEF's in-memory store.
3. Event notifications for that subscription stop immediately.
4. Any subsequent `GET`, `PUT`, or `DELETE` request for that `subscriptionId` returns
   `404 Not Found`.

**AF guidance**: Treat a `404` response on a previously valid `subscriptionId` as
"subscription expired — re-subscribe." Do not assume `404` always means an invalid ID.

If `maximumNumberOfReports` is set, NEF auto-deletes the subscription after delivering that
number of reports, regardless of `monitorExpireTime`.

---

## Southbound Behavior

When an AF creates (or updates) a monitoring event subscription, NEF:

1. Persists the T8 subscription record in its in-memory state.
2. Discovers the AMF SBI endpoint via NRF (or uses the statically configured address).
3. Creates a `Namf_EventExposure` subscription (TS 29.518) on AMF, specifying the event type
   and UE identity.
4. Stores the AMF subscription reference alongside the T8 record for lifecycle management.

AMF delivers event reports to `POST /nef-notify/v1/notify/{nf_sub_id}` on the NEF. NEF maps
the internal subscription identifier to the AF's T8 record and forwards the notification to the
AF's `notificationURI`.

AFs do not interact with AMF directly. The NEF-to-AMF mapping is maintained entirely inside NEF.

---

## Error Responses

| Status | Title | Typical Cause |
|---|---|---|
| `400 Bad Request` | Bad Request | Missing required field (`eventType`, `notificationURI`, or `supi` when `SINGLE_UE`); invalid enum value; malformed JSON; body exceeds 1 MiB. |
| `401 Unauthorized` | Unauthorized | Missing or invalid JWT; expired token. |
| `403 Forbidden` | Forbidden | Valid JWT but `scsAsId` does not match the `sub` claim; AF not in whitelist; `allowed_apis` does not include `monitoring_event`. |
| `404 Not Found` | Not Found | `subscriptionId` does not exist or the subscription has expired and been auto-deleted. |
| `429 Too Many Requests` | Too Many Requests | Global token-bucket rate limit reached. Apply exponential back-off. |
| `503 Service Unavailable` | Service Unavailable | Circuit breaker for AMF is in the OPEN state; AMF is unreachable. |

**Example — 400 Bad Request (missing required field)**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Required field 'notificationURI' is missing from the request body"
}
```

**Example — 401 Unauthorized**

```json
{
  "type": "about:blank",
  "title": "Unauthorized",
  "status": 401,
  "detail": "JWT token validation failed: signature mismatch"
}
```

**Example — 403 Forbidden**

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF 'my-af-1' is not authorised to call monitoring_event"
}
```

**Example — 404 Not Found**

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "Subscription 'a1b2c3d4-e5f6-7890-abcd-ef1234567890' not found"
}
```

**Example — 503 Service Unavailable**

```json
{
  "type": "about:blank",
  "title": "Service Unavailable",
  "status": 503,
  "detail": "Circuit breaker OPEN for AMF; downstream NF is unreachable"
}
```

---

See [Call Flows](../call-flows.md) for the complete end-to-end sequence diagram.
