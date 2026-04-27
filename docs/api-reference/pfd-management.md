# PFD Management API — T8 Interface (TS 29.551)

This page is the complete reference for the NEF PFD Management northbound T8 API. Read the
[API Reference Overview](overview.md) first for authentication, HTTP/2 requirements, and error
format conventions that apply to all NEF APIs.

---

## Overview

**Packet Flow Description (PFD) management** allows external AFs and SCS/AS entities to
provision traffic classification descriptors into the 5G Core Network. These descriptors enable
the network — specifically the UPF (User Plane Function) — to recognise and classify
application-specific traffic flows by IP 5-tuple, URL pattern, or domain name, without
relying on deep packet inspection beyond what the AF's PFD rules specify.

This API is defined by **3GPP TS 29.551** (T8 reference point) and provides:

- A **Transaction API** for batched, atomic creation and management of groups of application PFDs.
- An **Application API** for per-application CRUD and partial update operations within a transaction.

NEF persists each PFD to UDR via the `Nudr_DataRepository` service interface
(`PUT /nudr-dr/v2/application-data/pfds/{appId}`). UPF nodes retrieve PFDs from UDR as needed.

---

## Atomicity Guarantee

PFD transaction operations are **atomic**: when a `PUT /{scsAsId}/transactions/{transId}`
request contains multiple application PFDs, either all of them are committed to UDR or none
are. This prevents a partial-write state where some applications have PFDs registered and
others do not.

**Rollback mechanism**: If UDR returns an error while writing the _n_-th application in a
batch, NEF immediately issues `DELETE /nudr-dr/v2/application-data/pfds/{appId}` compensating
requests to UDR for every application that was successfully written in the same batch before
returning `400 Bad Request` or `500 Internal Server Error` to the AF. After rollback, no
application in that batch has a PFD registered in UDR.

> **Important**: Do not assume partial success for any transaction `PUT` that returns a 4xx or
> 5xx response. If the response is not `200` or `201`, no PFDs from the request body are active.

---

## Transaction API

### Base Path

```
/3gpp-pfd-management/v1/{scsAsId}/transactions
```

---

### GET `/{scsAsId}/transactions` — List All Transactions

Retrieve all PFD transactions created by the calling AF.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions
```

**Response (200 OK)**: Array of transaction objects.

```json
[
  {
    "self": "/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001",
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
      },
      {
        "externalAppId": "youtube",
        "pfdContent": {
          "pfds": [
            {
              "pfdId": "pfd-youtube-1",
              "domainNames": ["youtube.com", "googlevideo.com", "ytimg.com"]
            }
          ]
        }
      }
    ]
  }
]
```

---

### PUT `/{scsAsId}/transactions/{transId}` — Create or Update Transaction (Atomic Batch)

Create a new transaction or fully replace an existing one. The entire `pfds` array is processed
atomically: all applications are written to UDR, or none are (see [Atomicity
Guarantee](#atomicity-guarantee)).

**Path Parameters**

| Parameter | Description |
|---|---|
| `scsAsId` | SCS/AS identifier of the calling AF. |
| `transId` | Caller-assigned transaction identifier string (free-form, unique per AF). |

**Request Body**

| Field | Type | Required | Description |
|---|---|---|---|
| `externalAppIds` | array[string] | Required | List of application IDs included in this transaction. |
| `pfds` | array | Required | Array of per-application PFD objects (see below). |
| `pfds[*].externalAppId` | string | Required | Must match one of the values in `externalAppIds`. |
| `pfds[*].pfdContent` | object | Required | PFD content container. |
| `pfds[*].pfdContent.pfds` | array | Required | Array of individual PFD descriptors. |

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "externalAppIds": ["netflix", "youtube"],
           "pfds": [
             {
               "externalAppId": "netflix",
               "pfdContent": {
                 "pfds": [
                   {
                     "pfdId": "pfd-netflix-1",
                     "flowDescriptions": [
                       "permit out 6 from 198.38.96.0/19 to assigned",
                       "permit out 6 from 198.45.48.0/20 to assigned"
                     ],
                     "domainNames": ["netflix.com", "nflxvideo.net"]
                   }
                 ]
               }
             },
             {
               "externalAppId": "youtube",
               "pfdContent": {
                 "pfds": [
                   {
                     "pfdId": "pfd-youtube-1",
                     "domainNames": ["youtube.com", "googlevideo.com", "ytimg.com"]
                   }
                 ]
               }
             }
           ]
         }' \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001
```

**Response — 201 Created** (new transaction) or **200 OK** (existing transaction replaced).
Body echoes the stored transaction object with a `self` link.

---

### DELETE `/{scsAsId}/transactions/{transId}` — Delete Entire Transaction

Delete a transaction and all application PFDs it contains. NEF issues `DELETE` to UDR for
each application in the transaction before removing the transaction record.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001
```

**Response — 204 No Content** (empty body).

---

## Application API

### Base Path

```
/3gpp-pfd-management/v1/{scsAsId}/transactions/{transId}/applications
```

---

### GET `.../applications/{appId}` — Get PFD for One Application

Retrieve the PFD data for a single application within a transaction.

**Response (200 OK)**

```json
{
  "externalAppId": "netflix",
  "pfds": [
    {
      "pfdId": "pfd-netflix-1",
      "flowDescriptions": [
        "permit out 6 from 198.38.96.0/19 to assigned",
        "permit out 6 from 198.45.48.0/20 to assigned"
      ],
      "urls": [],
      "domainNames": ["netflix.com", "nflxvideo.net"]
    }
  ]
}
```

**curl Example**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/netflix
```

---

### PUT `.../applications/{appId}` — Create or Update PFD for One Application

Write or fully replace the PFD data for a single application within an existing transaction.
This operation is **not** part of the batch atomicity scope — only this single application is
written to UDR.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "externalAppId": "disney-plus",
           "pfds": [
             {
               "pfdId": "pfd-disney-1",
               "domainNames": ["disneyplus.com", "disney-plus.net", "bamgrid.com"],
               "urls": ["https://disneyplus.com/*"]
             }
           ]
         }' \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/disney-plus
```

**Response — 201 Created** (new) or **200 OK** (replaced). Body echoes the stored application
PFD object.

---

### PATCH `.../applications/{appId}` — Partial Update (JSON Merge Patch)

Partially update PFD data for one application. Uses **JSON Merge Patch** semantics (RFC 7396):
include only the fields to change. Set a field to `null` to clear it.

**curl Example** — Add a new domain name to an existing PFD:

```bash
curl --http2-prior-knowledge \
     -X PATCH \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/merge-patch+json" \
     -d '{
           "pfds": [
             {
               "pfdId": "pfd-disney-1",
               "domainNames": ["disneyplus.com", "disney-plus.net", "bamgrid.com", "cdn.disneystreaming.com"]
             }
           ]
         }' \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/disney-plus
```

**Response — 200 OK**: Full merged application PFD object.

---

### DELETE `.../applications/{appId}` — Delete PFD for One Application

Delete the PFD for a single application within a transaction. NEF issues `DELETE` to UDR for
this application only. The parent transaction record remains.

**curl Example**

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/disney-plus
```

**Response — 204 No Content** (empty body).

---

## PFD Content Fields

The `pfds` array within `pfdContent` (or within the application object) contains one or more
PFD descriptor objects. Each descriptor must include at least one of the optional classification
fields.

| Field | Type | Required | Description |
|---|---|---|---|
| `pfdId` | string | Required | Unique identifier for this PFD within the application. Used to reference and update individual descriptors. |
| `flowDescriptions` | array[string] | Optional | 5-tuple IP flow filter strings in IPFilterRule syntax (e.g., `"permit out 6 from 203.0.113.0/24 443 to assigned"`). |
| `urls` | array[string] | Optional | URL patterns for HTTP/HTTPS traffic classification (e.g., `"https://example.com/api/*"`). |
| `domainNames` | array[string] | Optional | Fully qualified domain name patterns for DNS-based classification (e.g., `"example.com"`, `"*.cdn.example.net"`). |

At least one of `flowDescriptions`, `urls`, or `domainNames` must be non-empty per PFD
descriptor.

---

## Atomicity Rollback Behavior

When `PUT /{scsAsId}/transactions/{transId}` processes a batch of applications and a UDR write
fails for one application (for example, the _n_-th application in the batch):

1. NEF records the error from UDR.
2. For each application that was **already successfully written** to UDR in this batch (indices
   1 through _n_−1), NEF sends a compensating `DELETE /nudr-dr/v2/application-data/pfds/{appId}`
   request to UDR.
3. After all compensating deletes complete (best-effort), NEF returns an error response to the AF.

**Error response for rollback scenario (500 Internal Server Error)**:

```json
{
  "type": "about:blank",
  "title": "Internal Server Error",
  "status": 500,
  "detail": "Transaction 'txn-2026-001' failed on application 'youtube': UDR write error. Rollback completed for 1 previously committed application(s)."
}
```

After a rollback, the transaction may still exist in NEF's in-memory state as an empty or
partial record. The AF should issue `DELETE /{scsAsId}/transactions/{transId}` to clean up and
then retry the `PUT` with the corrected payload.

---

## Southbound Behavior

Each PFD application object is stored in UDR via:

```
PUT /nudr-dr/v2/application-data/pfds/{appId}
```

where `{appId}` is the `externalAppId` value. NEF translates the T8 PFD format to the UDR data
model before writing. On delete (single application or full transaction), NEF issues the
corresponding `DELETE` to UDR.

UPF nodes retrieve PFD data from UDR via SMF at PDU session establishment or PFD refresh
intervals. NEF does not push PFDs directly to UPF.

---

## Error Responses

| Status | Title | Typical Cause |
|---|---|---|
| `400 Bad Request` | Bad Request | Missing required field; invalid `pfdId` format; `externalAppId` not listed in `externalAppIds`; body exceeds 1 MiB. |
| `401 Unauthorized` | Unauthorized | Missing or invalid JWT; expired token. |
| `403 Forbidden` | Forbidden | `scsAsId` does not match the JWT `sub` claim; `allowed_apis` restriction. |
| `404 Not Found` | Not Found | Transaction ID or application ID does not exist. |
| `429 Too Many Requests` | Too Many Requests | Global token-bucket rate limit exceeded. |
| `500 Internal Server Error` | Internal Server Error | UDR write failure during batch; rollback attempted. |
| `503 Service Unavailable` | Service Unavailable | Circuit breaker for UDR is in the OPEN state. |

**Example — 400 Bad Request**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "PFD descriptor 'pfd-1' must include at least one of: flowDescriptions, urls, domainNames"
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

---

See [Call Flows](../call-flows.md) for the complete end-to-end sequence diagram including the
atomic rollback flow.
