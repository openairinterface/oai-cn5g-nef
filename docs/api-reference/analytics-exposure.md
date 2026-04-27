# Analytics Exposure API (TS 29.122 / TS 29.520)

Read the [API Overview](overview.md) before this page for authentication, error format, and HTTP/2 requirements that apply to all NEF APIs.

---

> **Partial Implementation**: Analytics Exposure support in this release is structural only. The routing and data model are defined but no analytics data pipeline is connected. This page documents the API surface as currently implemented in OAI NEF v1.5.1.

---

## Overview

The Analytics Exposure API allows AFs to subscribe to network analytics data produced by the Network Data Analytics Function (NWDAF). NEF acts as a proxy: it forwards analytics subscription requests from AFs to NWDAF and relays analytics reports back to the AF.

This service is standardized in **3GPP TS 29.122 §8.g** and **TS 29.520** (NWDAF Analytics Subscription).

> **IMPORTANT — Path Naming**: The base path is `/3gpp-analyticsexposure/v1/` (not `/nnef-analytics/v1/`). The alternative naming is not registered in this implementation.

> **This release limitation**: In OAI NEF v1.5.1, the NWDAF integration layer is not active. The `handle_analytics_fetch` handler is present and the subscription data model is defined, but no NWDAF backend is connected. POST requests return `201 Created` with a `subscriptionId`, but no analytics notifications will be delivered. This feature is under development.

---

## Base Path

```
/3gpp-analyticsexposure/v1/{scsAsId}/subscriptions
```

| Parameter | Type | Description |
|---|---|---|
| `scsAsId` | string | SCS/AS identifier; must match the `sub` claim in the JWT |

---

## Endpoints

| Method | Path | Description |
|---|---|---|
| `POST` | `/3gpp-analyticsexposure/v1/{scsAsId}/subscriptions` | Create an analytics subscription |
| `GET` | `/3gpp-analyticsexposure/v1/{scsAsId}/subscriptions` | List all analytics subscriptions |
| `GET` | `/3gpp-analyticsexposure/v1/{scsAsId}/subscriptions/{subId}` | Get a single subscription |
| `DELETE` | `/3gpp-analyticsexposure/v1/{scsAsId}/subscriptions/{subId}` | Delete a subscription |

> **Note**: PUT and PATCH are not supported. Analytics subscriptions cannot be updated in place — delete and recreate to change subscription parameters.

---

## POST — Create Analytics Subscription

Creates a new analytics subscription. In this release, NEF stores the subscription and returns a `201 Created` but does not forward it to an active NWDAF.

**Request**

```http
POST /3gpp-analyticsexposure/v1/af-1/subscriptions HTTP/2
Host: oai-nef:8080
Content-Type: application/json
Authorization: Bearer <your-jwt-token>
```

```json
{
  "analyticsId": "UE_MOBILITY",
  "notificationUri": "http://af.example.com/notify/analytics",
  "supi": "imsi-208950000000031",
  "reportingReq": {
    "repPeriod": 60,
    "maxNumOfReports": 100,
    "sampRatio": 100
  }
}
```

**curl example**

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "analyticsId": "UE_MOBILITY",
    "notificationUri": "http://af.example.com/notify/analytics",
    "supi": "imsi-208950000000031",
    "reportingReq": {
      "repPeriod": 60,
      "maxNumOfReports": 100,
      "sampRatio": 100
    }
  }'
```

**Response 201 Created**

```json
{
  "self": "/3gpp-analyticsexposure/v1/af-1/subscriptions/e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b",
  "subscriptionId": "e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b",
  "analyticsId": "UE_MOBILITY",
  "notificationUri": "http://af.example.com/notify/analytics",
  "supi": "imsi-208950000000031",
  "reportingReq": {
    "repPeriod": 60,
    "maxNumOfReports": 100,
    "sampRatio": 100
  }
}
```

**Subscribing for all UEs** — set `anyUE: true` and omit `supi`/`tgtUe`:

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "analyticsId": "NETWORK_PERFORMANCE",
    "notificationUri": "http://af.example.com/notify/analytics",
    "anyUE": true,
    "reportingReq": {
      "repPeriod": 300,
      "maxNumOfReports": 24
    }
  }'
```

### Request Field Reference

| Field | Type | Required | Description |
|---|---|---|---|
| `analyticsId` | string | **Required** | Analytics event name (see [Supported Analytics IDs](#supported-analytics-ids)) |
| `notificationUri` | string | **Required** | AF callback URI for analytics report delivery |
| `supi` | string | Optional | Target UE SUPI (e.g., `"imsi-208950000000031"`) |
| `anyUE` | boolean | Optional | If `true`, request analytics across all UEs (cannot be combined with `supi`) |
| `reportingReq` | object | Optional | Reporting configuration |
| `reportingReq.repPeriod` | integer | Optional | Reporting period in seconds |
| `reportingReq.maxNumOfReports` | integer | Optional | Maximum number of reports to deliver before auto-cancelling |
| `reportingReq.sampRatio` | integer | Optional | Sampling ratio in percent (1–100) |
| `tgtUe` | object | Optional | Target UE identity object (alternative to `supi`) |

---

## GET — List Analytics Subscriptions

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions
```

**Response 200 OK**

```json
[
  {
    "self": "/3gpp-analyticsexposure/v1/af-1/subscriptions/e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b",
    "subscriptionId": "e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b",
    "analyticsId": "UE_MOBILITY",
    "notificationUri": "http://af.example.com/notify/analytics",
    "supi": "imsi-208950000000031"
  }
]
```

Returns `[]` if no subscriptions exist.

---

## GET — Get Single Analytics Subscription

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions/e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b
```

**Response 200 OK** — Returns the full subscription object.

---

## DELETE — Delete Analytics Subscription

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-analyticsexposure/v1/af-1/subscriptions/e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b
```

**Response 204 No Content** — Subscription deleted.

---

## Supported Analytics IDs

The table below lists analytics types that are structurally supported in the data model. Because the NWDAF integration is not active in v1.5.1, all are in **Structural** status — the relay framework is present but reports will not be delivered.

| Analytics ID | Description | TS 29.520 Reference | Status |
|---|---|---|---|
| `UE_MOBILITY` | UE location change predictions and trajectory analysis | §6.1.3.2 | Structural |
| `UE_COMM` | UE communication pattern analysis (traffic volume, periodicity) | §6.1.3.3 | Structural |
| `ABNORMAL_BEHAVIOR` | Detection of abnormal UE behavior (unexpected wakeup, ping-pong) | §6.1.3.4 | Structural |
| `NETWORK_PERFORMANCE` | Network resource utilization and congestion metrics | §6.1.3.5 | Structural |

**Structural**: The relay framework between NEF and NWDAF is present and the API accepts subscriptions, but no NWDAF backend is connected. No notifications will be delivered in v1.5.1.

---

## Known Limitations

> **v1.5.1 Implementation Status**
>
> The NWDAF integration layer is not active in this release.
>
> - `POST` returns `201 Created` with a valid `subscriptionId`.
> - The subscription is stored in NEF's in-memory state and visible via `GET`.
> - **No analytics notifications will be sent** to the `notificationUri`.
> - The southbound `Nnwdaf_AnalyticsSubscription` interface (TS 29.520) is not connected.
>
> This feature is under development. Future releases will activate the NWDAF relay pipeline.

Additional limitations:

- Subscriptions are held in memory only. They are lost when the NEF process restarts.
- `maxNumOfReports` is accepted but not enforced in this release.
- PUT and PATCH operations are not supported — recreate subscriptions to change parameters.

---

## Error Responses

**400 Bad Request** — Missing required field:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Required field 'analyticsId' is missing"
}
```

**400 Bad Request** — Unrecognized analytics ID:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Unknown analyticsId: 'SESSION_MANAGEMENT_EXPERIENCE'"
}
```

**401 Unauthorized**:

```json
{
  "type": "about:blank",
  "title": "Unauthorized",
  "status": 401,
  "detail": "JWT token validation failed: token has expired"
}
```

**403 Forbidden**:

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF 'af-1' is not authorised to call analytics_exposure"
}
```

**404 Not Found**:

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "Subscription 'e4f2a8b1-3c7d-4e9f-b2a1-6d5c8e3f2a7b' not found"
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

---

## Related Pages

- [API Overview](overview.md) — Authentication, error format, HTTP/2 requirements
- [Nnef_EventExposure SBI API](nnef-event-exposure.md) — Internal 5GC NF event subscriptions
- [Monitoring Event API](monitoring-event.md) — UE event monitoring for external AFs
