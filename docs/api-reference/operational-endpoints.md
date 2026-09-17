<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Operational Endpoints

Two of NEF's routes belong to no 3GPP service. They exist so that an operator can tell whether NEF
is alive, and so that peer NFs have somewhere to deliver their notifications.

Read the [API Overview](overview.md) first for the HTTP/2 and authentication conventions that
apply here as well.

## At a glance

| Endpoint | Method | Who calls it | Answers |
|---|---|---|---|
| [`/health`](#get-health) | `GET` | operators, orchestrators, load balancers | `200` ok · `503` draining · `405` wrong method |
| [`/nef-notify/v1/notify/{nf_sub_id}`](#post-nef-notifyv1notifynf_sub_id) | `POST` | AMF, SMF, PCF — **not** AFs | `204` accepted · `400` unparseable body · `404` unknown subscription · `405` wrong method · `503` overloaded |

Neither endpoint authorizes its caller. `/health` is the only route in the whole server that skips
`begin_request()` entirely — no bearer-token extraction, no drain rejection, no rate limiting. The
notification receiver does run `begin_request()`, so it is drain-guarded and rate-limited, but the
bearer token it extracts is used only as the rate-limit key: nothing validates it. Access to that
path is controlled by the network, not by a credential.

---

## Health Check Endpoint

### `GET /health`

Unauthenticated liveness and readiness probe. It reports whether NEF is serving normally or
draining for shutdown, and it keeps answering throughout the drain — that is the point of
exempting it from the drain guard.

```bash
curl --http2-prior-knowledge http://oai-nef:8080/health
```

`--http2-prior-knowledge` is required. NEF is cleartext HTTP/2 only, and `/health` is no exception
— a plain HTTP/1.1 request gets no usable response. This has consequences for probe
configuration; see [Wiring it into a deployment](#wiring-it-into-a-deployment) below.

**`200 OK` — serving normally**

```json
{
  "status": "ok",
  "nf_type": "NEF",
  "instance_id": "d3c2b1a0-f9e8-4d7c-b6a5-9e8f7d6c5b4a",
  "uptime_seconds": 3821,
  "draining": false
}
```

**`503 Service Unavailable` — graceful shutdown in progress**

```json
{
  "status": "draining",
  "nf_type": "NEF"
}
```

The draining body carries only those two fields. `instance_id`, `uptime_seconds` and `draining`
are omitted.

Any method other than `GET` on this path returns `405 Method Not Allowed`.

#### Response fields

| Field | Type | Description |
|---|---|---|
| `status` | string | `"ok"` (HTTP 200) or `"draining"` (HTTP 503) |
| `nf_type` | string | Always `"NEF"` |
| `instance_id` | string (UUID) | NEF's NF instance ID, assigned at startup and constant for the process lifetime. 200 responses only. |
| `uptime_seconds` | integer | Seconds since the HTTP server was constructed. 200 responses only. |
| `draining` | boolean | Always `false` — the draining case is signalled by the 503 above. 200 responses only. |

The body is produced by `nef_health_check::make_response` in `src/common/nef_health_check.hpp`.

Note that `uptime_seconds` counts from server construction, not from process start, and
`instance_id` is the same value NEF registers with the NRF — useful for confirming which instance
answered you behind a load balancer.

#### Wiring it into a deployment

Because the endpoint is h2c-only, any probe that speaks HTTP/1.1 will fail against a perfectly
healthy NEF. That rules out Kubernetes `httpGet` probes and a `HEALTHCHECK` built on a plain
`curl` URL.

**What the container image actually does.** The shipped Dockerfiles use the script, not the
endpoint:

```dockerfile
HEALTHCHECK --interval=10s --timeout=15s --retries=6 \
  CMD /openair-nef/bin/healthcheck.sh
```

`scripts/healthcheck.sh` reads the SBI interface and port out of
`/openair-nef/etc/config.yaml` and checks that something is listening there. It never opens an
HTTP request, so it proves the process is up — not that it is ready, and not that it is draining.

**Kubernetes.** Use a `tcpSocket` probe, which is protocol-agnostic:

```yaml
livenessProbe:
  tcpSocket:
    port: 8080
  initialDelaySeconds: 10
  periodSeconds: 15
  failureThreshold: 3
readinessProbe:
  tcpSocket:
    port: 8080
  initialDelaySeconds: 5
  periodSeconds: 10
```

Or an `exec` probe running the shipped script, which is equivalent to what Docker runs:

```yaml
readinessProbe:
  exec:
    command: ["/openair-nef/bin/healthcheck.sh"]
  initialDelaySeconds: 5
  periodSeconds: 10
```

Neither form distinguishes draining from serving. To get that, the probe has to be an `exec` that
speaks h2c — an HTTP/2-capable client is not installed in the runtime image, so you would need to
add one.

**Load balancers.** Point the check at `GET http://oai-nef:8080/health` over HTTP/2 with prior
knowledge. Treat `200` as in-service; remove the instance from the pool on a probe failure or on a
`503` with `"status": "draining"`.

---

## Inbound Notification Endpoint

### `POST /nef-notify/v1/notify/{nf_sub_id}`

This is where the 5G core talks back to NEF. When NEF subscribes to a peer NF on behalf of an AF —
for example `Namf_EventExposure` towards AMF — it hands over this URL as the callback. The peer NF
`POST`s here when the subscribed event fires, and NEF translates the notification and forwards it
to the AF.

> **Not an AF-facing endpoint.** This is a machine-to-machine interface between NEF and the NFs it
> has subscribed to. AFs should never call it, and the path should not be reachable from outside
> the 5GC network segment.

The path is assembled at startup as `/nef-notify/` + the configured API version + `/notify/` +
`{nf_sub_id}`, so with the default `v1` it reads `/nef-notify/v1/notify/{nf_sub_id}`.

#### Path parameter

| Parameter | Type | Description |
|---|---|---|
| `nf_sub_id` | string | Internal subscription tracking ID that NEF assigned when it created the southbound NF subscription. Opaque to external callers. |

#### Request

The body varies by source NF:

| Source NF | Body format | TS reference |
|---|---|---|
| AMF | `Namf_EventExposure` notification body | TS 29.518 §5.2.6 |
| SMF | `Nsmf_EventExposure` notification body | TS 29.508 §4.2.6 |
| PCF | `Npcf_PolicyAuthorization` notification body | TS 29.514 §4.2.6 |

An empty body is legal on this route and is treated as an empty JSON object — it is one of the few
places in NEF where a missing body is not an error.

#### Responses

| Status | Meaning |
|---|---|
| `204 No Content` | NEF matched `nf_sub_id` and accepted the notification |
| `400 Bad Request` | The body was present but did not parse as JSON |
| `404 Not Found` | No subscription matches `nf_sub_id`, with detail `"No subscription found for notification id: <id>"`. The calling NF can use this to clean up its side |
| `405 Method Not Allowed` | Any method other than `POST` |
| `503 Service Unavailable` | The dispatcher rejected the task; detail `"Server is overloaded, please retry later"` |

#### What happens between the 204 and the AF

```
sequenceDiagram
    AMF->>NEF: POST /nef-notify/v1/notify/{nf_sub_id}
    NEF->>NEF: Look up AF subscription by nf_sub_id
    NEF->>AF: POST {notificationURI}
    AF-->>NEF: 204 No Content
    NEF-->>AMF: 204 No Content
```

1. AMF, SMF or PCF fires a southbound notification at the callback URI NEF registered.
2. NEF resolves `nf_sub_id` to the original AF subscription in its internal subscription table.
3. `nef_notification_mapper` translates the internal NF event format into the T8 northbound
   format.
4. NEF `POST`s the translated notification to the AF's `notificationURI`.
5. NEF returns `204 No Content` to the originating NF.

#### Examples

These show the shape of the traffic for debugging and integration tracing. AFs do not call this
endpoint.

**AMF → NEF**

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/nef-notify/v1/notify/sub-amf-001 \
  -H "Content-Type: application/json" \
  -d '{
    "subscriptionId": "sub-amf-001",
    "reportList": [
      {
        "type": "LOSS_OF_CONNECTIVITY",
        "supi": "imsi-208950000000001",
        "timeStamp": "2026-04-25T14:22:31Z"
      }
    ]
  }'
```

Expected response: `HTTP/2 204`.

**PCF → NEF**, for a QoS or traffic influence notification:

```bash
curl --http2-prior-knowledge \
  -X POST http://oai-nef:8080/nef-notify/v1/notify/sub-pcf-042 \
  -H "Content-Type: application/json" \
  -d '{
    "appSessionId": "pcf-appsession-9f3b2c1a",
    "evNotifs": [
      {
        "event": "QOS_NOTIF",
        "qosNotifType": "GUARANTEED",
        "flows": [{"flowNumber": 1, "packFiltResults": []}]
      }
    ]
  }'
```

#### Security

Keep `/nef-notify/v1/notify/` reachable only from the internal 5GC network segment where AMF, SMF
and PCF live, and enforce it with firewall rules, Kubernetes `NetworkPolicy`, or Docker network
segmentation. Network isolation is the only control on this path: as noted above, the route
extracts a bearer token but does not verify it, so anyone who can reach the port can inject a
notification for any `nf_sub_id` they can guess.

---

## Related pages

- [API Overview](overview.md) — common HTTP/2 and authentication conventions
- [Nnef_EventExposure SBI API](nnef-event-exposure.md) — internal NF event subscriptions
- [Monitoring Event API](monitoring-event.md) — the AF subscriptions that trigger southbound
  AMF subscriptions
