<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Nnef_PFDmanagement SBI API (TS 29.551)

> **Audience.** This is the **SBI (Service-Based Interface)** face of PFD Management, for
> 5G Core NFs — SMF, PCF — that consume PFD data from NEF over the Nnef reference point.
> External Application Functions use the [T8 PFD Management API](pfd-management.md)
> instead. If you are an AF developer, this page does not apply to you.

## What this API is for

A consumer NF needs Packet Flow Descriptions to classify application traffic: the SMF needs
them at PDU session setup, and it needs to know when they change. This service gives it
three things the T8 face does not.

- **Bulk reads.** Fetch every application's PFD across every transaction in one call,
  optionally narrowed to a list of application ids — no walking transactions.
- **A pull with a UDR refresh.** `partial-pull` re-reads each requested application from UDR
  before answering, falling back to NEF's stored copy when UDR does not answer, so the
  consumer gets the freshest data available without talking to UDR itself.
- **Change notifications.** Subscribe once, and NEF posts an event whenever an
  application's PFD is written or removed through this service.

A consumer NF can also provision PFDs here. Transactions and applications work as they do on
T8 — same underlying store, same UDR writes, same all-or-nothing batch — but the
**request body shape is different** in ways that will reject a T8 payload. See
[Transaction body shapes](#transaction-body-shapes).

Read the [API Reference Overview](overview.md) first for authentication, HTTP/2
requirements, and the error format shared by all NEF APIs.

Defined by **3GPP TS 29.551 §5.2**.

---

## The happy path

An SMF provisions nothing; it reads and subscribes. That is the path shown here.

**1. Subscribe to PFD changes** for the applications you care about.

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "notifUri": "http://oai-smf:8080/pfd-notify",
           "supportedFeatures": "0",
           "applicationIds": ["app-video", "app-chat"]
         }' \
     http://oai-nef:8080/nnef-pfdmanagement/v1/subscriptions
```

`201 Created`, with a `Location` header and the assigned `subId` in the body:

```json
{
  "notifUri": "http://oai-smf:8080/pfd-notify",
  "supportedFeatures": "0",
  "applicationIds": ["app-video", "app-chat"],
  "subId": "7"
}
```

**2. Pull the current PFDs** for those applications, refreshed from UDR.

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{ "appIds": ["app-video", "app-chat"] }' \
     http://oai-nef:8080/nnef-pfdmanagement/v1/applications/partial-pull
```

**3. Receive a change notification** when someone rewrites `app-video`:

```json
{
  "eventType": "PFD_CHANGE",
  "appId": "app-video",
  "pfdData": {
    "externalAppId": "app-video",
    "pfds": {
      "pfd-video-1": {
        "pfdId": "pfd-video-1",
        "domainNames": ["video.example.com"]
      }
    }
  }
}
```

**4. Unsubscribe** when the consumer shuts down.

```bash
curl --http2-prior-knowledge \
     -X DELETE \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/subscriptions/7
```

`204 No Content`, empty body.

---

## Base path

```
/nnef-pfdmanagement/v1
```

There is no `{scsAsId}` segment on this API. The consumer NF's identity comes from the
`sub` claim of the bearer token, and that identity is checked against the same whitelist
and `allowed_apis` configuration the T8 side uses, under the service name
`nnef-pfdmanagement`. A rejection is `403` with the detail
`"NF not authorized for this Nnef service"` — the Nnef wording, distinct from the
`"AF not authorized for this service"` returned on T8 paths.

Transactions created here are **not owned** by any identity. Any authorized NF sees, reads
and can delete every transaction stored through this interface. There is no per-owner
filtering, unlike T8.

All transaction and subscription state is in memory and is lost on a NEF restart.

---

## Endpoints

### Transactions and applications

| Method | Path | What it does |
|---|---|---|
| `GET` | `/transactions` | List every PFD transaction stored through this interface |
| `PUT` | `/transactions/{transId}` | Create or replace a transaction as an atomic batch |
| `GET` | `/transactions/{transId}` | Read one transaction |
| `DELETE` | `/transactions/{transId}` | Delete a transaction and every PFD in it |
| `GET` | `/transactions/{transId}/applications/{appId}` | Read one application's PFD |
| `PUT` | `/transactions/{transId}/applications/{appId}` | Create or replace one application's PFD |
| `DELETE` | `/transactions/{transId}/applications/{appId}` | Delete one application's PFD |

### Bulk reads

| Method | Path | What it does |
|---|---|---|
| `GET` | `/applications[?app-ids=…]` | Every application across every transaction, optionally filtered |
| `POST` | `/applications/partial-pull` | The same set, re-read from UDR before answering |

### Change subscriptions

| Method | Path | What it does |
|---|---|---|
| `POST` | `/subscriptions` | Create a PFD-change subscription |
| `GET` | `/subscriptions/{subId}` | Read one subscription |
| `PUT` | `/subscriptions/{subId}` | Replace one subscription |
| `DELETE` | `/subscriptions/{subId}` | Delete one subscription |

There is no `PATCH` anywhere on this API, and no `GET /subscriptions` collection read.

> **Current implementation limitation — `POST /transactions`.** TS 29.551 defines a `POST`
> to the transactions collection as the create operation. This implementation answers it
> `405 Method Not Allowed`. Create a transaction with `PUT /transactions/{transId}`,
> choosing the identifier yourself.

**Two different 405 detail strings.** The `/transactions` route answers
`"HTTP method is not supported for this resource"`, the same wording the TS 29.122 routes
use. The `/applications` and `/subscriptions` routes answer the shorter
`"HTTP method is not supported"`. Both are `405` with the title `Method Not Allowed`; match
on the status, not the text.

---

## Transaction body shapes

This is where the SBI face differs most from T8, and where a copied T8 payload fails.

**`pfds` is an object, not an array.** On T8 an application's `pfds` is an array of
descriptors. Here it must be a JSON **object keyed by `pfdId`**. A `pfds` array is rejected
with `400` and the detail `Missing required field: pfds`, because the handler tests for an
object and reports the field as absent when it finds anything else.

**The application key is `externalAppId`, not `applicationId`.** If the field is present it
must equal the key or path segment it is filed under; if it is absent, NEF fills it in from
the key.

A transaction body may take any of five shapes. NEF normalises all of them to the same
stored form:

| Shape | Example |
|---|---|
| Object with `applications` as a map | `{"applications": {"app-video": { … }}}` |
| Object with `applications` as an array | `{"applications": [{"externalAppId": "app-video", … }]}` |
| Object with `pfdDatas` as a map | `{"pfdDatas": {"app-video": { … }}}` |
| A bare array of application objects | `[{"externalAppId": "app-video", … }]` |
| A single application object | `{"externalAppId": "app-video", "pfds": { … }}` |

A body in none of these shapes is `400` with the detail
`Transaction body must contain applications or pfdDatas`. A body that resolves to zero
applications is `400` with `Transaction must contain at least one application PFD` — unlike
T8, an empty transaction cannot be created here.

**Stored form.** Whatever you send, NEF stores and returns:

```json
{
  "transactionId": "txn-internal-001",
  "applications": {
    "app-video": {
      "externalAppId": "app-video",
      "pfds": {
        "pfd-video-1": {
          "pfdId": "pfd-video-1",
          "flowDescriptions": ["permit out 6 from 198.51.100.0/24 to assigned"],
          "domainNames": ["video.example.com"]
        }
      }
    }
  }
}
```

Any other top-level members you sent are preserved alongside these two. A `pfdDatas` member
is removed once its contents have been folded into `applications`. The identifier field is
`transactionId` here, where T8 uses `transId`.

**Validation applied to each application**

| Check | Failure detail (`400`) |
|---|---|
| The application value is an object | `Application PFD must be a JSON object` |
| `externalAppId`, if present, is a string | `externalAppId must be a string` |
| `externalAppId`, if present, matches its key | `externalAppId must match the target appId` |
| `pfds` is present and is an object | `Missing required field: pfds` |
| Each `pfds` entry is an object | `Each PFD entry must be a JSON object` |
| `pfdId`, if present, is a string | `pfdId must be a string` |
| `pfdId`, if present, matches its map key | `pfdId must match its map key` |
| `flowDescriptions` / `urls` / `domainNames`, if present, are arrays | `flowDescriptions must be an array when present` (and so on) |

Nothing beyond this is checked. An application whose descriptors carry none of
`flowDescriptions`, `urls` or `domainNames` is accepted and written to UDR, where it
classifies nothing.

---

## PUT `/transactions/{transId}` — create or replace a transaction

Call this to publish a batch of application PFDs from a consumer NF. The batch is atomic in
exactly the way the T8 transaction `PUT` is: NEF writes each application to UDR in turn, and
only after all of them succeed does it commit locally and answer.

```bash
curl --http2-prior-knowledge \
     -X PUT \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{
           "applications": {
             "app-music": {
               "externalAppId": "app-music",
               "pfds": {
                 "pfd-music-1": {
                   "pfdId": "pfd-music-1",
                   "domainNames": ["music.example.com", "cdn.music.example.net"],
                   "urls": ["https://api.music.example.com/*"]
                 }
               }
             }
           }
         }' \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001
```

**Response — 201 Created** when `{transId}` is new, **200 OK** when it existed. The body is
the stored transaction shown above.

**On failure.** If UDR cannot be discovered, nothing was written and the answer is `500`
with `PFD transaction aborted: UDR not available`. If a UDR write fails partway, NEF issues
compensating `DELETE`s over the applications already written, in reverse order, and answers
`500` with `PFD transaction aborted: UDR write failed for app <appId>`. In both cases no
local state is committed, so a transaction that already existed keeps its previous contents.
The compensating deletes are themselves best-effort, so UDR may retain orphans if they fail
too.

**After a successful replace**, and only then, NEF does two more things — both after the
response has already been sent, so neither can affect the status you see:

1. It notifies every matching PFD-change subscriber with `PFD_CHANGE`, once per application
   in the new transaction.
2. It fires best-effort UDR `DELETE`s for **removed applications** — those the previous
   version of the transaction held and the new body does not. This cleanup is the one
   behaviour the T8 transaction `PUT` does not have.

---

## GET `/transactions` — list transactions

Call this to enumerate the whole PFD store, for instance when a consumer NF starts up.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions
```

**Response — 200 OK.** An array of stored transaction objects in the form shown above.
Every transaction is returned regardless of which NF wrote it. An empty store is `[]`.

Only transactions created through **this** interface appear here. Transactions created over
T8 live in a separate store and are not visible on this path — though the *application
PFDs* both interfaces write end up in the same UDR records, keyed by application id.

---

## GET `/transactions/{transId}` — read one transaction

Call this to read back a single transaction by identifier. Unlike its T8 counterpart, this
one does look the identifier up: an unknown `{transId}` is `404` with the detail
`PFD transaction not found`.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001
```

**Response — 200 OK.** The stored transaction object, served from memory; UDR is not read.

---

## DELETE `/transactions/{transId}` — delete a transaction

Call this to retire a batch. NEF removes the local record first, then issues one UDR
`DELETE` per application it held.

**Response — 204 No Content.** The southbound deletes are best-effort, so `204` means NEF
has forgotten the transaction, not that UDR is clean. An unknown `{transId}` is `404`.

**Body exception:** responses on this path use an empty-body sink. `204`, `403` and `404`
are all bare status lines with no body and no content type. A `503` is the exception and
does carry Problem Details.

This path does **not** emit `PFD_REMOVE` notifications for the applications it deletes; only
the per-application `DELETE` does.

---

## GET `/transactions/{transId}/applications/{appId}` — read one application

Call this when you know which transaction holds the application.

```bash
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/transactions/txn-internal-001/applications/app-music
```

**Response — 200 OK.** The stored application object exactly as filed — no extra fields are
added on this path:

```json
{
  "externalAppId": "app-music",
  "pfds": {
    "pfd-music-1": {
      "pfdId": "pfd-music-1",
      "domainNames": ["music.example.com", "cdn.music.example.net"],
      "urls": ["https://api.music.example.com/*"]
    }
  }
}
```

`404` with `PFD transaction not found` if the transaction is unknown, or
`Application PFD not found in transaction` if the application is not in it.

---

## PUT `/transactions/{transId}/applications/{appId}` — write one application

Call this to add or replace a single application without resending the batch.

**Response — 201 Created** when the application was not already in the transaction,
**200 OK** when it was. The body is the normalised application object.

Three behaviours worth knowing:

- **The transaction is created implicitly.** If `{transId}` does not exist, this call
  creates it and puts the application in it. It does not `404`. A typo in the transaction
  id therefore produces a new transaction rather than an error.
- **The UDR write is best-effort.** NEF updates its own state first and then fires one
  `PUT /nudr-dr/v1/application-data/pfds/{appId}`. A UDR failure is logged and the
  consumer still receives `200`/`201`. Only the transaction `PUT` reports UDR failure.
- **Subscribers are notified** with `PFD_CHANGE` for this application, after the response
  is sent.

---

## DELETE `/transactions/{transId}/applications/{appId}` — delete one application

Call this to withdraw one application. The transaction record survives, even if this was its
last application.

**Response — 204 No Content.** The local entry is erased, one best-effort UDR `DELETE` is
fired, and every matching subscriber is notified with `PFD_REMOVE` (no `pfdData` member on
that event). An unknown transaction or application is `404`.

The same empty-body rule applies: `204`, `403` and `404` are bare status lines.

---

## GET `/applications` — every application, optionally filtered

Call this when you want PFDs by application id and do not care which transaction holds them.

**Query parameter**

| Parameter | Form | Description |
|---|---|---|
| `app-ids` | Repeated, one id per occurrence | Return only these application ids. Omit it to return everything. |

The parameter is spelled `app-ids`, with a hyphen, and is **repeated** rather than
comma-separated. `?app-ids=app-video,app-chat` matches an application literally named
`app-video,app-chat` and therefore returns nothing.

```bash
# every application
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     http://oai-nef:8080/nnef-pfdmanagement/v1/applications

# two of them
curl --http2-prior-knowledge \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     "http://oai-nef:8080/nnef-pfdmanagement/v1/applications?app-ids=app-video&app-ids=app-chat"
```

**Response — 200 OK.** An array of application objects, each with the `transId` of the
transaction that holds it added:

```json
[
  {
    "externalAppId": "app-video",
    "pfds": {
      "pfd-video-1": {
        "pfdId": "pfd-video-1",
        "flowDescriptions": ["permit out 6 from 198.51.100.0/24 to assigned"],
        "domainNames": ["video.example.com"]
      }
    },
    "transId": "txn-internal-001"
  },
  {
    "externalAppId": "app-chat",
    "pfds": {
      "pfd-chat-1": {
        "pfdId": "pfd-chat-1",
        "urls": ["https://chat.example.com/*"]
      }
    },
    "transId": "txn-internal-002"
  }
]
```

This read is served entirely from NEF's memory. Requested ids that do not exist are
absent from the array; there is no error and no placeholder. Compare what you asked for
against the `externalAppId` values you got back.

---

## POST `/applications/partial-pull` — refreshed selective fetch

Call this instead of `GET /applications` when the data must be current — at PDU session
setup, say. It performs the same selection, but for each selected application it first
issues `GET /nudr-dr/v1/application-data/pfds/{appId}` to UDR and prefers UDR's answer over
NEF's stored copy.

**Request body**

| Field | Type | Description |
|---|---|---|
| `appIds` | array[string] | Application ids to fetch. Note the camel-case spelling here, against the hyphenated `app-ids` query parameter on the `GET`. |

```bash
curl --http2-prior-knowledge \
     -X POST \
     -H "Authorization: Bearer <your-nf-jwt-token>" \
     -H "Content-Type: application/json" \
     -d '{ "appIds": ["app-video", "app-chat"] }' \
     http://oai-nef:8080/nnef-pfdmanagement/v1/applications/partial-pull
```

**Response — 200 OK.** An array of PFD objects, each stamped with `applicationId`:

```json
[
  {
    "externalAppId": "app-video",
    "pfds": {
      "pfd-video-1": {
        "pfdId": "pfd-video-1",
        "domainNames": ["video.example.com"]
      }
    },
    "applicationId": "app-video"
  }
]
```

**How each entry is resolved**

| UDR response for that application | Entry returned |
|---|---|
| Exactly `200` with a parseable body | The UDR body |
| Exactly `200` with an empty or unparseable body | `{}`, plus `applicationId` |
| Anything else — `204`, `4xx`, `5xx`, timeout, unreachable | NEF's stored copy |

The comparison against `200` is strict, so a UDR that answers `204 No Content` falls back to
the stored copy rather than being treated as success. If UDR cannot be discovered at all,
every entry is served from the stored copy and the response is still `200`. **A `200` from
this endpoint therefore does not prove the data came from UDR.** If freshness is
load-bearing, monitor NEF's logs for the per-application fallback warnings.

**Selection rules, and a filter that fails open**

- Ids not present in NEF's store are silently omitted. Check the response against your
  request.
- An empty or absent `appIds` array means "no filter", so **every** application is returned.
- **Current implementation limitation:** a malformed JSON body is not an error on this
  endpoint. Every sibling handler answers `400` for unparseable JSON; this one swallows the
  parse failure, treats the body as `{}`, and therefore returns the entire PFD store. A
  consumer that truncates or corrupts its request body gets `200` and a much larger answer
  than it asked for, with no indication anything went wrong. Validate the response size, and
  do not rely on a `200` to mean the request body was understood.
- If nothing matches, the answer is `200` with `[]` and no UDR call is made at all.

---

## Change subscriptions

### POST `/subscriptions` — subscribe to PFD changes

Call this once per consumer NF so that it learns about PFD writes and removals instead of
polling.

**Request body**

| Field | Type | Required | Description |
|---|---|---|---|
| `notifUri` | string (URI) | Required | Where NEF POSTs change events. Must pass the SSRF callback check. |
| `supportedFeatures` | string | Required | Feature-negotiation string. It has no default: omitting it is a `400`, so send `"0"` if you negotiate nothing. |
| `applicationIds` | array[string] | Optional | Restrict events to these application ids. If present it must hold at least one element, or the request is `422`. Omit it to receive events for every application. |

The wire field is `notifUri`; NEF translates it to and from the model's `notifyUri`
internally, and always answers with `notifUri`.

**Response — 201 Created**, with a `Location` header pointing at the new subscription and
`subId` in the body.

### GET / PUT `/subscriptions/{subId}`

`GET` returns the stored subscription with `subId` added. `PUT` replaces it and applies the
same validation as the create, including the `supportedFeatures` requirement — it is a
replacement, not a patch, so resend every field. An unknown `{subId}` is `404` with the
detail `PFD subscription not found`.

### DELETE `/subscriptions/{subId}`

`204 No Content`. This path uses the empty-body sink, so `204`, `403` and `404` are all bare
status lines with no body.

### Notification payload

NEF posts this to `notifUri`:

```json
{
  "eventType": "PFD_CHANGE",
  "appId": "app-video",
  "pfdData": {
    "externalAppId": "app-video",
    "pfds": {
      "pfd-video-1": { "pfdId": "pfd-video-1", "domainNames": ["video.example.com"] }
    }
  }
}
```

| Field | Description |
|---|---|
| `eventType` | `PFD_CHANGE` when an application was written, `PFD_REMOVE` when it was deleted. |
| `appId` | The application the event concerns. |
| `pfdData` | The new PFD data. **Present on `PFD_CHANGE` only** — a `PFD_REMOVE` event omits the member entirely. |

Events are emitted by the transaction `PUT` (one `PFD_CHANGE` per application in the new
transaction), the application `PUT` (one `PFD_CHANGE`) and the application `DELETE` (one
`PFD_REMOVE`). The transaction `DELETE` emits nothing, and neither does anything done over
the T8 interface. Delivery is attempted up to three times behind a circuit breaker and is
then dropped; it is enqueued after the triggering response has already been sent, so a
notification failure never changes the status the writer saw.

---

## Southbound behaviour

PFDs are stored in UDR at:

```
GET    /nudr-dr/v1/application-data/pfds/{appId}
PUT    /nudr-dr/v1/application-data/pfds/{appId}
DELETE /nudr-dr/v1/application-data/pfds/{appId}
```

Note the version: this build uses the UDR `v1` application-data path, not `v2`.

| Operation | UDR calls | Is a UDR failure reported to the caller? |
|---|---|---|
| Transaction `PUT` | One `PUT` per application, plus post-commit `DELETE`s for removed applications | **Yes** for the writes — a failure rolls back and answers `500`. No for the removals. |
| Transaction `DELETE` | One `DELETE` per application it held | No — always `204` |
| Application `PUT` | One `PUT` | No — warning only, still `200`/`201` |
| Application `DELETE` | One `DELETE` | No — always `204` |
| `POST /applications/partial-pull` | One `GET` per selected application | No — falls back to the stored copy and still answers `200` |
| `GET /transactions…`, `GET /applications` | None | — |

---

## Error responses

| Status | What happened | What the client should do |
|---|---|---|
| `400 Bad Request` | The body is not JSON (except on `partial-pull`, see above); the transaction body is in none of the accepted shapes or resolves to zero applications; `pfds` is missing or is not an object; `externalAppId` or `pfdId` does not match its key; `notifUri` is missing, empty or fails the SSRF check; `supportedFeatures` is missing on a subscription write. | The `detail` names the exact rule. Fix the body — in particular check that `pfds` is an object keyed by `pfdId`, not an array. Do not retry unchanged. A body over 1 MiB is not a `400` — the HTTP/2 layer resets the stream instead; see the [overview](overview.md). |
| `403 Forbidden` | No bearer token, a token whose `sub` claim cannot be extracted, an NF identity that is not whitelisted, or one whose `allowed_apis` does not include `nnef-pfdmanagement`. | Check the consumer NF's token and whitelist entry. NEF never emits `401` — every authorization failure is this `403`, here with the detail `"NF not authorized for this Nnef service"`. Retrying will not help until configuration or token changes. |
| `404 Not Found` | Unknown `{transId}`, unknown `{appId}` within a transaction, unknown `{subId}`, or a path that does not match one of the three route families. | Check the identifier. On this API a missing transaction may mean NEF restarted — all state is in memory. |
| `405 Method Not Allowed` | An unsupported method, including `POST /transactions`, which TS 29.551 defines but this build does not implement. | Use `PUT /transactions/{transId}` to create. The `detail` differs between routes; match on the status. |
| `422 Unprocessable Entity` | A subscription body parsed but failed model validation — most often an `applicationIds` array that is present and empty. | Correct the offending member. Do not retry unchanged. |
| `429 Too Many Requests` | The token-bucket rate limiter rejected the request before any handling. | Back off exponentially and retry. There is no `Retry-After` header. |
| `500 Internal Server Error` | On a transaction `PUT` only: UDR could not be discovered, or a UDR write failed mid-batch and the batch was rolled back. | Nothing from the batch is committed. Retry once UDR is reachable; the `detail` names the application that failed. |
| `503 Service Unavailable` | NEF is draining for shutdown (`"Server is draining"`), or the dispatcher queue is full or stopped (`"Server is overloaded, please retry later"`). | Retry after a short delay, or against another NEF instance. Nothing was written. |

**Example — 400, `pfds` sent as an array**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Missing required field: pfds"
}
```

**Example — 400, unrecognised transaction body**

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "Transaction body must contain applications or pfdDatas"
}
```

**Example — 404**

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "PFD transaction not found"
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

There is no `503` from a UDR circuit breaker on this API. A UDR that is open-circuited,
unreachable or erroring surfaces as `500` on the transaction `PUT`, as a silent fallback to
cached data on `partial-pull`, and as nothing at all everywhere else.

---

## Related pages

- [API Overview](overview.md) — authentication, error format, HTTP/2 requirements
- [PFD Management (T8)](pfd-management.md) — the AF-facing face of the same store, including
  its [atomicity guarantee](pfd-management.md#atomicity-guarantee)
- [Architecture](../ARCHITECTURE.md#2-the-request-path) — the generic request path,
  including the atomic-rollback cursor used by the transaction `PUT`
