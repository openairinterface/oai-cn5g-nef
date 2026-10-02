<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# QoS Monitoring / AS Session with QoS API (TS 29.122)

## What this API is for

An AF / SCS-AS uses this API to ask the network for a specific quality of service on a UE's
data flows, and to be told when the network delivers or fails to deliver it. A video
service can request a guaranteed-bitrate treatment for one media flow; a latency-sensitive
application can ask to be notified of measured uplink, downlink and round-trip delay.

NEF mediates the TS 29.122 **AS Session with QoS** resource to a TS 29.514 PCF
`Npcf_PolicyAuthorization` AppSession. The AF names the QoS treatment it wants with a
`qosReference` — a label for preconfigured QoS information. NEF copies that label southbound
unchanged; it does not translate it into a numeric 5QI, so the label must be one the PCF
already knows.

QoS monitoring is not a separate API. It is an event capability carried inside the AS
Session with QoS resource, via `events` and `qosMonInfo`. There is no `/3gpp-qos-monitoring/`
path in this implementation.

Read the [API Overview](overview.md) for common authentication, error, and HTTP/2
conventions. Handler-specific exceptions are documented below.

The procedure is defined in **3GPP TS 29.122 Release 17 clause 4.4.13**, the REST API in
clause **5.14** (collection resource 5.14.3.2, individual resource 5.14.3.3, event
notification 5.14.3A.2). See
[ETSI TS 129 122 V17.9.0](https://www.etsi.org/deliver/etsi_ts/129100_129199/129122/17.09.00_60/ts_129122v170900p.pdf).

---

## The happy path

Subscribe, receive notifications, unsubscribe.

**1. Create the subscription.** Supply a UE selector, the flows, the QoS reference you want,
and a callback.

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "ueIpv4Addr": "10.45.0.2",
    "notificationDestination": "http://af.example.com/notify/qos",
    "qosReference": "GBR_ConvVoice",
    "flowInfo": [{
      "flowId": 1,
      "flowDescriptions": [
        "permit out ip from 10.45.0.2 to any",
        "permit in ip from any to 10.45.0.2"
      ]
    }],
    "events": ["QOS_MONITORING", "QOS_GUARANTEED", "QOS_NOT_GUARANTEED"],
    "qosMonInfo": {
      "repThreshDl": 20,
      "repThreshUl": 20,
      "repThreshRp": 40
    }
  }'
```

`201 Created`. The body is the resource with a `self` link added, and the same value appears
in the `Location` header. The last path segment is the subscription id you use from here on.

**2. Receive user-plane notifications.** PCF reports to NEF; NEF maps each report into a
`UserPlaneNotificationData` document and POSTs it to `notificationDestination`. See
[Notification payload](#notification-payload).

**3. Delete the subscription** when the session ends.

```bash
curl --http2-prior-knowledge -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2
```

`204 No Content`, with an empty body.

Two things to know before building on this flow. The `repThresh*` members are the only
`qosMonInfo` members this build preserves — see
[the model collision](#current-implementation-limitation--qosmoninfo-model-collision). And
`DELETE` does not revoke the PCF AppSession — see
[DELETE](#delete--delete-subscription).

---

## Base path

```
/3gpp-as-session-with-qos/v1/{scsAsId}/subscriptions[/{subscriptionId}]
```

| Parameter | Type | Description |
|---|---|---|
| `scsAsId` | string | SCS/AS identity authorized for the literal `nnef-qosmonitoring` service identifier by the configured authentication and authorization policy |
| `subscriptionId` | string | The last segment of the `self` URI returned by the create |

A path whose second segment is not the literal `subscriptions` is answered `404 Not Found`
with the detail `"Requested resource was not found"`.

All subscription and PCF-correlation state is in memory and is lost on restart; see
[Architecture — known gaps](../ARCHITECTURE.md#12-known-gaps).

---

## Endpoints

| Method | Path | What it does |
|---|---|---|
| `POST` | `/{scsAsId}/subscriptions` | Create a local subscription and a PCF AppSession |
| `GET` | `/{scsAsId}/subscriptions` | List this SCS/AS's in-memory subscriptions |
| `GET` | `/{scsAsId}/subscriptions/{subscriptionId}` | Read one in-memory subscription |
| `PUT` | `/{scsAsId}/subscriptions/{subscriptionId}` | Replace a local subscription, then attempt a PCF update |
| `PATCH` | `/{scsAsId}/subscriptions/{subscriptionId}` | JSON Merge Patch a local subscription, then attempt a PCF update |
| `DELETE` | `/{scsAsId}/subscriptions/{subscriptionId}` | Delete a local subscription after a best-effort southbound call |

Any other method-and-path combination — a `POST` to an item URI, a `PUT` or `DELETE` on the
collection — is answered `405 Method Not Allowed` with the detail
`"HTTP method is not supported for this resource"`.

---

## POST — create QoS session subscription

Call this at the start of a session that needs a specific QoS treatment. NEF stores the
subscription provisionally, creates the PCF AppSession, and only answers `201` once PCF has
returned a usable AppSession identifier. A PCF failure removes the provisional state, so a
non-`201` leaves nothing behind in NEF.

A runnable request should carry a supported UE address selector, flow or application
identity, a QoS reference, and a callback — even though both the OpenAPI schema and the
current handler are looser than the TS procedure and than what PCF needs.

**Request**

```http
POST /3gpp-as-session-with-qos/v1/scs-as-1/subscriptions HTTP/2
Host: oai-nef:8080
Content-Type: application/json
Authorization: Bearer <your-jwt-token>
```

```json
{
  "ueIpv4Addr": "10.45.0.2",
  "notificationDestination": "http://af.example.com/notify/qos",
  "qosReference": "GBR_ConvVoice",
  "flowInfo": [
    {
      "flowId": 1,
      "flowDescriptions": [
        "permit out ip from 10.45.0.2 to any",
        "permit in ip from any to 10.45.0.2"
      ]
    }
  ],
  "events": [
    "QOS_MONITORING",
    "QOS_GUARANTEED",
    "QOS_NOT_GUARANTEED"
  ],
  "qosMonInfo": {
    "reqQosMonParams": ["DOWNLINK", "UPLINK", "ROUND_TRIP"],
    "repFreqs": ["PERIODIC", "EVENT_TRIGGERED"],
    "repPeriod": 30,
    "waitTime": 5
  },
  "dnn": "internet",
  "snssai": {
    "sst": 1,
    "sd": "010203"
  }
}
```

### What is actually required

The inputs sit in three requirement layers, and they do not agree.

| Layer | Required or expected inputs | Consequence |
|---|---|---|
| TS 29.122 clause 4.4.13 procedure | `{scsAsId}`, UE IP address, IP flow description, QoS reference, notification destination; where the `AppId` feature applies, the flow-description / external-application-ID rule applies too | This is the procedural content applications should follow |
| Vendored OpenAPI mechanics | Only `notificationDestination` is in `AsSessionWithQoSSubscription.required`; inside `FlowInfo`, only `flowId` is structurally required | A body can validate structurally while omitting inputs the procedure and PCF expect |
| Current handler and downstream PCF | NEF directly requires and SSRF-validates `notificationDestination`, but does not require UE, flow/application or QoS fields before provisional storage. PCF `ascReqData` requires an IPv4, IPv6 or MAC selector | A PCF rejection, a discovery or request failure, or an unusable AppSession ID causes local rollback and a generic `500` |

The practical rule: satisfy the first layer. Passing the second is not enough to get a
working session.

**Response — 201 Created**

The body holds the standardized resource fields plus `self`. It does not add a
`subscriptionId` property. The shape below is what the current implementation produces when
the raw bind address is `10.0.0.4`:

```http
HTTP/2 201
Content-Type: application/json
Location: 10.0.0.4/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2
```

```json
{
  "self": "10.0.0.4/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2",
  "ueIpv4Addr": "10.45.0.2",
  "notificationDestination": "http://af.example.com/notify/qos",
  "qosReference": "GBR_ConvVoice",
  "flowInfo": [
    {
      "flowId": 1,
      "flowDescriptions": [
        "permit out ip from 10.45.0.2 to any",
        "permit in ip from any to 10.45.0.2"
      ]
    }
  ],
  "events": [
    "QOS_MONITORING",
    "QOS_GUARANTEED",
    "QOS_NOT_GUARANTEED"
  ],
  "qosMonInfo": null,
  "dnn": "internet",
  "snssai": {
    "sst": 1,
    "sd": "010203"
  }
}
```

### Current implementation limitation — `qosMonInfo` model collision

The vendored TS 29.122 schema requires `reqQosMonParams` and `repFreqs` inside a supplied
`qosMonInfo`, and also defines `waitTime`, `repPeriod` and three `repThresh*` members.

The compiled C++ type that carries the same `QosMonitoringInformation` name is the
PCF-shaped model instead. It parses and serializes only `repThreshDl`, `repThreshUl` and
`repThreshRp`, and its validation does not enforce the two TS 29.122 required arrays.

The consequences for the complete request shown above:

- It is accepted, but `reqQosMonParams`, `repFreqs`, `repPeriod` and `waitTime` are
  discarded by the typed create path.
- With no `repThresh*` values present, typed serialization emits `"qosMonInfo": null` in the
  `201` body and `"qosMon": null` in the PCF `evSubsc`. Both null-valued objects are
  schema-nonconformant for this request shape.
- The separately translated `events` array still requests `QOS_MONITORING`. Even with
  `events` absent, the presence of `qosMonInfo` adds that event.
- The original request is retained in the raw in-memory subscription, so a later `GET` or
  list may show the complete input object even though the `201` and the PCF request did not
  preserve it.

**What to do about it:** if you need thresholds to reach PCF, send `repThreshDl`,
`repThreshUl` and `repThreshRp`. The requested-parameter, frequency, period and wait-time
information cannot currently be conveyed at all.

### Current implementation limitation — non-absolute `self` and `Location`

`self` and `Location` concatenate the raw bind address with the relative path, omitting the
URI scheme and port. They are not absolute URIs. Do not copy them as standards-conformant
examples, and do not feed them to a client that expects to dereference them without repair.

### TS-defined versus implemented PCF response handling

| Behavior | PCF response | NEF handling |
|---|---|---|
| TS 29.514 normal creation | `201` with required `Location` | The standard success case supplies the new AppSession resource URI |
| TS 29.514 equivalent existing resource | `303` with `Location` | **Current implementation limitation:** NEF rejects `303` as non-2xx, rolls back provisional state, and returns `500` |
| Implemented accommodation | Any 2xx carrying an ID in the non-schema body property `appSessionId` or in the final `Location` segment | NEF accepts a valid, safe ID and returns northbound `201`; a missing or invalid ID causes rollback and `500` |

### Request field reference

| Field | Description |
|---|---|
| `ueIpv4Addr` | UE IPv4 selector. Use one supported IPv4, IPv6 or MAC selector in runnable requests |
| `ueIpv6Addr` | UE IPv6 selector |
| `macAddr` | UE MAC-address selector |
| `ipDomain` | IPv4 address-domain identifier |
| `dnn` | Data Network Name |
| `snssai` | S-NSSAI slice identifier |
| `notificationDestination` | AF / SCS-AS callback URI. OpenAPI-required, and directly required and SSRF-validated by NEF |
| `exterAppId` | External application identity, where applicable instead of flow descriptions |
| `flowInfo` | One or more IP-flow objects. `flowId` is OpenAPI-required inside each object; `flowDescriptions` carries TS 29.214-style packet filters |
| `qosReference` | Name of preconfigured QoS information. Copied into PCF media components rather than converted by NEF into a numeric 5QI |
| `altQoSReferences` | Ordered alternative preconfigured QoS references |
| `events` | Requested user-plane events: `QOS_MONITORING`, `QOS_GUARANTEED`, `QOS_NOT_GUARANTEED`, `SUCCESSFUL_RESOURCES_ALLOCATION`, `FAILED_RESOURCES_ALLOCATION`, `USAGE_REPORT`, `ACCESS_TYPE_CHANGE`, `PLMN_CHG` |
| `qosMonInfo` | TS 29.122 QoS monitoring configuration. When present, the vendored schema requires both `reqQosMonParams` and `repFreqs`; the compiled model does not enforce them |
| `qosMonInfo.reqQosMonParams` | One or more of `DOWNLINK`, `UPLINK`, `ROUND_TRIP`. Currently discarded by the colliding compiled model |
| `qosMonInfo.repFreqs` | One or more of `EVENT_TRIGGERED`, `PERIODIC`, `SESSION_RELEASE`. Currently discarded by the colliding compiled model |
| `qosMonInfo.repPeriod` | Period in seconds for periodic reporting. Currently discarded by the colliding compiled model |
| `qosMonInfo.waitTime` | Minimum interval in seconds between event-triggered reports. Currently discarded by the colliding compiled model |
| `qosMonInfo.repThreshDl`, `repThreshUl`, `repThreshRp` | Downlink, uplink and round-trip reporting thresholds. These are the only `qosMonInfo` members the compiled model parses and serializes |
| `usageThreshold` | Volume and/or duration threshold for usage reporting |
| `directNotifInd` | Requests direct event notification when `true` |

---

## GET — list subscriptions

Call this to recover the subscription ids this SCS/AS holds, for example after an AF restart.

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions
```

**Response — 200 OK**

```json
[
  {
    "subId": "b7e3a1c2",
    "ueIpv4Addr": "10.45.0.2",
    "notificationDestination": "http://af.example.com/notify/qos",
    "qosReference": "GBR_ConvVoice",
    "flowInfo": [
      {
        "flowId": 1,
        "flowDescriptions": [
          "permit out ip from 10.45.0.2 to any",
          "permit in ip from any to 10.45.0.2"
        ]
      }
    ]
  }
]
```

`subId` is a non-standard, list-only implementation augmentation. It is not a property of the
TS 29.122 subscription resource, and the item `GET` does not add it. An empty collection is
`[]`.

The implementation does not read PCF state here and does not apply the OpenAPI collection
query filters, so a query string is accepted and ignored.

---

## GET — read one subscription

Call this to see what NEF currently holds for a subscription, for instance to confirm a
`PATCH` landed.

```bash
curl --http2-prior-knowledge \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2
```

**Response — 200 OK.** The stored in-memory subscription, with no PCF read. Create stores
the request body before adding `self` to the serialized `201`, so the item `GET` need not
reproduce that `201` exactly.

A missing item is `404` with the detail `"QoS subscription not found"`. An item owned by
another SCS/AS is `403` with `"AF is not allowed to access this subscription"`.

---

## PUT — replace subscription

Call this to change a live subscription wholesale — a new callback, a different QoS
reference. Keep every procedure-required field in the replacement: what you omit can be
removed from local state.

```bash
curl --http2-prior-knowledge \
  -X PUT http://oai-nef:8080/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2 \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "ueIpv4Addr": "10.45.0.2",
    "notificationDestination": "http://af.example.com/notify/qos-v2",
    "qosReference": "GBR_ConvVoice",
    "flowInfo": [{
      "flowId": 1,
      "flowDescriptions": [
        "permit out ip from 10.45.0.2 to any",
        "permit in ip from any to 10.45.0.2"
      ]
    }],
    "events": ["QOS_MONITORING", "QOS_GUARANTEED", "QOS_NOT_GUARANTEED"],
    "qosMonInfo": {
      "repThreshDl": 15,
      "repThreshUl": 15,
      "repThreshRp": 30
    }
  }'
```

**Response — 200 OK.** The locally replaced subscription — including when PCF rejected the
attempted update.

**Immutable-field guard.** For `ueIpv4Addr`, `ueIpv6Addr`, `macAddr`, `ipDomain`, `dnn`,
`snssai` and `supportedFeatures`, the guard rejects changing an existing field or newly
adding one that was absent, with a `400` and the detail `Field '<name>' is immutable`. It
does not reject removal: omitting one of these on a `PUT` removes it from local state.

**Current implementation limitations.** Four, and they compound:

- NEF mutates local state first, before the southbound call.
- TS 29.514 defines `AppSessionContextUpdateDataPatch` with update members nested beneath
  `ascReqData`; NEF sends a flat, schema-nonconformant fragment instead.
- The builder invocation omits `evSubsc`, so changes to `events`, `qosMonInfo`,
  `usageThreshold` and `directNotifInd` never reach PCF.
- A PCF failure is warning-only and the AF still receives `200`.

Together these mean a `200` here does **not** confirm the PCF side changed, and local and PCF
state can diverge silently. If an update must take effect in the network, delete the
subscription and create a new one.

---

## PATCH — merge patch subscription

Call this for a narrow change — one field, typically. The body is an RFC 7396 JSON Merge
Patch applied to local state, followed by the same best-effort PCF update as `PUT`.

```bash
curl --http2-prior-knowledge \
  -X PATCH http://oai-nef:8080/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2 \
  -H "Content-Type: application/merge-patch+json" \
  -H "Authorization: Bearer <your-jwt-token>" \
  -d '{
    "qosReference": "GBR_Conversational_Video",
    "qosMonInfo": {
      "repThreshDl": 10,
      "repThreshUl": 10,
      "repThreshRp": 25
    }
  }'
```

**Response — 200 OK.** The locally merged subscription — including when PCF rejected the
attempted update.

The same seven-field guard used by `PUT` rejects changes and additions here, but a `PATCH`
with an explicit `null` *can* remove a guarded field, because the guard runs after the merge
has already removed it. `notificationDestination` cannot be removed: attempting it is a
`400`.

Every `PUT` limitation above — local-first mutation, the flat schema-nonconformant PCF
fragment, omitted `evSubsc` changes, and `200`-on-PCF-failure — applies to `PATCH` as well.

---

## DELETE — delete subscription

Call this when the session ends.

```bash
curl --http2-prior-knowledge \
  -X DELETE \
  -H "Authorization: Bearer <your-jwt-token>" \
  http://oai-nef:8080/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2
```

**Response — 204 No Content.** The northbound resource and its SCS/AS profile association
are removed locally regardless of the southbound result.

**Current implementation limitation — the PCF AppSession is not revoked.** NEF makes a
best-effort call to SMF Event Exposure `DELETE` using the stored PCF AppSession ID. It does
not invoke PCF AppSession deletion. The PCF resource therefore survives the `DELETE`, and the
PCF-ID and QoS-to-PCF correlation entries are left stale. Operators who need the PCF session
gone must remove it out of band. The per-service lifecycle state machine has not been
written down.

**Body exception on this method.** After the dispatcher accepts a `DELETE`, its empty-body
sink discards the application handler's body and emits no content type. An authorization or
ownership failure is therefore a bare `403`, and a missing local subscription a bare `404`,
neither carrying Problem Details. A successful `DELETE` is likewise a bare `204`. The one
exception is a queue-full or stopped rejection, which happens before that sink is used and
does return a `503` Problem Details body.

---

## Southbound behavior

| Operation | Implemented southbound behavior | Result or limitation |
|---|---|---|
| Create | Conditionally discover PCF, then `POST` `AppSessionContext { ascReqData, evSubsc }` to `/npcf-policyauthorization/v1/app-sessions` | `flowInfo` becomes media components, `qosReference` is copied, and `events` are translated into `evSubsc`; successful and failed resource-allocation events are always requested. The model collision means the example's four non-threshold `qosMonInfo` members are dropped and PCF receives schema-nonconformant `qosMon: null` |
| `GET` item or list | None | Results come only from in-memory NEF state |
| `PUT` / `PATCH` | Attempt a PCF AppSession `PATCH` with a flat fragment | Missing `ascReqData` envelope, omitted event-subscription changes and warning-only failure can leave PCF and local state different |
| `DELETE` | Attempt an SMF Event Exposure `DELETE` using the PCF AppSession ID | The PCF AppSession remains and correlation entries become stale |

`notificationDestination` is the AF-facing callback. During create, NEF constructs a
separate PCF-facing `evSubsc.notifUri` under `/nef-notify/v1/notify/{qosSubId}`.

For the create request shown earlier, the relevant implemented PCF fragment is shaped as
follows — event order is not significant:

```json
{
  "evSubsc": {
    "notifUri": "http://<nef>/nef-notify/v1/notify/b7e3a1c2",
    "events": [
      { "event": "FAILED_RESOURCES_ALLOCATION" },
      { "event": "QOS_MONITORING" },
      { "event": "QOS_NOTIF" },
      { "event": "SUCCESSFUL_RESOURCES_ALLOCATION" }
    ],
    "qosMon": null
  }
}
```

This is an observed implementation shape, not a TS-defined recommendation. Supplying one or
more `repThresh*` members preserves those members inside `qosMon`, but does not restore the
discarded requested-parameter, frequency, period or wait-time information. NEF does not
separately populate the PCF `evSubsc.reqQosMonParams` member from the discarded northbound
array.

**Racing a `DELETE` against an in-flight create.** If the `DELETE` wins while the PCF create
is still outstanding, a later accepted 2xx with a usable ID makes the create continuation
answer `204` without resurrecting local state — and the PCF AppSession is orphaned. A later
PCF failure or unusable ID makes that same continuation answer `500`, because PCF result
validation runs before the vanished-subscription check.

See [Architecture](../ARCHITECTURE.md#2-the-request-path) for how discovery, the southbound
response and the deferred answer are sequenced.

---

## Notification payload

PCF sends an `EventsNotification` to NEF's `evSubsc.notifUri`. NEF correlates it, builds a
`UserPlaneNotificationData`-shaped document, and POSTs that to the AF / SCS-AS
`notificationDestination`.

The following is the **observed mapper output** for a schema-conformant PCF event containing
`"flows": [{ "medCompN": 1 }]`:

```json
{
  "transaction": "/3gpp-as-session-with-qos/v1/scs-as-1/subscriptions/b7e3a1c2",
  "eventReports": [
    {
      "event": "QOS_MONITORING",
      "flowIds": [
        {
          "medCompN": 1
        }
      ],
      "qosMonReports": [
        {
          "ulDelays": [12],
          "dlDelays": [8],
          "rtDelays": [20]
        }
      ]
    }
  ]
}
```

**Current implementation limitation — `flowIds` shape.** The observed `flowIds` value above
is not TS 29.122-conformant. TS 29.514 defines each PCF `AfEventNotification.flows` entry as
an object requiring `medCompN`, while TS 29.122 defines `flowIds` as an array of integers.
The mapper copies `flows` verbatim rather than extracting `medCompN`; the TS-defined desired
output for that PCF entry is `"flowIds": [1]`. An AF callback must therefore accept objects
where the spec says integers. If PCF omits `flows`, the mapper omits `flowIds`, which means
the event report applies to all flows.

**`transaction` is a relative path.** TS 29.122 models it as a link. NEF copies the stored
relative `self` shown above; the bind-address prefixing applied to the serialized create
response and its `Location` does not update the stored value used in notifications.

**Event mapping and delivery.** PCF `QOS_NOTIF` is split into `QOS_GUARANTEED` or
`QOS_NOT_GUARANTEED`, and available monitoring, usage, QoS-reference and PLMN details are
propagated. Flow details are subject to the object-versus-integer mismatch above. If mapping
fails, NEF forwards the raw PCF payload, so an AF callback must tolerate a body that is not
in the shape above.

NEF acknowledges PCF with `204` after enqueueing AF delivery. It does not wait for AF
success, and AF delivery and the PCF `204` have no guaranteed wire order. AF delivery is
attempted up to three times; if the notification queue is full, delivery is dropped while
PCF still receives `204`.

---

## Error responses

Create and update error branches carry Problem Details bodies. For example, an absent or
empty callback produces:

```json
{
  "type": "about:blank",
  "title": "Bad Request",
  "status": 400,
  "detail": "notificationDestination is required"
}
```

| Status | What happened | What the client should do |
|---|---|---|
| `400 Bad Request` | Malformed or typed-invalid JSON; a missing or empty callback; an unsafe callback URI; a change to one of the seven immutable fields (`detail`: `Field 'dnn' is immutable`); a `PATCH` that removes `notificationDestination`; a `PATCH` whose merged result fails validation; or a create whose flow/QoS content cannot be translated into a PCF body. | Fix the body. Do not retry unchanged. A body over 1 MiB is not a `400` — the HTTP/2 layer resets the stream instead; see the [overview](overview.md). |
| `403 Forbidden` | Missing or invalid authorization as exposed by this handler; an identity mismatch; an API denial; or an ownership failure on an existing subscription. | Check the token and the AF's `allowed_apis` for `nnef-qosmonitoring`. NEF never emits `401` — every authorization failure is this `403`, with the detail `"AF not authorized for this service"`. Retrying will not help until configuration or token changes. |
| `404 Not Found` | The subscription is absent locally, an update has no active correlated PCF AppSession, or the path's second segment is not `subscriptions`. | Check the id and the path. A subscription that worked before a NEF restart is gone for good — create a new one. |
| `405 Method Not Allowed` | The method is not supported on that path — a `POST` to an item URI, or a `PUT`/`PATCH`/`DELETE` with no `{subscriptionId}`. | Use one of the methods in the endpoint table. |
| `422 Unprocessable Entity` | On `POST` and `PUT`: a generated-model `validate()` failure, the path identity empty or over 256 characters (reported as `afId`), or `notificationDestination` over 2048 characters. `PATCH` reports the same validation failure as a `400` instead. | The `detail` names the field. Correct it; do not retry unchanged. |
| `429 Too Many Requests` | The token-bucket rate limiter rejected the request before any application handling. | Back off exponentially and retry. There is no `Retry-After` header. |
| `500 Internal Server Error` | PCF discovery or request failure, any non-2xx PCF response **including the TS-defined `303`**, or a missing or invalid AppSession ID. Provisional create state has been removed. | Nothing was created in NEF; the request is safe to retry. If it persists, check PCF and NRF — and note that a PCF answering `303` (a legitimate TS 29.514 response) lands here permanently, not transiently. |
| `503 Service Unavailable` | NEF is draining for shutdown, or the deferred dispatcher queue is full or stopped. | Retry after a short delay, or against another NEF instance. Nothing was created. |

A PCF create failure and an unknown QoS reference are **not** separate `503` branches on this
API: a rejected PCF create collapses into the rollback-and-`500` path above. This differs
from the Traffic Influence API, which answers the same class of PCF failure with `502`.

**DELETE exception.** `DELETE` responses carry no body at all — see
[DELETE](#delete--delete-subscription).

---

## Related pages

- [Architecture](../ARCHITECTURE.md) — request path, threading model and concurrency invariants
- [API Overview](overview.md) — authentication, error format, HTTP/2 requirements
- [Monitoring Event API](monitoring-event.md) — UE-level event subscriptions over T8
- [BDT Policy API](bdt-policy.md) — background data transfer scheduling
- [Traffic Influence API](traffic-influence.md) — routing and uplink classifier control
