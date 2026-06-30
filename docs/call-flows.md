# Call Flows

This document shows the interactions between OAI NEF and external entities — AFs, NRF, AMF, SMF, PCF, and UDR — for key operations. Each diagram covers one complete flow from request initiation to final response, including error and expiry paths where relevant.

---

## 1. NRF Registration and Heartbeat

At startup, NEF registers its NF profile with NRF using a `PUT` to the NF management endpoint. The profile includes the NF type (`NEF`), IP address, SBI port, and the list of services NEF provides. Once registered, `task_manager` issues a periodic heartbeat `PATCH` to keep the registration alive. On graceful shutdown, NEF deregisters with a `DELETE`.

```mermaid
sequenceDiagram
    participant NEF
    participant NRF
    NEF->>NRF: PUT /nnrf-nfm/v1/nf-instances/{nfId} (register, NF profile)
    NRF-->>NEF: 201 Created
    loop Every heartbeat_interval
        NEF->>NRF: PATCH /nnrf-nfm/v1/nf-instances/{nfId} (heartbeat, status=REGISTERED)
        NRF-->>NEF: 200 OK
    end
    Note over NEF,NRF: On shutdown: DELETE /nnrf-nfm/v1/nf-instances/{nfId}
```

**Key points**:

- If the initial `PUT` fails (NRF unreachable), NEF retries with exponential backoff.
- If `nfStatus` in the `/health` response shows `UNDISCOVERABLE`, the heartbeat loop has stopped or the initial registration failed. Check `NRF_FQDN` and `NRF_PORT`.
- On abnormal exit (SIGKILL, crash), the `DELETE` is not sent. NRF will eventually expire the registration based on its own TTL policy.

---

## 2. Monitoring Event Subscription (with Expiry)

When an AF creates a monitoring event subscription, NEF validates the request, persists it in memory, and immediately proxies a corresponding `Namf_EventExposure_Subscribe` request southbound to AMF. When the AMF detects the subscribed event it delivers a notification to NEF's internal callback; NEF translates it and forwards it to the AF's `notificationURI`. When `monitorExpireTime` is reached, NEF automatically cancels both the southbound AMF subscription and its own record.

```mermaid
sequenceDiagram
    participant AF
    participant NEF
    participant AMF
    AF->>NEF: POST /3gpp-monitoring-event/v1/{afId}/subscriptions
    NEF->>NEF: Validate request, generate subscriptionId
    NEF->>AMF: POST /namf-evts/v1/subscriptions (Namf_EventExposure_Subscribe)
    AMF-->>NEF: 201 Created (amfSubId)
    NEF-->>AF: 201 Created (subscriptionId, self URI)
    Note over NEF: task_manager monitors monitorExpireTime
    AMF->>NEF: POST /nef-notify/v1/notify/{nf_sub_id} (event report)
    NEF->>AF: POST {notificationURI} (event notification)
    AF-->>NEF: 204 No Content
    NEF-->>AMF: 204 No Content
    Note over NEF: monitorExpireTime reached
    NEF->>AMF: DELETE /namf-evts/v1/subscriptions/{amfSubId}
    AMF-->>NEF: 204 No Content
    NEF->>NEF: Delete subscription record
    AF->>NEF: GET /3gpp-monitoring-event/v1/{afId}/subscriptions/{subId}
    NEF-->>AF: 404 Not Found (subscription expired)
```

**Key points**:

- The AF's `notificationURI` must be reachable from the NEF container. In Docker Compose deployments, use the Docker service name rather than `localhost`.
- If the AF returns anything other than `2xx` for the notification, NEF logs the error but does not retry in this release.
- If NEF restarts after creating the subscription, the in-memory record is lost. See [Resilience — In-Memory State](resilience.md#in-memory-state-and-restart-behavior).

---

## 3. Traffic Influence Policy

The Traffic Influence API allows AFs to steer traffic for specific UEs or application flows. NEF maps the northbound T8 subscription to a `Npcf_PolicyAuthorization` application session on PCF. Updates and deletions are propagated southbound to PCF with matching PATCH and DELETE operations.

```mermaid
sequenceDiagram
    participant AF
    participant NEF
    participant PCF
    AF->>NEF: POST /3gpp-traffic-influence/v1/{afId}/subscriptions
    NEF->>NEF: Validate, map to Npcf_PolicyAuthorization
    NEF->>PCF: POST /npcf-policyauthorization/v1/app-sessions
    PCF-->>NEF: 201 Created (appSessionId)
    NEF-->>AF: 201 Created (subscriptionId)
    AF->>NEF: PATCH .../subscriptions/{subId} (update policy)
    NEF->>PCF: PATCH /npcf-policyauthorization/v1/app-sessions/{appSessionId}
    PCF-->>NEF: 200 OK
    NEF-->>AF: 200 OK
    AF->>NEF: DELETE .../subscriptions/{subId}
    NEF->>PCF: DELETE /npcf-policyauthorization/v1/app-sessions/{appSessionId}
    PCF-->>NEF: 204 No Content
    NEF-->>AF: 204 No Content
```

**Key points**:

- NEF maintains the mapping between its own `subscriptionId` and the PCF `appSessionId` in memory.
- If PCF returns an error on `POST`, NEF returns the error to the AF and does not persist the subscription.
- PCF circuit-breaker state is independent of AMF and UDR circuit states.

---

## 4. PFD Management Atomic Batch (with Rollback)

PFD (Packet Flow Description) transactions are atomic: either all application PFDs in a `PUT` transaction succeed, or none are committed. If any individual UDR write fails, NEF issues compensating `DELETE` calls for all previously written PFDs in that transaction before returning an error to the AF.

```mermaid
sequenceDiagram
    participant AF
    participant NEF
    participant UDR
    AF->>NEF: PUT /3gpp-pfd-management/v1/{afId}/transactions/{transId}
    NEF->>UDR: PUT /nudr-dr/v2/application-data/pfds/app1
    UDR-->>NEF: 200 OK
    NEF->>UDR: PUT /nudr-dr/v2/application-data/pfds/app2
    UDR-->>NEF: 500 Internal Server Error
    Note over NEF: Rollback: compensating DELETE for app1
    NEF->>UDR: DELETE /nudr-dr/v2/application-data/pfds/app1
    UDR-->>NEF: 204 No Content
    NEF-->>AF: 400 Bad Request (atomicity violated, no partial commit)
```

**Key points**:

- Atomicity is enforced by `nef_pfd_atomicity` (see [Architecture](architecture.md#module-reference)).
- If the rollback `DELETE` also fails, NEF logs the inconsistency and still returns `400` to the AF. Manual reconciliation against UDR may be required.
- A successful transaction returns `200 OK` with the stored PFD objects.

---

## 5. Circuit Breaker State Transitions (NF Connectivity)

Each southbound NF connection (AMF, SMF, PCF, UDR) has an independent circuit breaker. When consecutive failures reach `failure_threshold`, the circuit trips to OPEN and NEF rejects all calls to that NF immediately — no network request is made — protecting both NEF and the failing NF from request pile-up. After `recovery_timeout` the circuit enters HALF-OPEN and allows one probe request through. A successful probe returns the circuit to CLOSED; a failed probe returns it to OPEN.

```mermaid
sequenceDiagram
    participant NEF
    participant AMF
    Note over NEF: Circuit: CLOSED (normal)
    NEF->>AMF: Request
    AMF-->>NEF: 503 Service Unavailable (fail #1)
    NEF->>AMF: Request
    AMF-->>NEF: 503 Service Unavailable (fail #2)
    Note over NEF: failure_threshold reached → OPEN
    NEF->>NEF: Reject all AMF calls immediately (no network traffic)
    Note over NEF: recovery_timeout elapsed → HALF-OPEN
    NEF->>AMF: Probe request
    AMF-->>NEF: 200 OK
    Note over NEF: probe success → CLOSED
```

**State transition diagram**:

```mermaid
stateDiagram-v2
    [*] --> CLOSED
    CLOSED --> OPEN : failure_threshold exceeded
    OPEN --> HALF_OPEN : recovery_timeout elapsed
    HALF_OPEN --> CLOSED : probe success
    HALF_OPEN --> OPEN : probe failure
```

**Key points**:

- Circuit breaker state is scoped **per NF instance (NF ID)**. An open AMF circuit does not affect the PCF or UDR circuits.
- When a circuit is OPEN, NEF returns `503 Service Unavailable` to the requesting AF immediately.
- Configuration parameters (`failure_threshold`, `recovery_timeout`) are described in [Resilience](resilience.md#configuration) and the [Configuration Reference](configuration-reference.md).

---

## 6. Async Dispatch (nef_app_adapter)

The HTTP/2 server always routes NEF requests through a bounded thread-pool dispatch queue instead of calling `nef_app` inline on the libevent worker thread. Two response delivery modes are available.

### 6.1 Option A — Synchronous Handoff (blocking wait)

Used by all 46 non-deferred handlers. The HTTP worker blocks on a `std::future` until the dispatcher worker completes the handler and resolves the promise.

```mermaid
sequenceDiagram
    participant EL as libevent EL
    participant HW as HTTP Worker
    participant Adp as nef_app_adapter
    participant DQ as Dispatcher Queue
    participant DW as Dispatcher Worker
    participant App as nef_app

    EL->>HW: on_frame_recv_callback (request)
    HW->>Adp: dispatch_*(params, bearer_token, sink)
    Note over Adp: creates promise/future pair,<br/>captures token by value
    Adp->>DQ: enqueue task lambda
    Adp-->>HW: dispatch_status::ok
    HW->>HW: fut.wait()  [blocks HW thread]
    DQ->>DW: dequeue task
    DW->>DW: execute_with_token(token, fn)
    DW->>App: handle_*(params, status, body)
    App-->>DW: return
    DW->>DW: sink(status, body) → promise.set_value()
    HW->>HW: fut.get() → unblocks
    HW->>EL: event_base_once → response_post_cb
    EL->>EL: send HTTP/2 response to client
```

**Key properties:**
- HTTP worker thread is tied up for the full handler duration (database + southbound calls).
- No extra synchronization beyond the `std::promise/future`. Suitable for all handlers where downstream latency is bounded.
- On `queue_full` or `stopped`, `dispatch_*` returns the error status and the server helper sends `503 Service Unavailable`.

### 6.2 Option B — Deferred Response (non-blocking)

Used by 6 handlers with high southbound latency: `handle_monitoring_event_subscribe`, `handle_qos_create`, `handle_ti_create`, `handle_ti_update`, `handle_ti_patch`, `handle_pfd_app_put`.

The HTTP worker returns immediately after enqueue. The dispatcher worker delivers the response by posting back to the libevent event loop via `event_base_once`.

```mermaid
sequenceDiagram
    participant EL as libevent EL
    participant HW as HTTP Worker
    participant Adp as nef_app_adapter
    participant DQ as Dispatcher Queue
    participant DW as Dispatcher Worker
    participant App as nef_app

    EL->>HW: on_frame_recv_callback (request)
    HW->>HW: res.make_deferred() → http2_deferred_response handle
    HW->>Adp: dispatch_*_async(params, token, std::move(handle))
    Note over Adp: wraps handle in shared_ptr,<br/>captures token by value
    Adp->>DQ: enqueue task lambda
    Adp-->>HW: returns immediately (no fut.wait())
    HW-->>EL: handler returns — HW free for next request

    DQ->>DW: dequeue task
    DW->>DW: execute_with_token(token, fn)
    DW->>App: handle_*(params, status, body)
    App-->>DW: return
    DW->>DW: sink(status, body) → deferred_handle.send()
    DW->>EL: event_base_once → response_post_cb
    EL->>EL: send HTTP/2 response to client

    Note over Adp,DW: If handle destroyed before send():<br/>destructor posts 500 Internal Error
```

**Key properties:**
- HTTP worker is freed immediately after `dispatch_*_async`; a busy southbound call does not block the worker thread pool.
- `http2_deferred_response::send()` is exactly-once (atomic CAS). Calling `send()` twice silently no-ops the second call.
- If the handle is dropped without calling `send()` (e.g., due to an exception in the dispatcher worker), the destructor posts a `500 Internal Server Error` to prevent the client from hanging.
- The pool worker lambda skips the default was-sent/500 guard and `response_post_cb` post when `was_deferred()` is true — the deferred handle owns posting.
- Bearer token is captured by value into the task lambda; `execute_with_token()` re-sets the thread-local on the dispatcher worker thread before calling the handler and clears it on exit. See [Security — Bearer Token Cross-Thread Safety](security.md#bearer-token-cross-thread-safety).

### 6.3 Mode Selection

| Handler | Dispatch mode | Reason |
|---|---|---|
| All DELETE, GET, and non-southbound POST | Option A (sync wait) | Bounded latency; simple |
| `handle_monitoring_event_subscribe` | Option B (deferred) | AMF event exposure southbound call |
| `handle_qos_create` | Option B (deferred) | PCF policy auth southbound call |
| `handle_ti_create`, `_update`, `_patch` | Option B (deferred) | UDR influence data write + SMF notify |
| `handle_pfd_app_put` | Option B (deferred) | UDR PFD data write |
| `handle_ti_list` | Option A (sync wait) | Pure in-memory; no southbound call |

Both modes share the same `nef_app_adapter` dispatch path and the same `nef_request_dispatcher` worker pool. The legacy `use_async_dispatch: false` inline adapter mode was removed; stale configs that set it now fail during parsing.
