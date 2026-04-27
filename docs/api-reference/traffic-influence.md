# Traffic Influence API (TS 29.522)

This page is the complete reference for the NEF Traffic Influence northbound API. Read the
[API Reference Overview](overview.md) first for authentication, HTTP/2 requirements, and error
format conventions that apply to all NEF APIs.

---

## Overview

The Traffic Influence API allows AFs to create **traffic steering policies** that route UE
sessions to specific destinations or apply Quality-of-Service (QoS) constraints. Policies can
target individual UEs (by SUPI or IPv4 address), specific Data Network Names (DNN), or specific
network slices (S-NSSAI), and can be constrained to defined validity time windows.

This API is defined by **3GPP TS 29.522** (T8 reference point). Once an AF creates a traffic
influence subscription, NEF proxies it southbound to PCF as a
`Npcf_PolicyAuthorization` application session (TS 29.514). PCF translates the policy into
PCC rules that are applied to the relevant PDU sessions via SMF.

Key operations covered here:

- AF → NEF: CRUD + PATCH operations on traffic influence subscriptions.
- NEF → PCF: Policy application session create / update / delete via `Npcf_PolicyAuthorization`.
- NEF → AF: Optional notifications on traffic routing changes via the AF's `notificationUri`.

---

## Base Path

```
/3gpp-traffic-influence/v1/{afId}/subscriptions
```

`{afId}` is the AF identifier of the calling application. It must match the `sub` claim of the
presented JWT. See the [Overview — AF/SCS-AS ID](overview.md#af--scs-as-id-path-parameter)
section for validation rules.

---

## Request Fields for POST and PUT

| Field | Type | Required | Description |
|---|---|---|---|
| `afAppId` | string | Optional | Application identifier used to label the policy in PCF. |
| `dnn` | string | Optional | Data Network Name to which the policy applies, e.g. `"internet"` or `"ims"`. |
| `snssai` | object | Optional | S-NSSAI identifying the network slice: `{"sst": 1, "sd": "010203"}`. |
| `notificationUri` | string (URI) | Optional | AF callback URI for traffic influence event notifications from NEF. |
| `trafficFilters` | array | Optional | IP flow filter descriptors that identify the target traffic stream. |
| `trafficRoutes` | array | Optional | Traffic routing rules specifying the desired path or uplink classifier behaviour. |
| `tempValidities` | array | Optional | Time validity windows during which the policy is active. Each window has `startTime` and `stopTime` (ISO 8601). |
| `ueIpv4Addr` | string (IPv4) | Optional | IPv4 address of the target UE. |
| `supi` | string | Optional | SUPI of the target UE, e.g. `"imsi-208950000000001"`. |
| `priorityLevel` | integer | Optional | Routing priority, 1 (highest) to 16 (lowest). |

For `PATCH` requests, use **JSON Merge Patch** semantics (RFC 7396): include only the fields to
change. Fields not present in the PATCH body are left unchanged. Set a field to `null` to
remove it.

---

## Endpoints

### POST `/{afId}/subscriptions` — Create Subscription

Create a new traffic influence subscription. NEF forwards the policy to PCF via
`Npcf_PolicyAuthorization` immediately after persisting the record.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "afAppId": "video-streaming-app",
           "dnn": "internet",
           "snssai": {"sst": 1, "sd": "010203"},
           "notificationUri": "http://my-af.example.com:9090/notify/traffic",
           "supi": "imsi-208950000000001",
           "ueIpv4Addr": "10.45.0.3",
           "priorityLevel": 2,
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
             {
               "dnai": "edge-dc-north",
               "routeProfId": "low-latency-profile"
             }
           ],
           "tempValidities": [
             {
               "startTime": "2026-04-25T00:00:00Z",
               "stopTime": "2026-04-26T00:00:00Z"
             }
           ]
         }' \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions
```

**Response (201 Created)**

```json
{
  "self": "/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc",
  "afTransId": "c3d4e5f6-a7b8-9012-cdef-234567890abc",
  "afAppId": "video-streaming-app",
  "dnn": "internet",
  "snssai": {"sst": 1, "sd": "010203"},
  "notificationUri": "http://my-af.example.com:9090/notify/traffic",
  "supi": "imsi-208950000000001",
  "ueIpv4Addr": "10.45.0.3",
  "priorityLevel": 2,
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
    {
      "dnai": "edge-dc-north",
      "routeProfId": "low-latency-profile"
    }
  ],
  "tempValidities": [
    {
      "startTime": "2026-04-25T00:00:00Z",
      "stopTime": "2026-04-26T00:00:00Z"
    }
  ]
}
```

---

### GET `/{afId}/subscriptions` — List All Subscriptions

Retrieve all active traffic influence subscriptions owned by the calling AF.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions
```

**Response (200 OK)**: Array of subscription objects. Returns `[]` if none exist.

```json
[
  {
    "self": "/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc",
    "afTransId": "c3d4e5f6-a7b8-9012-cdef-234567890abc",
    "afAppId": "video-streaming-app",
    "dnn": "internet",
    "snssai": {"sst": 1, "sd": "010203"},
    "supi": "imsi-208950000000001",
    "priorityLevel": 2
  }
]
```

---

### GET `/{afId}/subscriptions/{afTransId}` — Get One Subscription

Retrieve a single traffic influence subscription.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

**Response (200 OK)**: Single subscription object (same schema as the 201 response).

---

### PUT `/{afId}/subscriptions/{afTransId}` — Update Subscription (Full Replace)

Replace the entire subscription resource. All fields must be re-supplied; omitted fields revert
to defaults. NEF pushes the updated policy to PCF via `Npcf_PolicyAuthorization` update.

**Request Body**: Same schema as `POST`.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "afAppId": "video-streaming-app",
           "dnn": "internet",
           "snssai": {"sst": 1, "sd": "010203"},
           "notificationUri": "http://my-af.example.com:9090/notify/traffic-v2",
           "supi": "imsi-208950000000001",
           "ueIpv4Addr": "10.45.0.3",
           "priorityLevel": 1,
           "trafficFilters": [
             {
               "flowId": 1,
               "flowDescriptions": [
                 "permit out 6 from 203.0.113.10 443 to assigned",
                 "permit in 6 from assigned to 203.0.113.10 443"
               ]
             }
           ],
           "trafficRoutes": [
             {
               "dnai": "edge-dc-south",
               "routeProfId": "ultra-low-latency-profile"
             }
           ]
         }' \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

**Response (200 OK)**: Updated subscription object.

---

### PATCH `/{afId}/subscriptions/{afTransId}` — Partial Update

Apply a partial update using **JSON Merge Patch** (RFC 7396). Include only the fields you want
to change. Fields absent from the body are left unchanged. Set a field to `null` to clear it.

**curl Example** — Change only the priority level and notification URI:

```bash
curl --http2-prior-knowledge \
     -X PATCH \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/merge-patch+json" \
     -d '{
           "priorityLevel": 5,
           "notificationUri": "http://my-af.example.com:9090/notify/traffic-patched"
         }' \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

**Response (200 OK)**: Full updated subscription object reflecting the merged state.

```json
{
  "self": "/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc",
  "afTransId": "c3d4e5f6-a7b8-9012-cdef-234567890abc",
  "afAppId": "video-streaming-app",
  "dnn": "internet",
  "snssai": {"sst": 1, "sd": "010203"},
  "notificationUri": "http://my-af.example.com:9090/notify/traffic-patched",
  "supi": "imsi-208950000000001",
  "ueIpv4Addr": "10.45.0.3",
  "priorityLevel": 5,
  "trafficFilters": [
    {
      "flowId": 1,
      "flowDescriptions": [
        "permit out 6 from 203.0.113.10 443 to assigned",
        "permit in 6 from assigned to 203.0.113.10 443"
      ]
    }
  ]
}
```

---

### DELETE `/{afId}/subscriptions/{afTransId}` — Delete Subscription

Delete a traffic influence subscription. NEF removes the PCF app session via
`Npcf_PolicyAuthorization` delete, and event notifications for this subscription stop
immediately.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-traffic-influence/v1/my-af-1/subscriptions/c3d4e5f6-a7b8-9012-cdef-234567890abc
```

**Response — 204 No Content** (empty body).

---

## Subscription Expiry Semantics

If `tempValidities` defines one or more time windows, NEF's `task_manager` monitors each
window's `stopTime`. When **all validity windows have expired**:

1. NEF sends a `Npcf_PolicyAuthorization` delete to PCF, revoking the traffic policy.
2. The subscription record is removed from NEF's in-memory state.
3. Any subsequent `GET`, `PUT`, `PATCH`, or `DELETE` for that subscription ID returns `404`.

Subscriptions with **no `tempValidities`** persist indefinitely until explicitly deleted by the
AF.

If `notificationUri` is configured, NEF delivers an expiry notification to the AF before
removing the subscription.

---

## Southbound Behavior

| AF Action | NEF → PCF Action |
|---|---|
| `POST` — create subscription | `POST Npcf_PolicyAuthorization/v1/app-sessions` — create app session |
| `PUT` — full update | `PATCH Npcf_PolicyAuthorization/v1/app-sessions/{appSessionId}/modify` — update session |
| `PATCH` — partial update | `PATCH Npcf_PolicyAuthorization/v1/app-sessions/{appSessionId}/modify` — update session |
| `DELETE` — delete subscription | `POST Npcf_PolicyAuthorization/v1/app-sessions/{appSessionId}/delete` — terminate session |

NEF maintains the mapping between the T8 subscription identifier (`afTransId`) and the PCF app
session identifier internally. AFs do not interact with PCF directly.

---

## Error Responses

| Status | Title | Typical Cause |
|---|---|---|
| `400 Bad Request` | Bad Request | Malformed JSON; invalid field type; body exceeds 1 MiB. |
| `401 Unauthorized` | Unauthorized | Missing or invalid JWT; expired token. |
| `403 Forbidden` | Forbidden | `afId` does not match the JWT `sub` claim; `allowed_apis` restriction. |
| `404 Not Found` | Not Found | Subscription ID does not exist or has been auto-expired. |
| `429 Too Many Requests` | Too Many Requests | Global token-bucket rate limit exceeded; apply exponential back-off. |
| `503 Service Unavailable` | Service Unavailable | Circuit breaker for PCF is in the OPEN state. |

**Example — 400 Bad Request**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Field 'priorityLevel' must be an integer between 1 and 16"
}
```

**Example — 404 Not Found**

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "Subscription 'c3d4e5f6-a7b8-9012-cdef-234567890abc' not found"
}
```

**Example — 503 Service Unavailable**

```json
{
  "type": "about:blank",
  "title": "Service Unavailable",
  "status": 503,
  "detail": "Circuit breaker OPEN for PCF; downstream NF is unreachable"
}
```

---

See [Call Flows](../call-flows.md) for the complete end-to-end sequence diagram.
