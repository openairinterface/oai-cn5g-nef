# Nnef_PFDmanagement SBI API (TS 29.551)

> **Audience note**: This is the **SBI (Service-Based Interface)** version of the PFD
> Management API, intended for use by internal 5G Core NFs (e.g., SMF, PCF) that consume PFD
> data from NEF over the SBI. External Application Functions (AFs) must use the
> [T8 PFD Management API](pfd-management.md) instead. If you are an AF developer, this page
> does not apply to you.

Read the [API Reference Overview](overview.md) first for authentication, HTTP/2 requirements,
and error format conventions that apply to all NEF APIs.

---

## Overview

The `Nnef_PFDmanagement` service interface provides internal 5GC NFs with direct access to the
PFD data managed by NEF. It mirrors the T8 PFD Management data model but uses the SBI path
prefix and is intended for machine-to-machine NF communication within the operator's 5GC.

This API is defined by **3GPP TS 29.551 §5.2** (Service-Based Interface, Nnef reference point).

Available capabilities:
- Full CRUD on PFD transactions (same structure as T8).
- Full CRUD on individual application PFDs within a transaction.
- **Bulk operations**: fetch all applications across all transactions, with optional filtering.
- **Partial pull**: selective retrieval of PFD data for a specified list of application IDs.

---

## Base Path

```
/nnef-pfdmanagement/v1
```

---

## Endpoints

### Transaction CRUD

#### GET `/transactions` — List All PFD Transactions

Retrieve all PFD transactions stored in NEF, across all AF owners.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions
```

**Response (200 OK)**: Array of transaction objects.

```json
[
  {
    "transId": "txn-2026-001",
    "externalAppIds": ["netflix", "youtube"],
    "pfds": [
      {
        "externalAppId": "netflix",
        "pfdContent": {
          "pfds": [
            {
              "pfdId": "pfd-netflix-1",
              "flowDescriptions": ["permit out 6 from 198.38.96.0/19 to assigned"],
              "domainNames": ["netflix.com", "nflxvideo.net"]
            }
          ]
        }
      }
    ]
  }
]
```

---

#### PUT `/transactions/{transId}` — Create or Update Transaction

Create a new PFD transaction or fully replace an existing one. The operation follows the same
**atomic batch** semantics as the T8 interface (see [Atomicity
Guarantee](pfd-management.md#atomicity-guarantee)).

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "externalAppIds": ["spotify"],
           "pfds": [
             {
               "externalAppId": "spotify",
               "pfdContent": {
                 "pfds": [
                   {
                     "pfdId": "pfd-spotify-1",
                     "domainNames": ["spotify.com", "scdn.co", "akamaized.net"],
                     "urls": ["https://api.spotify.com/*", "https://*.scdn.co/*"]
                   }
                 ]
               }
             }
           ]
         }' \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001
```

**Response — 201 Created** (new transaction) or **200 OK** (replaced). Body: stored transaction
object.

```json
{
  "transId": "txn-internal-001",
  "externalAppIds": ["spotify"],
  "pfds": [
    {
      "externalAppId": "spotify",
      "pfdContent": {
        "pfds": [
          {
            "pfdId": "pfd-spotify-1",
            "domainNames": ["spotify.com", "scdn.co", "akamaized.net"],
            "urls": ["https://api.spotify.com/*", "https://*.scdn.co/*"]
          }
        ]
      }
    }
  ]
}
```

---

#### GET `/transactions/{transId}` — Get Transaction

Retrieve a single PFD transaction by its identifier.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001
```

**Response (200 OK)**: Single transaction object (same schema as above).

---

#### DELETE `/transactions/{transId}` — Delete Transaction

Delete a transaction and all PFDs it contains. NEF issues `DELETE` to UDR for each application
before removing the record.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001
```

**Response — 204 No Content** (empty body).

---

### Application CRUD

#### GET `/transactions/{transId}/applications/{appId}` — Get Application PFD

Retrieve the PFD data for a single application within a transaction.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001/applications/spotify
```

**Response (200 OK)**

```json
{
  "externalAppId": "spotify",
  "pfds": [
    {
      "pfdId": "pfd-spotify-1",
      "domainNames": ["spotify.com", "scdn.co", "akamaized.net"],
      "urls": ["https://api.spotify.com/*", "https://*.scdn.co/*"]
    }
  ]
}
```

---

#### PUT `/transactions/{transId}/applications/{appId}` — Create or Update Application PFD

Write or fully replace the PFD data for a single application. NEF writes to UDR for this
application only.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "externalAppId": "spotify",
           "pfds": [
             {
               "pfdId": "pfd-spotify-1",
               "domainNames": [
                 "spotify.com",
                 "scdn.co",
                 "akamaized.net",
                 "audio-ak.spotify.com"
               ],
               "urls": [
                 "https://api.spotify.com/*",
                 "https://*.scdn.co/*",
                 "https://audio-ak.spotify.com/*"
               ]
             }
           ]
         }' \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001/applications/spotify
```

**Response — 200 OK**: Updated application PFD object.

---

#### DELETE `/transactions/{transId}/applications/{appId}` — Delete Application PFD

Delete the PFD for a single application. The parent transaction is not affected.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001/applications/spotify
```

**Response — 204 No Content** (empty body).

---

### Bulk Operations

#### GET `/applications` — List All Applications Across All Transactions

Returns PFD data for all applications stored in NEF, regardless of which transaction they
belong to. Supports optional query-parameter filtering.

**Query Parameters**

| Parameter | Type | Description |
|---|---|---|
| `appIds` | string (comma-separated) | Optional. Return only the listed application IDs. When omitted, all applications are returned. |

**curl Example — All applications**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/applications
```

**curl Example — Filtered by appId**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     "http://oai-nef:8080/nnef-pfdmanagement/v1/applications?appIds=netflix,youtube"
```

**Response (200 OK)**: Array of application PFD objects.

```json
[
  {
    "externalAppId": "netflix",
    "pfds": [
      {
        "pfdId": "pfd-netflix-1",
        "flowDescriptions": ["permit out 6 from 198.38.96.0/19 to assigned"],
        "domainNames": ["netflix.com", "nflxvideo.net"]
      }
    ]
  },
  {
    "externalAppId": "youtube",
    "pfds": [
      {
        "pfdId": "pfd-youtube-1",
        "domainNames": ["youtube.com", "googlevideo.com", "ytimg.com"]
      }
    ]
  }
]
```

---

#### POST `/applications/partial-pull` — Selective Fetch of Application PFDs

Retrieve PFD data for a specific set of application IDs in a single request. Useful when an
NF (e.g., SMF at PDU session setup) needs PFDs for a known subset of applications without
fetching the entire PFD database.

**Request Body**

| Field | Type | Required | Description |
|---|---|---|---|
| `appIds` | array[string] | Required | List of application IDs for which PFD data is requested. |

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "appIds": ["netflix", "youtube"]
         }' \
     http://oai-nef:8080/nnef-pfdmanagement/v1/applications/partial-pull
```

**Response (200 OK)**: Array of PFD objects for the requested applications.

```json
[
  {
    "externalAppId": "netflix",
    "pfds": [
      {
        "pfdId": "pfd-netflix-1",
        "flowDescriptions": ["permit out 6 from 198.38.96.0/19 to assigned"],
        "domainNames": ["netflix.com", "nflxvideo.net"]
      }
    ]
  },
  {
    "externalAppId": "youtube",
    "pfds": [
      {
        "pfdId": "pfd-youtube-1",
        "domainNames": ["youtube.com", "googlevideo.com", "ytimg.com"]
      }
    ]
  }
]
```

If an application ID in `appIds` does not exist in NEF's PFD store, it is silently omitted
from the response array. The caller should check that all requested IDs are present in the
response.

---

## Error Responses

| Status | Title | Typical Cause |
|---|---|---|
| `400 Bad Request` | Bad Request | Missing required field; empty `appIds` array in partial-pull; invalid JSON; body exceeds 1 MiB. |
| `401 Unauthorized` | Unauthorized | Missing or invalid JWT; expired token. |
| `403 Forbidden` | Forbidden | Caller is not authorized for the `nnef_pfd_management` API. |
| `404 Not Found` | Not Found | Transaction ID or application ID does not exist. |
| `429 Too Many Requests` | Too Many Requests | Global token-bucket rate limit exceeded; apply exponential back-off. |
| `503 Service Unavailable` | Service Unavailable | Circuit breaker for UDR is in the OPEN state. |

**Example — 400 Bad Request (empty appIds)**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Field 'appIds' must be a non-empty array"
}
```

**Example — 404 Not Found**

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "Transaction 'txn-internal-999' not found"
}
```

**Example — 503 Service Unavailable**

```json
{
  "type": "about:blank",
  "title": "Service Unavailable",
  "status": 503,
  "detail": "Circuit breaker OPEN for UDR; downstream NF is unreachable"
}
```
