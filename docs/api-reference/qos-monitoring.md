# QoS Monitoring / AS Session with QoS API (TS 29.122)

Read the [API Overview](overview.md) before this page for authentication, error format, and HTTP/2 requirements that apply to all NEF APIs.

---

## Overview

Application Functions (AFs) use this API to create **AS (Application Server) sessions with QoS** — requesting guaranteed quality-of-service parameters for specific UE data flows. NEF mediates between the AF and the Policy Control Function (PCF), translating the AF's QoS reference into a 5QI-based policy authorization.

This service is standardized in **3GPP TS 29.122 §8.a** (T8 interface, AS Session with QoS resource).

> **IMPORTANT — Path Naming**: The base path in this implementation is `/3gpp-as-session-with-qos/v1/` after the "AS Session with QoS" feature name defined in TS 29.122. This differs from the label "QoS Monitoring" that appears in some 3GPP discussions. Do not use `/3gpp-qos-monitoring/` — that path is not registered.

---

## Base Path

```
/3gpp-as-session-with-qos/v1/{scsAsId}/subscriptions
```

| Parameter | Type | Description |
|---|---|---|
| `scsAsId` | string | SCS/AS identifier; must match the `sub` claim in the JWT |

---

## Endpoints

| Method | Path | Description |
|---|---|---|
| `POST` | `/3gpp-as-session-with-qos/v1/{scsAsId}/subscriptions` | Create a QoS session subscription |
| `GET` | `/3gpp-as-session-with-qos/v1/{scsAsId}/subscriptions` | List all subscriptions for the AF |
| `GET` | `/3gpp-as-session-with-qos/v1/{scsAsId}/subscriptions/{subId}` | Get a single subscription |
| `PUT` | `/3gpp-as-session-with-qos/v1/{scsAsId}/subscriptions/{subId}` | Replace a subscription |
| `DELETE` | `/3gpp-as-session-with-qos/v1/{scsAsId}/subscriptions/{subId}` | Delete a subscription |

---

## POST — Create QoS Session Subscription

Creates a new AS session with QoS subscription. NEF forwards the request to PCF via `Npcf_PolicyAuthorization`.

**Request**

```http
POST /3gpp-as-session-with-qos/v1/af-1/subscriptions HTTP/2
Host: oai-nef:8080
Content-Type: application/json
Authorization: Bearer <your-jwt-token>
```

```json
{
  "ueIpv4Addr": "10.45.0.2",
  "notificationUri": "http://af.example.com/notify/qos",
  "qosReference": "GBR_ConvVoice",
  "dnn": "internet",
  "snssai": {
    "sst": 1,
    "sd": "010203"
  },
  "qosMon": {
    "reqQosMonParams": ["DOWNLINK", "UPLINK"],
    "repPeriod": 30,
    "waitTime": 5
  },
  "usageThreshold": {
    "duration": 3600,
    "totalVolume": 104857600,
    "uplinkVolume": 10485760,
    "downlinkVolume": 94371840
  }
}
```

**curl example**

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-as-session-with-qos/v1/af-1/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "ueIpv4Addr": "10.45.0.2",
    "notificationUri": "http://af.example.com/notify/qos",
    "qosReference": "GBR_ConvVoice",
    "dnn": "internet",
    "snssai": {"sst": 1, "sd": "010203"},
    "qosMon": {
      "reqQosMonParams": ["DOWNLINK", "UPLINK"],
      "repPeriod": 30,
      "waitTime": 5
    }
  }'
```

**Response 201 Created**

```json
{
  "self": "/3gpp-as-session-with-qos/v1/af-1/subscriptions/b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90",
  "subscriptionId": "b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90",
  "ueIpv4Addr": "10.45.0.2",
  "notificationUri": "http://af.example.com/notify/qos",
  "qosReference": "GBR_ConvVoice",
  "dnn": "internet",
  "snssai": {
    "sst": 1,
    "sd": "010203"
  },
  "qosMon": {
    "reqQosMonParams": ["DOWNLINK", "UPLINK"],
    "repPeriod": 30,
    "waitTime": 5
  }
}
```

### Request Field Reference

| Field | Type | Required | Description |
|---|---|---|---|
| `ueIpv4Addr` | string | Optional* | UE's IPv4 address (e.g., `"10.45.0.2"`) |
| `ueIpv6Addr` | string | Optional* | UE's IPv6 address |
| `supi` | string | Optional* | UE's SUPI (e.g., `"imsi-208950000000031"`) |
| `notificationUri` | string | **Required** | AF callback URI for QoS event notifications |
| `qosReference` | string | **Required** | Pre-configured QoS profile name (mapped to a 5QI by PCF) |
| `usageThreshold` | object | Optional | Volume/time limits before notifying the AF |
| `usageThreshold.duration` | integer | Optional | Time threshold in seconds |
| `usageThreshold.totalVolume` | integer | Optional | Total volume threshold in bytes |
| `usageThreshold.uplinkVolume` | integer | Optional | Uplink volume threshold in bytes |
| `usageThreshold.downlinkVolume` | integer | Optional | Downlink volume threshold in bytes |
| `qosMon` | object | Optional | QoS monitoring parameters |
| `qosMon.reqQosMonParams` | array of string | Optional | Directions to monitor: `"UPLINK"`, `"DOWNLINK"`, `"ROUND_TRIP"` |
| `qosMon.repPeriod` | integer | Optional | Reporting period in seconds |
| `qosMon.waitTime` | integer | Optional | Minimum wait time between consecutive reports in seconds |
| `dnn` | string | Optional | Data Network Name (APN) |
| `snssai` | object | Optional | S-NSSAI slice identifier |
| `snssai.sst` | integer | Optional | Slice/Service Type (0–255) |
| `snssai.sd` | string | Optional | Slice Differentiator (hex string, e.g., `"010203"`) |

*At least one UE identifier (`ueIpv4Addr`, `ueIpv6Addr`, or `supi`) should be provided to target a specific UE.

---

## GET — List Subscriptions

Returns all active QoS session subscriptions for the AF.

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-as-session-with-qos/v1/af-1/subscriptions
```

**Response 200 OK**

```json
[
  {
    "self": "/3gpp-as-session-with-qos/v1/af-1/subscriptions/b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90",
    "subscriptionId": "b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90",
    "ueIpv4Addr": "10.45.0.2",
    "notificationUri": "http://af.example.com/notify/qos",
    "qosReference": "GBR_ConvVoice",
    "dnn": "internet"
  }
]
```

Returns an empty array `[]` if no subscriptions exist.

---

## GET — Get Single Subscription

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-as-session-with-qos/v1/af-1/subscriptions/b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90
```

**Response 200 OK** — Returns the subscription object (same schema as POST response).

---

## PUT — Replace Subscription

Replaces all fields of an existing subscription. The request body must contain a complete subscription object — partial updates are not supported by PUT. Use the same schema as POST.

```bash
curl --http2-prior-knowledge \
  -X PUT http://oai-nef:8080/3gpp-as-session-with-qos/v1/af-1/subscriptions/b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90 \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "ueIpv4Addr": "10.45.0.2",
    "notificationUri": "http://af.example.com/notify/qos-v2",
    "qosReference": "GBR_ConvVoice",
    "dnn": "internet",
    "snssai": {"sst": 1, "sd": "010203"},
    "qosMon": {
      "reqQosMonParams": ["DOWNLINK", "UPLINK", "ROUND_TRIP"],
      "repPeriod": 60,
      "waitTime": 10
    }
  }'
```

**Response 200 OK** — Returns the updated subscription object.

---

## DELETE — Delete Subscription

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-as-session-with-qos/v1/af-1/subscriptions/b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90
```

**Response 204 No Content** — Subscription deleted. NEF also revokes the corresponding `Npcf_PolicyAuthorization` AppSession at PCF.

---

## Southbound Behavior

When the AF creates a QoS session subscription, NEF performs the following southbound operations:

1. **PCF — `Npcf_PolicyAuthorization` (TS 29.514)**: NEF creates an AppSession at PCF with the AF's `qosReference` translated to a 5QI. The PCF enforces QoS for the UE's PDU session via the SMF.
2. **QoS monitoring reporting**: If `qosMon` is present, NEF configures the PCF to report QoS metrics at the requested `repPeriod`. When the PCF detects a QoS change or threshold crossing, it notifies NEF, which forwards the notification to the AF's `notificationUri`.

The `qosReference` value must correspond to a pre-configured 5QI mapping at the PCF. If the reference is unknown to the PCF, the southbound call fails and NEF returns `503 Service Unavailable`.

---

## Notification Payload

When the PCF reports a QoS event, NEF forwards it to the AF's `notificationUri`:

```json
{
  "subscriptionId": "b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90",
  "eventType": "QOS_MONITORING",
  "ueIpv4Addr": "10.45.0.2",
  "qosMonitoringReport": {
    "ulDelay": 12,
    "dlDelay": 8,
    "rtDelay": 20
  },
  "timestamp": "2026-04-25T10:30:00Z"
}
```

---

## Error Responses

**400 Bad Request** — Missing or invalid field:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Required field 'qosReference' is missing"
}
```

**401 Unauthorized** — Invalid or missing JWT:

```json
{
  "type": "about:blank",
  "title": "Unauthorized",
  "status": 401,
  "detail": "JWT token validation failed: signature mismatch"
}
```

**403 Forbidden** — `scsAsId` does not match the authenticated identity:

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF 'af-1' is not authorised to call qos_monitoring"
}
```

**404 Not Found** — Subscription ID does not exist:

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "Subscription 'b7e3a1c2-4f8d-4b9e-a3d2-1c5f6e7d8b90' not found"
}
```

**429 Too Many Requests** — Rate limit exceeded:

```json
{
  "type": "about:blank",
  "title": "Too Many Requests",
  "status": 429,
  "detail": "Rate limit exceeded; retry after the token bucket refills"
}
```

**503 Service Unavailable** — PCF circuit breaker is open:

```json
{
  "type": "about:blank",
  "title": "Service Unavailable",
  "status": 503,
  "detail": "Circuit breaker OPEN for PCF; downstream NF is unreachable"
}
```

---

## Related Pages

- [API Overview](overview.md) — Authentication, error format, HTTP/2 requirements
- [BDT Policy API](bdt-policy.md) — Background data transfer scheduling
- [Traffic Influence API](traffic-influence.md) — Routing and uplink classifier control
