<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Background Data Transfer Policy API (TS 29.122)

## What this service is for

An ASP (Application Service Provider) with a large, deferrable transfer — a firmware push to a
fleet of devices, a nightly telemetry upload, a map or model refresh — uses this API to agree a
transfer window with the operator instead of sending the traffic whenever it likes and competing
with interactive users.

The AF describes the job (how many UEs, how much data per UE, the desired time interval) and the
transfer policies it is willing to accept, NEF registers that with the PCF over
`Npcf_BDTPolicyControl`, and the resulting BDT reference can then be attached to the subscriptions
that will perform the transfer.

Read the [API Overview](overview.md) first for authentication, HTTP/2 (h2c) requirements and the
shared ProblemDetails error format.

---

## The happy path

Register a BDT policy, read it back, select a transfer policy, then delete it. `af-1` is the
SCS/AS identifier and must match the `sub` claim of the JWT.

**1. Register.** The body is a `BdtPolicy`: `bdtPolData` carries the BDT reference and the
candidate transfer policies, and the optional `bdtReqData` carries the request that produced them.

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-bdt/v1/af-1/bdtPolicies \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "bdtPolData": {
      "bdtRefId": "BDT-REF-2026-00042",
      "transfPolicies": [
        {
          "transPolicyId": 1,
          "ratingGroup": 10,
          "recTimeInt": {"startTime": "2026-04-26T02:00:00Z", "stopTime": "2026-04-26T06:00:00Z"},
          "maxBitRateDl": "50 Mbps",
          "maxBitRateUl": "10 Mbps"
        },
        {
          "transPolicyId": 2,
          "ratingGroup": 10,
          "recTimeInt": {"startTime": "2026-04-27T03:00:00Z", "stopTime": "2026-04-27T05:00:00Z"},
          "maxBitRateDl": "100 Mbps",
          "maxBitRateUl": "20 Mbps"
        }
      ]
    },
    "bdtReqData": {
      "aspId": "asp-firmware-1",
      "numOfUes": 500,
      "volPerUe": {"uplinkVolume": 52428800, "downlinkVolume": 524288000, "totalVolume": 576716800},
      "desTimeInt": {"startTime": "2026-04-26T02:00:00Z", "stopTime": "2026-04-27T06:00:00Z"},
      "dnn": "internet",
      "snssai": {"sst": 1, "sd": "010203"}
    }
  }'
```

**Response `201 Created`** — the body you sent, plus a top-level `bdtRefId` holding the identifier
NEF assigned to the resource:

```json
{
  "bdtPolData": {
    "bdtRefId": "BDT-REF-2026-00042",
    "transfPolicies": [
      {
        "transPolicyId": 1,
        "ratingGroup": 10,
        "recTimeInt": {"startTime": "2026-04-26T02:00:00Z", "stopTime": "2026-04-26T06:00:00Z"},
        "maxBitRateDl": "50 Mbps",
        "maxBitRateUl": "10 Mbps"
      },
      {
        "transPolicyId": 2,
        "ratingGroup": 10,
        "recTimeInt": {"startTime": "2026-04-27T03:00:00Z", "stopTime": "2026-04-27T05:00:00Z"},
        "maxBitRateDl": "100 Mbps",
        "maxBitRateUl": "20 Mbps"
      }
    ]
  },
  "bdtReqData": { "...": "as sent" },
  "bdtRefId": "7a3c1e9f-2b44-4d1e-9c8a-0f5b2d6e1a73"
}
```

Read the resource identifier from the **top-level** `bdtRefId`, not from `bdtPolData.bdtRefId` —
the nested one is the BDT reference you supplied and NEF does not overwrite it. There is no
`Location` header and no `self` field.

**2. Read it back.**

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-bdt/v1/af-1/bdtPolicies/7a3c1e9f-2b44-4d1e-9c8a-0f5b2d6e1a73
```

**3. Select the transfer policy the AF will actually use.**

```bash
curl --http2-prior-knowledge \
  -X PATCH http://oai-nef:8080/3gpp-bdt/v1/af-1/bdtPolicies/7a3c1e9f-2b44-4d1e-9c8a-0f5b2d6e1a73 \
  -H "Content-Type: application/merge-patch+json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{"bdtPolData": {"selTransPolicyId": 2}}'
```

**4. Delete when the transfer campaign is over.**

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-bdt/v1/af-1/bdtPolicies/7a3c1e9f-2b44-4d1e-9c8a-0f5b2d6e1a73
```

---

## Base path

```
/3gpp-bdt/v1/{scsAsId}/bdtPolicies[/{bdtPolicyId}]
```

| Parameter | Type | Description |
|---|---|---|
| `scsAsId` | string | SCS/AS identifier; must match the `sub` claim in the JWT. Maximum 256 characters. |
| `bdtPolicyId` | string | The identifier NEF returned in the top-level `bdtRefId` of the `201`. |

> **Path naming**: the base is `/3gpp-bdt/v1/`, not `/3gpp-bdt-policy/v1/`. See the
> [API Overview](overview.md#service-identifiers) for the full service identifier table.

### The deprecated `/policies` spelling

`/3gpp-bdt/v1/{scsAsId}/policies` is still routed for `POST`, `GET`, `PUT` and `DELETE`. It logs a
deprecation warning server-side and marks every response with an `x-deprecated: true` header.

`PATCH` is **not** routed on `/policies`. A `PATCH` there is answered `405 Method Not Allowed`, not
a successful update. Use `/bdtPolicies` — it is the canonical TS 29.122 §5.13 spelling and the only
one on which the full method set works.

---

## Request body

The body is a TS 29.122 `BdtPolicy` with two members.

| Member | Required | Description |
|---|---|---|
| `bdtPolData` | **Required** on `POST` and `PUT` | The BDT policy itself: the reference id and the transfer policies. Missing it is a `400`. |
| `bdtReqData` | Optional | The originating BDT request: ASP, UE count, volume and desired interval. |

### `bdtPolData` — `BdtPolicyData`

| Field | Type | Required | Description |
|---|---|---|---|
| `bdtRefId` | string | **Required** | BDT reference identifier, as used by the subscriptions that will carry the transfer. Absent, this fails JSON deserialization and returns `400`. |
| `transfPolicies` | array of `TransferPolicy` | **Required**, at least one element | The candidate transfer policies. An empty array is a `422`. |
| `selTransPolicyId` | integer | Optional | The `transPolicyId` of the policy the AF has selected. Normally set later, with `PATCH`. |
| `suppFeat` | string | Optional | Hex feature bitmask (TS 29.500 clause 6.6). |

### `transfPolicies[]` — `TransferPolicy`

| Field | Type | Required | Description |
|---|---|---|---|
| `transPolicyId` | integer | **Required** | Index identifying this policy within `transfPolicies`; the value quoted in `selTransPolicyId`. |
| `ratingGroup` | integer | **Required** | Charging rating group applied to traffic under this policy. |
| `recTimeInt` | object | **Required** | The recommended time window: `startTime` and `stopTime`, both RFC 3339. Both members are mandatory. |
| `maxBitRateDl` | string | Optional | Maximum downlink bit rate during the window, as a TS 29.571 BitRate string, for example `"50 Mbps"`. |
| `maxBitRateUl` | string | Optional | Maximum uplink bit rate during the window. |

### `bdtReqData` — `BdtReqData`

When `bdtReqData` is present, `aspId`, `desTimeInt`, `numOfUes` and `volPerUe` are all mandatory
within it; omitting any of them fails deserialization and returns `400`.

| Field | Type | Required | Description |
|---|---|---|---|
| `aspId` | string | **Required** | Identifier of the ASP requesting the transfer. |
| `desTimeInt` | object | **Required** | Desired time interval: `startTime` and `stopTime`, RFC 3339, both mandatory. |
| `numOfUes` | integer | **Required** | Expected number of UEs that will perform the transfer. |
| `volPerUe` | object | **Required** | `UsageThreshold`. All of its members are optional: `totalVolume`, `uplinkVolume`, `downlinkVolume` (bytes) and `duration` (seconds). |
| `dnn` | string | Optional | Target DNN. |
| `snssai` | object | Optional | S-NSSAI of the target slice: `{"sst": 1, "sd": "010203"}`. |
| `interGroupId` | string | Optional | External group id (TS 23.003) scoping the transfer to a UE group. |
| `nwAreaInfo` | object | Optional | Network area (TAI / cell / NG-RAN node lists) scoping the request. |
| `trafficDes` | string | Optional | Traffic descriptor per TS 24.526. |
| `notifUri` | string (URI) | Optional | Callback URI. Forwarded to PCF; see [Notifications](#notifications) for what this release does with it. |
| `warnNotifReq` | boolean | Optional | Whether BDT warning notifications are requested. |
| `suppFeat` | string | Optional | Hex feature bitmask. |

---

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/3gpp-bdt/v1/{scsAsId}/bdtPolicies` | Register a BDT policy and create it in PCF |
| `GET` | `/3gpp-bdt/v1/{scsAsId}/bdtPolicies` | List the BDT policies owned by this AF |
| `GET` | `/3gpp-bdt/v1/{scsAsId}/bdtPolicies/{bdtPolicyId}` | Read one BDT policy |
| `PUT` | `/3gpp-bdt/v1/{scsAsId}/bdtPolicies/{bdtPolicyId}` | Replace a BDT policy |
| `PATCH` | `/3gpp-bdt/v1/{scsAsId}/bdtPolicies/{bdtPolicyId}` | Change part of a BDT policy, typically to set `selTransPolicyId` |
| `DELETE` | `/3gpp-bdt/v1/{scsAsId}/bdtPolicies/{bdtPolicyId}` | Delete a BDT policy |

### POST — register a BDT policy

Call this when the ASP has a transfer to schedule. NEF validates the body, stores it, and creates
the policy in PCF over `Npcf_BDTPolicyControl`.

**PCF answers a successful BDT create with `303 See Other`**, and NEF treats that as success
alongside `2xx`. It then extracts the PCF-side policy identifier — from the last path segment of
the `Location` header first, then from the body's `bdtPolicyId`, `bdtRefId` or
`bdtPolData.bdtRefId` — and stores the mapping. The `303` is never propagated northbound: the AF
sees `201 Created`.

If PCF fails, the local record is rolled back and the request fails with `502`. Unlike `PUT` and
`PATCH`, `POST` answers `502` even when the PCF call timed out; it does not map a timeout to `504`.

`POST` is the one operation NEF does not restrict to a path-segment match: it is accepted on both
`/bdtPolicies` and the deprecated `/policies`.

### GET — list or read

The collection `GET` returns every BDT policy owned by `{scsAsId}` as an array, or `[]` when there
are none. The resource `GET` returns a single policy. Both add the top-level `bdtRefId`.

Both are served from NEF's in-memory state and do not query PCF, so they report what NEF registered,
not what PCF is currently enforcing.

### PUT — replace a BDT policy

Call this when the transfer job itself changes: a different volume estimate, a different UE count,
a different set of candidate windows.

The stored policy is replaced by the body you send, so re-supply everything, `bdtPolData` included.
NEF pushes the new policy to PCF first and overwrites its own copy only on success, so a PCF failure
leaves the previous policy intact.

**Response `200 OK`**: the replacement policy. The `PUT` response does **not** carry the top-level
`bdtRefId` — you already have it, it is in the URL.

```bash
curl --http2-prior-knowledge \
  -X PUT http://oai-nef:8080/3gpp-bdt/v1/af-1/bdtPolicies/7a3c1e9f-2b44-4d1e-9c8a-0f5b2d6e1a73 \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "bdtPolData": {
      "bdtRefId": "BDT-REF-2026-00042",
      "transfPolicies": [
        {
          "transPolicyId": 1,
          "ratingGroup": 10,
          "recTimeInt": {"startTime": "2026-04-28T01:00:00Z", "stopTime": "2026-04-28T05:00:00Z"},
          "maxBitRateDl": "200 Mbps",
          "maxBitRateUl": "40 Mbps"
        }
      ]
    },
    "bdtReqData": {
      "aspId": "asp-firmware-1",
      "numOfUes": 750,
      "volPerUe": {"uplinkVolume": 104857600, "downlinkVolume": 1073741824, "totalVolume": 1178599424},
      "desTimeInt": {"startTime": "2026-04-28T01:00:00Z", "stopTime": "2026-04-28T05:00:00Z"}
    }
  }'
```

### PATCH — change part of a BDT policy

This is how an AF confirms its choice of transfer policy. Set `bdtPolData.selTransPolicyId` to the
`transPolicyId` of the window it will use.

Semantics are JSON Merge Patch (RFC 7396) applied to the stored resource; send
`Content-Type: application/merge-patch+json`. NEF merges the patch locally, sends the **merged**
policy to PCF, and only then re-parses and re-validates the merged document. That ordering matters:
a patch that leaves the resource invalid — removing `transfPolicies`, for instance — has already
reached PCF by the time it is rejected northbound with `422`.

`PATCH` requires the canonical `/bdtPolicies` path. On `/policies` it is `405`.

**Response `200 OK`**: the merged resource with the top-level `bdtRefId`.

### DELETE — delete a BDT policy

Call this once the transfer campaign has finished. NEF asks PCF to delete the BDT policy, then
removes its own record.

The southbound delete is best-effort: a PCF failure is logged and the AF still gets `204`. A `204`
means "NEF no longer holds this policy", not "PCF has confirmed the removal". The operation is
idempotent; a second call answers `404`.

`403` and `404` from `DELETE` have **empty bodies** — no ProblemDetails. Branch on the status code.

---

## Notifications

`bdtReqData.notifUri` and `bdtReqData.warnNotifReq` are carried to PCF as part of the policy, but
this release delivers no BDT notifications northbound. BDT policies are not registered in NEF's
subscription store or in its southbound-to-northbound notification map, so a PCF BDT notification
arriving at `/nef-notify/v1/notify/` has nothing to resolve against and is dropped with a warning.

Do not build an AF flow that waits for a BDT warning notification from this NEF.

---

## Lifetime

BDT policies live in NEF's process memory until the AF deletes them. There is no expiry timer for
BDT: a policy whose transfer window has long passed still answers `GET` and still needs an explicit
`DELETE`.

A NEF restart loses every BDT policy. After a restart, `GET` on the collection returns `[]` and the
AF must register its policies again.

---

## Southbound behaviour

| AF operation | NEF → PCF (`Npcf_BDTPolicyControl`) | On southbound failure |
|---|---|---|
| `POST` | create BDT policy | `502` and full rollback. `303 See Other` counts as success. A timeout is also `502` here, not `504`. |
| `PUT` | update BDT policy | `502`, or `504` on a timeout. The stored policy is left unchanged. |
| `PATCH` | update BDT policy with the merged body | `502`, or `504` on a timeout. The stored policy is left unchanged. |
| `DELETE` | delete BDT policy | Logged only; the AF still gets `204`. |

`PUT` and `PATCH` both need the PCF-side policy identifier that `POST` recorded. If it is missing —
because the PCF `201`/`303` carried neither a usable `Location` nor a recognisable id in its body —
the update fails `502` with `"Missing PCF BDT policy identifier"` without any southbound call being
made. Recovering means deleting the policy and registering it again.

---

## Error responses

| Status | Title | What happened | What to do |
|---|---|---|---|
| `400` | Bad Request | Body is not valid JSON; `bdtPolData` is absent; a mandatory member of a nested model is missing (`bdtPolData.bdtRefId`, `transfPolicies[].ratingGroup` / `.recTimeInt` / `.transPolicyId`, `bdtReqData.aspId` / `.desTimeInt` / `.numOfUes` / `.volPerUe`, `recTimeInt.startTime` / `.stopTime`). | Fix the body; `detail` names the missing member. Do not retry unchanged. |
| `403` | Forbidden | Authorization failed: no or invalid JWT, `{scsAsId}` not matching the `sub` claim, AF absent from the whitelist, or its `allowed_apis` not listing `nnef-bdt`. On a resource endpoint it can also mean the policy belongs to a different AF. | Check the token and the `{scsAsId}` in the path. NEF never answers `401`, so `403` is the single authentication-and-authorization outcome. On `DELETE` the body is empty. |
| `404` | Not Found | No BDT policy with that id for this AF. | Re-list with the collection `GET`. `404` after a NEF restart is expected: register the policy again. |
| `405` | Method Not Allowed | The method is not routed on that path — most often `PATCH` on the deprecated `/policies`, or a method that needs a `{bdtPolicyId}` used on the collection. | Switch to `/bdtPolicies`, or correct the method. |
| `422` | Unprocessable Entity | `transfPolicies` is empty; `{scsAsId}` exceeds 256 characters; or, on `PATCH`, the merged document failed model validation. | Fix the field named in `detail`. After a `422` on `PATCH`, read the policy back: the merged body was already sent to PCF even though NEF rejected it. |
| `429` | Too Many Requests | The token-bucket rate limit for this caller was reached. The bucket is keyed on the bearer token, or on the peer address when no token is presented. | Back off exponentially. Register BDT policies steadily rather than in a burst. |
| `503` | Service Unavailable | NEF is draining for shutdown (`detail` is `"Server is draining"`), or the request dispatcher queue is full (`"Server is overloaded, please retry later"`). | Retry later, or send it to another NEF instance. Nothing was registered. |
| `502` | Bad Gateway | PCF could not be reached or rejected the BDT policy create/update, or the stored PCF policy identifier is missing. | Nothing was registered (on `POST`) or changed (on `PUT`/`PATCH`). Retry after a short delay; if it persists, check PCF reachability and NRF registration. |
| `504` | Gateway Timeout | The PCF call on a `PUT` or `PATCH` timed out (a southbound `408` maps to `504`). | The outcome at PCF is unknown. Read the policy back with `GET` before retrying. |

A body larger than 1 MiB produces no status code at all: the HTTP/2 layer resets the stream while
the body is still arriving, so the client sees `RST_STREAM` rather than a `4xx`.

**`400` on a missing member**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Missing required field: bdtPolData"
}
```

**`422` on an empty `transfPolicies`**

```json
{
  "type": "about:blank",
  "title": "Unprocessable Entity",
  "status": 422,
  "detail": "Validation failed: BdtPolicyData.transfPolicies: must have at least 1 elements;"
}
```

**`502` on a PCF failure**

```json
{
  "type": "about:blank",
  "title": "Bad Gateway",
  "status": 502,
  "detail": "Failed to create BDT policy in PCF"
}
```

**`504` on a PCF timeout during `PUT` or `PATCH`.** The title in this release is still
`"Bad Gateway"` — match on `status`, not on `title`:

```json
{
  "type": "about:blank",
  "title": "Bad Gateway",
  "status": 504,
  "detail": "Failed to update BDT policy in PCF"
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
- [Traffic Influence API](traffic-influence.md) — DNAI steering, the other `Npcf`-backed service
- [QoS Monitoring API](qos-monitoring.md) — per-flow guaranteed QoS sessions
