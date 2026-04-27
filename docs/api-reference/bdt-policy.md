# Background Data Transfer Policy API (TS 29.122)

Read the [API Overview](overview.md) before this page for authentication, error format, and HTTP/2 requirements that apply to all NEF APIs.

---

## Overview

The Background Data Transfer (BDT) Policy API enables AFs to negotiate time windows with the network for large, deferrable data transfers — firmware updates, backups, analytics uploads — that should occur during low-demand periods. NEF interacts with the Policy Control Function (PCF) via `Npcf_BDTPolicyControl` (TS 29.554) to obtain an operator-approved transfer schedule.

This service is standardized in **3GPP TS 29.122** (T8 interface, Background Data Transfer resource).

> **IMPORTANT — Path Naming**: The base path in this implementation is `/3gpp-bdt/v1/` (not `/3gpp-bdt-policy/v1/`). See [API Overview](overview.md) for the full service identifier table.

---

## Base Path

```
/3gpp-bdt/v1/{scsAsId}/policies
```

| Parameter | Type | Description |
|---|---|---|
| `scsAsId` | string | SCS/AS identifier; must match the `sub` claim in the JWT |

---

## BDT Negotiation Flow

```
AF  ──POST /policies──►  NEF  ──Npcf_BDTPolicyControl──►  PCF
                                                            │
                          NEF  ◄── transferPolicies ────────┘
AF  ◄── 200 + policies ──
     (selects policy ID)
AF  ──PATCH /policies/{id}──►  NEF  (confirms selected policy to PCF)
```

1. AF proposes `desiredTimeWindows` in a POST request.
2. NEF forwards the request to PCF via `Npcf_BDTPolicyControl`.
3. PCF evaluates network load forecasts and returns one or more offered `transferPolicies`.
4. NEF returns the offered policies to the AF in the POST response body.
5. The AF selects one `transferPolicyId` and confirms the selection via PATCH.

---

## Endpoints

| Method | Path | Description |
|---|---|---|
| `POST` | `/3gpp-bdt/v1/{scsAsId}/policies` | Request a new BDT policy |
| `GET` | `/3gpp-bdt/v1/{scsAsId}/policies` | List all BDT policies for the AF |
| `GET` | `/3gpp-bdt/v1/{scsAsId}/policies/{bdtPolicyId}` | Get a single BDT policy |
| `PUT` | `/3gpp-bdt/v1/{scsAsId}/policies/{bdtPolicyId}` | Replace a BDT policy |
| `PATCH` | `/3gpp-bdt/v1/{scsAsId}/policies/{bdtPolicyId}` | Select a transfer policy (confirm negotiation) |
| `DELETE` | `/3gpp-bdt/v1/{scsAsId}/policies/{bdtPolicyId}` | Delete a BDT policy |

---

## POST — Request a BDT Policy

Submits a BDT negotiation request. NEF forwards to PCF and returns the operator-offered transfer windows.

**Request**

```http
POST /3gpp-bdt/v1/af-1/policies HTTP/2
Host: oai-nef:8080
Content-Type: application/json
Authorization: Bearer <your-jwt-token>
```

```json
{
  "volPerUE": {
    "uplink": 52428800,
    "downlink": 524288000,
    "total": 576716800
  },
  "numOfUEs": 500,
  "desiredTimeWindows": [
    {
      "startTime": "2026-04-26T02:00:00Z",
      "stopTime": "2026-04-26T06:00:00Z"
    },
    {
      "startTime": "2026-04-27T02:00:00Z",
      "stopTime": "2026-04-27T06:00:00Z"
    }
  ],
  "notificationUri": "http://af.example.com/notify/bdt",
  "trafficDnn": "internet",
  "trafficSnssai": {
    "sst": 1,
    "sd": "010203"
  }
}
```

**curl example**

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-bdt/v1/af-1/policies \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "volPerUE": {
      "uplink": 52428800,
      "downlink": 524288000,
      "total": 576716800
    },
    "numOfUEs": 500,
    "desiredTimeWindows": [
      {"startTime": "2026-04-26T02:00:00Z", "stopTime": "2026-04-26T06:00:00Z"},
      {"startTime": "2026-04-27T02:00:00Z", "stopTime": "2026-04-27T06:00:00Z"}
    ],
    "notificationUri": "http://af.example.com/notify/bdt",
    "trafficDnn": "internet",
    "trafficSnssai": {"sst": 1, "sd": "010203"}
  }'
```

**Response 200 OK**

The PCF returns offered transfer windows. NEF relays them in the response body along with a `bdtRefId` that the AF uses in subsequent requests.

```json
{
  "self": "/3gpp-bdt/v1/af-1/policies/bdt-policy-7a3c1e9f",
  "bdtPolicyId": "bdt-policy-7a3c1e9f",
  "bdtRefId": "BDT-REF-2026-00042",
  "volPerUE": {
    "uplink": 52428800,
    "downlink": 524288000,
    "total": 576716800
  },
  "numOfUEs": 500,
  "transferPolicies": [
    {
      "transPolicyId": 1,
      "ratingGroup": 10,
      "timeWindow": {
        "startTime": "2026-04-26T02:00:00Z",
        "stopTime": "2026-04-26T06:00:00Z"
      },
      "maxBitRateDl": "50 Mbps",
      "maxBitRateUl": "10 Mbps"
    },
    {
      "transPolicyId": 2,
      "ratingGroup": 10,
      "timeWindow": {
        "startTime": "2026-04-27T03:00:00Z",
        "stopTime": "2026-04-27T05:00:00Z"
      },
      "maxBitRateDl": "100 Mbps",
      "maxBitRateUl": "20 Mbps"
    }
  ],
  "notificationUri": "http://af.example.com/notify/bdt"
}
```

### Request Field Reference

| Field | Type | Required | Description |
|---|---|---|---|
| `supportedFeatures` | string | Optional | Bitmask of supported optional features (hex string) |
| `volPerUE` | object | **Required** | Per-UE volume requirements |
| `volPerUE.uplink` | integer | Optional | Uplink volume in bytes |
| `volPerUE.downlink` | integer | Optional | Downlink volume in bytes |
| `volPerUE.total` | integer | Optional | Total volume in bytes |
| `numOfUEs` | integer | **Required** | Estimated number of UEs that will perform the transfer |
| `desiredTimeWindows` | array | **Required** | Candidate time windows; each item has `startTime` and `stopTime` (ISO 8601) |
| `notificationUri` | string | Optional | AF callback URI for BDT status notifications |
| `nwAreaInfo` | object | Optional | Network area info (TAI list, cell list) to scope the BDT request |
| `trafficDnn` | string | Optional | Target Data Network Name |
| `trafficSnssai` | object | Optional | S-NSSAI slice identifier (`{sst, sd}`) |

### Response Field Reference

| Field | Type | Description |
|---|---|---|
| `bdtPolicyId` | string | NEF-assigned policy identifier (used in subsequent GET/PATCH/DELETE) |
| `bdtRefId` | string | PCF-assigned BDT reference ID |
| `transferPolicies` | array | Operator-offered time windows with associated QoS constraints |
| `transferPolicies[].transPolicyId` | integer | Transfer policy index; used in PATCH to select a policy |
| `transferPolicies[].timeWindow` | object | Actual offered time window (`startTime`, `stopTime`) |
| `transferPolicies[].maxBitRateDl` | string | Maximum allowed downlink bit rate during the window |
| `transferPolicies[].maxBitRateUl` | string | Maximum allowed uplink bit rate during the window |

---

## GET — List BDT Policies

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-bdt/v1/af-1/policies
```

**Response 200 OK** — Returns an array of BDT policy objects. Returns `[]` if none exist.

---

## GET — Get Single BDT Policy

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-bdt/v1/af-1/policies/bdt-policy-7a3c1e9f
```

**Response 200 OK** — Returns the full BDT policy object including `transferPolicies`.

---

## PATCH — Select a Transfer Policy

After reviewing the offered `transferPolicies` from the POST response, the AF confirms its selection by PATCHing with the chosen `selTransPolicyId`.

```bash
curl --http2-prior-knowledge \
  -X PATCH http://oai-nef:8080/3gpp-bdt/v1/af-1/policies/bdt-policy-7a3c1e9f \
  -H "Content-Type: application/merge-patch+json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{"selTransPolicyId": 2}'
```

This selects `transPolicyId: 2` (the 03:00–05:00 window on 2026-04-27 in the example above). NEF informs the PCF of the selection via `Npcf_BDTPolicyControl`. The PCF activates the corresponding QoS policy for the scheduled window.

**Response 200 OK** — Returns the updated BDT policy object with `selTransPolicyId` set.

---

## PUT — Replace BDT Policy

Replaces all modifiable fields of an existing BDT policy. The response contains the updated policy with new `transferPolicies` returned by the PCF.

```bash
curl --http2-prior-knowledge \
  -X PUT http://oai-nef:8080/3gpp-bdt/v1/af-1/policies/bdt-policy-7a3c1e9f \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "volPerUE": {
      "uplink": 104857600,
      "downlink": 1073741824,
      "total": 1178599424
    },
    "numOfUEs": 750,
    "desiredTimeWindows": [
      {"startTime": "2026-04-28T01:00:00Z", "stopTime": "2026-04-28T05:00:00Z"}
    ],
    "notificationUri": "http://af.example.com/notify/bdt",
    "trafficDnn": "internet",
    "trafficSnssai": {"sst": 1, "sd": "010203"}
  }'
```

**Response 200 OK** — Returns the updated policy with new PCF-offered windows.

---

## DELETE — Delete BDT Policy

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-bdt/v1/af-1/policies/bdt-policy-7a3c1e9f
```

**Response 204 No Content** — Policy deleted. NEF notifies PCF to revoke the BDT policy via `Npcf_BDTPolicyControl`.

---

## Southbound Behavior

| Step | NF | Interface | TS Reference |
|---|---|---|---|
| POST | PCF | `Npcf_BDTPolicyControl_Create` | TS 29.554 §4.2.2 |
| PATCH | PCF | `Npcf_BDTPolicyControl_Update` | TS 29.554 §4.2.3 |
| PUT | PCF | `Npcf_BDTPolicyControl_Update` | TS 29.554 §4.2.3 |
| DELETE | PCF | `Npcf_BDTPolicyControl_Delete` | TS 29.554 §4.2.4 |

If the PCF is unreachable (circuit breaker OPEN), NEF returns `503 Service Unavailable` and does not persist the request.

---

## Error Responses

**400 Bad Request** — Missing required field:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Required field 'numOfUEs' is missing"
}
```

**400 Bad Request** — Invalid time window format:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "desiredTimeWindows[0].startTime is not a valid ISO 8601 datetime"
}
```

**401 Unauthorized**:

```json
{
  "type": "about:blank",
  "title": "Unauthorized",
  "status": 401,
  "detail": "JWT token has expired"
}
```

**403 Forbidden**:

```json
{
  "type": "about:blank",
  "title": "Forbidden",
  "status": 403,
  "detail": "AF 'af-1' is not authorised to call bdt_policy"
}
```

**404 Not Found**:

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "BDT policy 'bdt-policy-7a3c1e9f' not found"
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
  "detail": "Circuit breaker OPEN for PCF; downstream NF is unreachable"
}
```

---

## Related Pages

- [API Overview](overview.md) — Authentication, error format, HTTP/2 requirements
- [QoS Monitoring API](qos-monitoring.md) — Per-flow guaranteed QoS sessions
- [Traffic Influence API](traffic-influence.md) — Routing and uplink classifier control
