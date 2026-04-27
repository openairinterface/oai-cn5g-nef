# Nnef_EventExposure SBI API (TS 29.591)

Read the [API Overview](overview.md) before this page for authentication, error format, and HTTP/2 requirements that apply to all NEF APIs.

---

> **Audience Note**: This is the **SBI (Service-Based Interface)** for internal 5G Core Network Functions. It is consumed by AMF, SMF, and PCF to subscribe to NEF-aggregated event notifications. External Application Functions must use the [Monitoring Event API](monitoring-event.md) instead.

---

## Overview

`Nnef_EventExposure` is the SBI defined in **3GPP TS 29.591**. Internal NFs (AMF, SMF, PCF) use this interface to subscribe to NEF for event aggregation and correlation across multiple AFs or NFs. NEF acts as a broker: it collects events from multiple sources and delivers aggregated notifications to the subscribing NF's callback URI.

This interface differs from the T8 Monitoring Event API in three key ways:

| Dimension | Nnef_EventExposure (this page) | Monitoring Event T8 |
|---|---|---|
| Caller | Internal 5GC NF (AMF, SMF, PCF) | External AF / SCS-AS |
| Spec | TS 29.591 | TS 29.122 |
| Base path | `/nnef-eventexposure/v1/` | `/3gpp-monitoring-event/v1/` |
| Auth | Bearer JWT with NF identity | Bearer JWT with AF identity |

---

## Base Path

```
/nnef-eventexposure/v1/subscriptions
```

---

## Endpoints

| Method | Path | Description |
|---|---|---|
| `POST` | `/nnef-eventexposure/v1/subscriptions` | Create an event subscription |
| `GET` | `/nnef-eventexposure/v1/subscriptions` | List all subscriptions |
| `GET` | `/nnef-eventexposure/v1/subscriptions/{subId}` | Get a single subscription |
| `PUT` | `/nnef-eventexposure/v1/subscriptions/{subId}` | Replace a subscription |
| `DELETE` | `/nnef-eventexposure/v1/subscriptions/{subId}` | Delete a subscription |

---

## POST — Create Event Subscription

Creates a new event subscription. NEF stores the subscription and begins monitoring for the specified events. When a subscribed event is detected, NEF POSTs a notification to the `notifUri`.

**Request**

```http
POST /nnef-eventexposure/v1/subscriptions HTTP/2
Host: oai-nef:8080
Content-Type: application/json
Authorization: Bearer <your-jwt-token>
```

```json
{
  "nfId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "notifUri": "http://oai-amf:8080/namf-evts/v1/notify/nef-events",
  "nfType": "AMF",
  "eventsSubs": [
    {
      "event": "UE_REACHABILITY_FOR_DATA",
      "extraData": {}
    },
    {
      "event": "LOSS_OF_CONNECTIVITY",
      "extraData": {
        "maximumDetectionTime": 3600
      }
    }
  ],
  "expiry": "2026-05-01T00:00:00Z",
  "options": {
    "trigger": "PERIODIC",
    "maxReports": 500,
    "repPeriod": 60
  }
}
```

**curl example**

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/nnef-eventexposure/v1/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "nfId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "notifUri": "http://oai-amf:8080/namf-evts/v1/notify/nef-events",
    "nfType": "AMF",
    "eventsSubs": [
      {"event": "UE_REACHABILITY_FOR_DATA", "extraData": {}},
      {"event": "LOSS_OF_CONNECTIVITY", "extraData": {"maximumDetectionTime": 3600}}
    ],
    "expiry": "2026-05-01T00:00:00Z",
    "options": {
      "trigger": "PERIODIC",
      "maxReports": 500,
      "repPeriod": 60
    }
  }'
```

**Response 201 Created**

```json
{
  "subscriptionId": "nef-sub-abc123",
  "notifUri": "http://oai-amf:8080/namf-evts/v1/notify/nef-events",
  "nfId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "nfType": "AMF",
  "eventsSubs": [
    {
      "event": "UE_REACHABILITY_FOR_DATA",
      "extraData": {}
    },
    {
      "event": "LOSS_OF_CONNECTIVITY",
      "extraData": {
        "maximumDetectionTime": 3600
      }
    }
  ],
  "expiry": "2026-05-01T00:00:00Z",
  "reportList": []
}
```

### Request Field Reference

| Field | Type | Required | Description |
|---|---|---|---|
| `nfId` | string (UUID) | **Required** | Subscribing NF's instance ID (UUID v4) |
| `notifUri` | string | **Required** | NF callback URI; NEF POSTs notifications here |
| `nfType` | string | **Required** | NF type: `"AMF"`, `"SMF"`, `"PCF"`, `"NWDAF"` |
| `eventsSubs` | array | **Required** | List of event subscription objects |
| `eventsSubs[].event` | string | **Required** | Event name (e.g., `"UE_REACHABILITY_FOR_DATA"`, `"LOSS_OF_CONNECTIVITY"`) |
| `eventsSubs[].extraData` | object | Optional | Event-specific configuration parameters |
| `expiry` | string (ISO 8601) | Optional | Subscription expiry datetime; see [Subscription Expiry Semantics](#subscription-expiry-semantics) |
| `options` | object | Optional | Delivery control options |
| `options.trigger` | string | Optional | Delivery trigger: `"PERIODIC"` or `"ONE_TIME"` |
| `options.maxReports` | integer | Optional | Maximum total notifications to deliver before auto-cancelling |
| `options.repPeriod` | integer | Optional | Reporting period in seconds (applies when `trigger` is `"PERIODIC"`) |

---

## GET — List Subscriptions

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/nnef-eventexposure/v1/subscriptions
```

**Response 200 OK**

```json
[
  {
    "subscriptionId": "nef-sub-abc123",
    "notifUri": "http://oai-amf:8080/namf-evts/v1/notify/nef-events",
    "nfId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "nfType": "AMF",
    "eventsSubs": [
      {"event": "UE_REACHABILITY_FOR_DATA", "extraData": {}},
      {"event": "LOSS_OF_CONNECTIVITY", "extraData": {"maximumDetectionTime": 3600}}
    ],
    "expiry": "2026-05-01T00:00:00Z"
  }
]
```

---

## GET — Get Single Subscription

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/nnef-eventexposure/v1/subscriptions/nef-sub-abc123
```

**Response 200 OK** — Returns the full subscription object.

**Response 404 Not Found** — Returned if the subscription does not exist or has expired and been auto-deleted (see [Subscription Expiry Semantics](#subscription-expiry-semantics)).

---

## PUT — Replace Subscription

Replaces all modifiable fields. Useful for updating the `notifUri`, adding/removing subscribed events, or extending the `expiry` time.

```bash
curl --http2-prior-knowledge \
  -X PUT http://oai-nef:8080/nnef-eventexposure/v1/subscriptions/nef-sub-abc123 \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "nfId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "notifUri": "http://oai-amf:8080/namf-evts/v1/notify/nef-events-v2",
    "nfType": "AMF",
    "eventsSubs": [
      {"event": "UE_REACHABILITY_FOR_DATA", "extraData": {}},
      {"event": "LOSS_OF_CONNECTIVITY", "extraData": {"maximumDetectionTime": 7200}},
      {"event": "COMMUNICATION_FAILURE", "extraData": {}}
    ],
    "expiry": "2026-06-01T00:00:00Z",
    "options": {
      "trigger": "PERIODIC",
      "maxReports": 1000,
      "repPeriod": 120
    }
  }'
```

**Response 200 OK** — Returns the updated subscription.

---

## DELETE — Delete Subscription

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/nnef-eventexposure/v1/subscriptions/nef-sub-abc123
```

**Response 204 No Content** — Subscription deleted. NEF immediately stops delivering notifications to the `notifUri`.

---

## Notification Delivery

When NEF detects a subscribed event, it POSTs a notification to the `notifUri`. The notification body follows **TS 29.591 §5.2.6.2** format.

**Example notification payload**

```json
{
  "subscriptionId": "nef-sub-abc123",
  "notifItems": [
    {
      "event": "UE_REACHABILITY_FOR_DATA",
      "ueId": "imsi-208950000000001",
      "timestamp": "2026-04-25T12:00:00Z"
    }
  ]
}
```

**Multiple events in a single notification**

```json
{
  "subscriptionId": "nef-sub-abc123",
  "notifItems": [
    {
      "event": "LOSS_OF_CONNECTIVITY",
      "ueId": "imsi-208950000000031",
      "timestamp": "2026-04-25T12:05:00Z"
    },
    {
      "event": "UE_REACHABILITY_FOR_DATA",
      "ueId": "imsi-208950000000044",
      "timestamp": "2026-04-25T12:05:01Z"
    }
  ]
}
```

**NF response**: The `notifUri` endpoint must return `200 OK` or `204 No Content` to acknowledge receipt. NEF does not retry failed notification deliveries in this release.

---

## Subscription Expiry Semantics

If the `expiry` field is set on a subscription, NEF's `task_manager` periodically checks subscription validity. When a subscription expires:

1. NEF removes the subscription from its internal in-memory store.
2. NEF immediately stops delivering notifications to the `notifUri`.
3. Subsequent `GET`, `PUT`, or `DELETE` requests for that `subscriptionId` return `404 Not Found`.

NFs should either:

- Re-subscribe before expiry by sending a new `POST` request, or
- Use `PUT` to extend the `expiry` before the old one lapses, or
- Handle `404` gracefully by re-subscribing when it is received in response to a `GET` or `PUT`.

If `expiry` is omitted, the subscription is persistent until explicitly deleted via `DELETE` or the NEF process restarts (subscriptions are held in memory only — they do not survive a restart).

---

## Error Responses

**400 Bad Request** — Missing required field:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Required field 'nfId' is missing"
}
```

**400 Bad Request** — Invalid `nfType` value:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Invalid nfType: 'UPF'; expected one of AMF, SMF, PCF, NWDAF"
}
```

**401 Unauthorized**:

```json
{
  "type": "about:blank",
  "title": "Unauthorized",
  "status": 401,
  "detail": "JWT token validation failed: signature mismatch"
}
```

**403 Forbidden**:

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "Caller is not authorised to access nnef-eventexposure"
}
```

**404 Not Found** — Subscription not found or has expired:

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "Subscription 'nef-sub-abc123' not found"
}
```

**429 Too Many Requests**:

```json
{
  "type": "about:blank",
  "title": "Too Many Requests",
  "status": 429,
  "detail": "Rate limit exceeded; retry after the token bucket refills"
}
```

**503 Service Unavailable**:

```json
{
  "type": "about:blank",
  "title": "Service Unavailable",
  "status": 503,
  "detail": "Circuit breaker OPEN for AMF; downstream NF is unreachable"
}
```

---

## Related Pages

- [API Overview](overview.md) — Authentication, error format, HTTP/2 requirements
- [Monitoring Event API](monitoring-event.md) — External AF event subscriptions (T8 interface)
- [Analytics Exposure API](analytics-exposure.md) — NWDAF analytics relay subscriptions
- [Operational Endpoints](operational-endpoints.md) — Health check and NF notification receiver
