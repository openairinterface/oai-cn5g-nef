<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# PFD Management API — T8 Interface (TS 29.551)

## What this API is for

A **Packet Flow Description (PFD)** tells the 5G Core how to recognise one application's
traffic: by IP 5-tuple, by URL pattern, or by domain name. An AF / SCS-AS that owns an
application — a video service, a game backend — uses this API to publish those descriptors
into the core so that the network can classify the application's flows and apply the right
policy to them.

NEF stores each application's PFD in the UDR (`Nudr_DataRepository`). SMF and UPF read PFDs
from UDR at PDU session establishment and at PFD refresh; NEF never pushes PFDs to UPF
directly and is not on the data path.

Descriptors are grouped into **transactions**. A transaction is a named batch of
application PFDs owned by one SCS/AS, and writing one is all-or-nothing: either every
application in the batch reaches UDR, or none does.

Read the [API Reference Overview](overview.md) first for authentication, HTTP/2
requirements, and the error format shared by all NEF APIs.

If you are an internal 5GC NF (SMF, PCF) rather than an external AF, use the
[Nnef_PFDmanagement SBI API](nnef-pfd-management.md) instead — same data, different
reference point and a different request shape.

---

## The happy path

Create a transaction holding two applications, read one back, then delete the whole thing.

**1. Create the transaction.** The body has exactly one top-level field, `pfdDatas`: an
**object keyed by external application id**, not an array.

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "pfdDatas": {
             "app-video": {
               "applicationId": "app-video",
               "pfds": [
                 {
                   "pfdId": "pfd-video-1",
                   "flowDescriptions": [
                     "permit out 6 from 198.51.100.0/24 to assigned",
                     "permit out 6 from 203.0.113.0/24 to assigned"
                   ],
                   "domainNames": ["video.example.com", "cdn.example.net"]
                 }
               ]
             },
             "app-chat": {
               "applicationId": "app-chat",
               "pfds": [
                 {
                   "pfdId": "pfd-chat-1",
                   "urls": ["https://chat.example.com/*"]
                 }
               ]
             }
           }
         }' \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001
```

`201 Created` on a new transaction, `200 OK` when it replaced an existing one. The body is
the request body echoed back with `transId` added:

```json
{
  "pfdDatas": { "app-video": { "...": "..." }, "app-chat": { "...": "..." } },
  "transId": "txn-2026-001"
}
```

**2. Read one application back.**

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/app-chat
```

```json
{
  "applicationId": "app-chat",
  "pfds": [
    { "pfdId": "pfd-chat-1", "urls": ["https://chat.example.com/*"] }
  ],
  "appId": "app-chat"
}
```

**3. Delete the transaction.** This removes every application it holds from UDR.

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001
```

`204 No Content`, empty body.

---

## Base path

```
/3gpp-pfd-management/v1/{scsAsId}/transactions[/{transId}[/applications/{appId}]]
```

| Parameter | Description |
|---|---|
| `scsAsId` | SCS/AS identifier of the calling AF. Checked against the JWT `sub` claim or the AF whitelist, and used as the owner of every transaction created under it. |
| `transId` | Caller-assigned transaction identifier. Free-form; unique per NEF instance, not per AF. |
| `appId` | External application identifier. This is the key UDR stores the PFD under, and it is global across NEF. |

A path whose second segment is not the literal `transactions` is answered `404 Not Found`
with the detail `"Requested resource was not found"`.

All transaction state is in memory. A NEF restart loses every transaction record, while
the PFDs already written to UDR survive — after a restart, NEF no longer knows they exist.

---

## Endpoints

| Method | Path | What it does |
|---|---|---|
| `GET` | `/{scsAsId}/transactions` | List this SCS/AS's transactions |
| `PUT` | `/{scsAsId}/transactions/{transId}` | Create or replace a transaction as an atomic batch |
| `GET` | `/{scsAsId}/transactions/{transId}` | Returns the whole transaction list — see the note below |
| `DELETE` | `/{scsAsId}/transactions/{transId}` | Delete the transaction and every PFD in it |
| `GET` | `/{scsAsId}/transactions/{transId}/applications/{appId}` | Read one application's PFD |
| `PUT` | `/{scsAsId}/transactions/{transId}/applications/{appId}` | Create or replace one application's PFD |
| `PATCH` | `/{scsAsId}/transactions/{transId}/applications/{appId}` | Merge-patch one application's PFD |
| `DELETE` | `/{scsAsId}/transactions/{transId}/applications/{appId}` | Delete one application's PFD |

`POST` is not supported anywhere on this API: creating a transaction is a `PUT` to the
identifier you choose. A `POST` to the collection, or any other unsupported method, is
answered `405 Method Not Allowed` with the detail
`"HTTP method is not supported for this resource"`.

> **Current implementation limitation — `GET .../transactions/{transId}`.** The item read
> is wired to the same handler as the collection read. It **ignores `{transId}` entirely**
> and returns the full array of this SCS/AS's transactions, exactly as
> `GET /{scsAsId}/transactions` does. It never returns `404` for an unknown id, because it
> never looks the id up. Do not use it to check whether a transaction exists — read the
> list and match on `transId` yourself, or read one of its applications.

---

## Atomicity guarantee

A `PUT /{scsAsId}/transactions/{transId}` carrying several applications is committed as one
batch. NEF writes each application to UDR in turn. Only after **all** of them succeed does
it commit the transaction to its own state and answer `200`/`201`.

**Rollback.** If the write for the _n_-th application fails, NEF issues compensating
`DELETE`s to UDR for applications 1 … _n_−1, **in reverse order**, and then answers
`500 Internal Server Error`. Two consequences matter to the caller:

- **Nothing partial is left in NEF.** Local state is committed after the last successful
  UDR write, so a failed `PUT` commits none of it. If the transaction already existed, its
  previous contents are untouched and still current.
- **UDR may still hold orphans.** The compensating deletes are themselves best-effort. If
  one of them fails too, NEF logs it and still answers `500`. Reissue the `PUT`, or delete
  the transaction, once UDR is healthy again.

> Do not assume partial success for a transaction `PUT` that returns `4xx` or `5xx`.
> If the response is not `200` or `201`, treat the whole batch as not applied — and if the
> transaction existed before, as unchanged.

Single-application `PUT` and `PATCH` are outside this scope: they write exactly one
application, and their UDR result is best-effort (see below).

---

## PUT — create or replace a transaction

This is the main write operation. Call it to publish a coherent set of application PFDs in
one shot, and call it again with the full set to change any of them.

**Request body**

| Field | Type | Required | Description |
|---|---|---|---|
| `pfdDatas` | object | Required | Map of external application id → PFD data object. Missing → `400`; present but not an object → `422`. An empty object is accepted and produces a transaction with no applications. |
| `pfdDatas.<appId>.applicationId` | string | Required | The application identifier. Absent → `400` naming the offending key. NEF does not check that it equals `<appId>`; UDR is written under the map key. |
| `pfdDatas.<appId>.pfds` | array | Optional | Array of PFD descriptors — see [PFD descriptor fields](#pfd-descriptor-fields). If present it must hold at least one element, or the request is `422`. |
| `pfdDatas.<appId>.cachingTime` | string (date-time) | Optional | Passed through to UDR unchanged. |
| `pfdDatas.<appId>.cachingTimer` | integer | Optional | Passed through to UDR unchanged. |
| `pfdDatas.<appId>.pfdTimestamp` | string (date-time) | Optional | Passed through to UDR unchanged. |
| `pfdDatas.<appId>.partialFlag` | boolean | Optional | Passed through to UDR unchanged. |
| `pfdDatas.<appId>.supportedFeatures` | string | Optional | Passed through to UDR unchanged. |

**Responses**

| Status | Meaning |
|---|---|
| `201 Created` | The `{transId}` did not exist. The body is the request body with `transId` added. |
| `200 OK` | The `{transId}` existed and was replaced. Same body shape. |

There is no `Location` header and no `self` field on this API.

**Replacement semantics.** A `PUT` replaces the transaction's application set wholesale in
NEF. An application that was in the previous version and is absent from the new body is
dropped from the local record, but **its PFD is not deleted from UDR** — the T8 path has no
removed-application cleanup. Delete it explicitly with
`DELETE .../applications/{appId}` before shrinking a transaction, or delete and recreate the
whole transaction.

---

## GET — list transactions

Call this to enumerate what the calling SCS/AS currently owns — the only reliable way to
discover transaction ids after an AF restart.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions
```

**Response — 200 OK.** An array. Transactions owned by other SCS/AS identities are filtered
out; an empty result is `[]`. Each entry carries `transId` and `pfdDatas`, the same map
shape used on write:

```json
[
  {
    "transId": "txn-2026-001",
    "pfdDatas": {
      "app-video": {
        "applicationId": "app-video",
        "pfds": [
          {
            "pfdId": "pfd-video-1",
            "flowDescriptions": ["permit out 6 from 198.51.100.0/24 to assigned"],
            "domainNames": ["video.example.com", "cdn.example.net"]
          }
        ]
      }
    }
  }
]
```

The list is served from NEF's memory. UDR is not read, so a PFD changed in UDR by another
route is not reflected here.

---

## DELETE — delete a transaction

Call this to retire a whole batch of application PFDs. NEF removes the local record first,
then issues one `DELETE` to UDR per application it contained.

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001
```

**Response — 204 No Content.**

The southbound deletes are best-effort: the local record is already gone, so `204` is
returned even if UDR discovery fails or individual deletes are rejected. A `204` means NEF
has forgotten the transaction; it does not prove UDR is clean.

**Body exception on this method.** DELETE responses use an empty-body sink, so every status
here is a bare status line with no body and no content type: `204` on success, `403` on an
authorization or ownership failure, `404` on an unknown `{transId}`. Do not parse a body
from a failed DELETE. A `503` is the exception — it is produced before that sink is reached
and does carry Problem Details.

---

## GET — read one application's PFD

Call this to check what NEF holds for a single application — including, in practice, to
test whether a transaction exists, since the transaction item read does not.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/app-video
```

**Response — 200 OK.** The stored PFD data object with `appId` added:

```json
{
  "applicationId": "app-video",
  "pfds": [
    {
      "pfdId": "pfd-video-1",
      "flowDescriptions": [
        "permit out 6 from 198.51.100.0/24 to assigned",
        "permit out 6 from 203.0.113.0/24 to assigned"
      ],
      "domainNames": ["video.example.com", "cdn.example.net"]
    }
  ],
  "appId": "app-video"
}
```

Three distinct failures, each with a Problem Details body:

| Status | `detail` | Means |
|---|---|---|
| `404` | `PFD transaction not found` | No such `{transId}` in NEF. |
| `403` | `AF is not allowed to access this resource` | The transaction exists but another SCS/AS owns it. |
| `404` | `Application PFD not found in transaction` | The transaction exists and is yours; the application is not in it. |

Like the list, this is answered from memory. UDR is not read.

---

## PUT — create or replace one application's PFD

Call this to add an application to an existing transaction, or to replace one
application's descriptors without resending the rest of the batch.

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "applicationId": "app-news",
           "pfds": [
             {
               "pfdId": "pfd-news-1",
               "domainNames": ["news.example.com", "static.news.example.com"],
               "urls": ["https://news.example.com/*"]
             }
           ]
         }' \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/app-news
```

**Response — 201 Created** when the application was not previously in the transaction,
**200 OK** when it was. The body is the stored object with `appId` added.

The transaction must already exist and be owned by the caller (`404` / `403` as in the read
above). `applicationId` is required in the body — omitting it is a `400`.

**This write is not atomic and its UDR result is not reported.** NEF updates its own state
first and then fires one `PUT /nudr-dr/v1/application-data/pfds/{appId}` to UDR. A UDR
failure is logged as a warning and the AF still receives `200`/`201`. After a
`2xx` here, confirm the UDR side out of band if it matters; if it does matter, prefer the
transaction `PUT`, which is the only PFD write that reports UDR failure to the caller.

---

## PATCH — merge-patch one application's PFD

Call this for a small edit — adding a domain name, adjusting a caching timer — without
restating the application's whole PFD set.

The body is an RFC 7396 JSON Merge Patch: include only what changes, and set a member to
`null` to remove it.

```bash
curl --http2-prior-knowledge \
     -X PATCH \
     -H "Authorization: Bearer <your-jwt-token>" \
     -H "Content-Type: application/merge-patch+json" \
     -d '{
           "pfds": [
             {
               "pfdId": "pfd-news-1",
               "domainNames": [
                 "news.example.com",
                 "static.news.example.com",
                 "img.news.example.com"
               ],
               "urls": ["https://news.example.com/*"]
             }
           ]
         }' \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/app-news
```

**Response — 200 OK.** The full merged object with `appId` added.

Merge Patch replaces arrays outright rather than merging their elements, so a patch that
touches `pfds` must carry every descriptor you want to keep, each with all of its members.
That is why the example above repeats `urls`.

After merging, NEF re-parses and re-validates the result. A patch that produces an invalid
object is rejected with `422` and the detail `Patched body invalid: …`; the stored object is
left unchanged. As with the single-application `PUT`, the subsequent UDR write is
best-effort and its failure is not reported to the AF.

---

## DELETE — delete one application's PFD

Call this to withdraw one application while leaving the rest of the transaction in place.
The transaction record itself is not removed, even if this was its last application.

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-jwt-token>" \
     http://oai-nef:8080/3gpp-pfd-management/v1/my-af-1/transactions/txn-2026-001/applications/app-news
```

**Response — 204 No Content.** The local entry is erased and one best-effort
`DELETE /nudr-dr/v1/application-data/pfds/{appId}` is sent to UDR; a UDR failure does not
change the `204`.

The same empty-body rule as the transaction `DELETE` applies: `403` and `404` on this path
are bare status lines with no body.

---

## PFD descriptor fields

Each element of the `pfds` array describes one way to recognise the application's traffic.

| Field | Type | Description |
|---|---|---|
| `pfdId` | string | Identifier for this descriptor within the application. Optional in the model, but supply it — it is how you refer to the descriptor in a later `PATCH`. |
| `flowDescriptions` | array[string] | IPFilterRule-syntax 5-tuple filters, e.g. `"permit out 6 from 203.0.113.0/24 443 to assigned"`. If present, must hold at least one element. |
| `urls` | array[string] | URL patterns for HTTP/HTTPS classification, e.g. `"https://example.com/api/*"`. If present, must hold at least one element. |
| `domainNames` | array[string] | Domain name patterns for DNS-based classification, e.g. `"example.com"`. If present, must hold at least one element. |
| `dnProtocol` | string | Domain-name protocol qualifier, carried through to UDR. |

A descriptor is normally expected to carry at least one of `flowDescriptions`, `urls` or
`domainNames`. **Current implementation limitation:** NEF does not enforce that. A
descriptor with none of them, or an empty `pfds` array, is accepted and written to UDR,
where it classifies nothing. Validate this AF-side.

An **empty** `flowDescriptions`, `urls` or `domainNames` array is different from an absent
one and *is* rejected, with `422` and a detail of the form
`PfdDataForApp.pfds[0].pfds.urls: must have at least 1 elements`.

---

## Southbound behaviour

Every application PFD is stored in UDR at:

```
PUT    /nudr-dr/v1/application-data/pfds/{appId}
DELETE /nudr-dr/v1/application-data/pfds/{appId}
```

where `{appId}` is the external application identifier. NEF serialises its stored PFD data
object as the UDR request body.

Note the version: this build uses the UDR `v1` application-data path, not `v2`.

| Operation | UDR calls | Is a UDR failure reported to the AF? |
|---|---|---|
| Transaction `PUT` | One `PUT` per application, in map order | **Yes** — a failure rolls back and answers `500` |
| Transaction `DELETE` | One `DELETE` per application it held | No — always `204` |
| Application `PUT` / `PATCH` | One `PUT` | No — warning only, still `200`/`201` |
| Application `DELETE` | One `DELETE` | No — always `204` |
| Any `GET` | None | — |

`{appId}` is global in UDR. Two SCS/AS identities that publish the same external
application id write to the same UDR record and will overwrite each other. NEF scopes
ownership at the *transaction* level only, not at the application level.

SMF and UPF read PFDs from UDR at PDU session establishment and at their PFD refresh
interval. NEF does not push PFDs to UPF.

---

## Error responses

| Status | What happened | What the client should do |
|---|---|---|
| `400 Bad Request` | The body is not JSON; `pfdDatas` is absent; an application object is missing `applicationId` or has a member of the wrong JSON type. | Fix the body. Do not retry unchanged. A body over 1 MiB is not a `400` — the HTTP/2 layer resets the stream instead; see the [overview](overview.md). |
| `403 Forbidden` | Missing, invalid or expired JWT; `scsAsId` does not match the token's `sub`; the AF is not whitelisted; `allowed_apis` does not include `nnef-pfdmanagement`; or the transaction belongs to another SCS/AS. | Check the token and the AF's `allowed_apis`. NEF never emits `401` — every authorization failure is this `403`, with the detail `"AF not authorized for this service"`. Retrying will not help until configuration or token changes. |
| `404 Not Found` | Unknown `{transId}`, unknown `{appId}` within a transaction, or a path whose second segment is not `transactions`. | Check the id, or create the transaction first. On this API a lost transaction may mean NEF restarted. |
| `405 Method Not Allowed` | An unsupported method — `POST` anywhere, or `PATCH` on a transaction. | Use `PUT` to create a transaction; see the endpoint table. |
| `422 Unprocessable Entity` | `pfdDatas` is not an object; a `pfds` array or one of `flowDescriptions` / `urls` / `domainNames` is present but empty; a `PATCH` produced an object that fails validation. | The `detail` names the path, e.g. `pfdDatas.app-video: …`. Correct that value. Do not retry unchanged. |
| `429 Too Many Requests` | The token-bucket rate limiter rejected the request before any handling. | Back off exponentially and retry. There is no `Retry-After` header. |
| `500 Internal Server Error` | UDR could not be discovered, or a UDR write failed mid-batch and the transaction was rolled back. | Nothing from this batch is committed in NEF. Retry once UDR is reachable. If it keeps failing, check UDR and NRF; the `detail` names the application that failed. |
| `503 Service Unavailable` | NEF is draining for shutdown (`"Server is draining"`), or the dispatcher queue is full or stopped (`"Server is overloaded, please retry later"`). | Retry after a short delay, or against another NEF instance. Nothing was written. |

**Example — 400, `pfdDatas` missing**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Missing required field: pfdDatas"
}
```

**Example — 422, empty descriptor array**

```json
{
  "type": "about:blank",
  "title": "Unprocessable Entity",
  "status": 422,
  "detail": "pfdDatas.app-video: PfdDataForApp.pfds: must have at least 1 elements;"
}
```

**Example — 500, batch rolled back**

```json
{
  "type": "about:blank",
  "title": "Internal Server Error",
  "status": 500,
  "detail": "PFD transaction aborted: UDR write failed for app app-chat"
}
```

**Example — 500, UDR not reachable at all**

```json
{
  "type": "about:blank",
  "title": "Internal Server Error",
  "status": 500,
  "detail": "PFD transaction aborted: UDR not available"
}
```

There is no `503` from a UDR circuit breaker on this API. A UDR that is open-circuited,
unreachable or erroring surfaces as `500` on the transaction `PUT` and is invisible
everywhere else.

---

## Related pages

- [API Overview](overview.md) — authentication, error format, HTTP/2 requirements
- [Nnef_PFDmanagement API](nnef-pfd-management.md) — the SBI-side twin of this service,
  for internal 5GC NFs
- [Architecture](../ARCHITECTURE.md#2-the-request-path) — the generic request path,
  including the atomic-rollback cursor used by the transaction `PUT`
