# OAI Network Exposure Function (NEF) — User Guide

This guide covers the OpenAirInterface implementation of the 5G Core Network Exposure Function (NEF). It describes the REST APIs exposed to external applications, deployment and configuration procedures, security model, and operational constraints.

---

## What is NEF?

The Network Exposure Function (NEF) is a standardised 5G Core (5GC) element defined in 3GPP TS 23.501 v16.0.0, Section 6.2.5. NEF sits at the boundary between the 5G core network and external entities — Service Capability Servers (SCS) and Application Servers (AS) — providing a single, authenticated, north-bound interface through which third-party applications can observe and influence network behaviour.

NEF translates between the operator's internal Service Based Interface (SBI) protocols and the external T8 REST API defined by 3GPP TS 29.122 and TS 29.522. It enforces authentication and authorisation of all external callers, validates request payloads, proxies subscriptions to the relevant internal network functions (AMF, SMF, PCF, UDR), and delivers real-time notifications from those functions back to the subscribing application.

---

## Version & Release

| Property | Value |
|----------|-------|
| **Latest release** | v2.2.1 (see [CHANGELOG.md](../CHANGELOG.md)) |
| **3GPP Release** | Release 16 |
| **Reference Specification** | TS 23.501 v16.0.0, Section 6.2.5 |
| **HTTP Protocol** | HTTP/2 cleartext (h2c) |
| **SBI API Version** | v1 |

---

## Specification Coverage Matrix

| Spec | Feature | Implementation Status | Documentation Page |
|------|---------|----------------------|-------------------|
| TS 29.122 | Monitoring Event (T8 interface) | Implemented | [api-reference/monitoring-event.md](api-reference/monitoring-event.md) |
| TS 29.122 | BDT Policy (T8 interface) | Implemented | [api-reference/bdt-policy.md](api-reference/bdt-policy.md) |
| TS 29.122 | PFD Management (T8 interface) | Implemented | [api-reference/pfd-management.md](api-reference/pfd-management.md) |
| TS 29.522 | Traffic Influence | Implemented | [api-reference/traffic-influence.md](api-reference/traffic-influence.md) |
| TS 29.122 | QoS Monitoring (AS Session with QoS) | Implemented | [api-reference/qos-monitoring.md](api-reference/qos-monitoring.md) |
| TS 29.591 | Nnef_EventExposure (SBI) | Implemented | [api-reference/nnef-event-exposure.md](api-reference/nnef-event-exposure.md) |
| TS 29.522 | Analytics Exposure | Partially implemented | [api-reference/analytics-exposure.md](api-reference/analytics-exposure.md) |
| TS 29.551 | Nnef_PFDmanagement (SBI) | Implemented | [api-reference/nnef-pfd-management.md](api-reference/nnef-pfd-management.md) |
| TS 29.222 | CAPIF (Common API Framework for 3GPP Northbound APIs) | **Not Implemented** — spec files present for reference | See [Out-of-Scope Features](#out-of-scope-features) |
| TS 29.541 | Nnef_SMContext / Nnef_SMService | **Not Implemented** — spec files present for reference | See [Out-of-Scope Features](#out-of-scope-features) |

---

## Implemented Services

| Service Name | Spec | Base Path | Documentation Page |
|---|---|---|---|
| Nnef_EventExposure | TS 29.591 | `/nnef-eventexposure/v1/` | [api-reference/nnef-event-exposure.md](api-reference/nnef-event-exposure.md) |
| Monitoring Event (T8) | TS 29.122 | `/3gpp-monitoring-event/v1/` | [api-reference/monitoring-event.md](api-reference/monitoring-event.md) |
| Traffic Influence | TS 29.522 | `/3gpp-traffic-influence/v1/` | [api-reference/traffic-influence.md](api-reference/traffic-influence.md) |
| PFD Management (T8) | TS 29.122 | `/3gpp-pfd-management/v1/` | [api-reference/pfd-management.md](api-reference/pfd-management.md) |
| Nnef_PFDmanagement (SBI) | TS 29.551 | `/nnef-pfdmanagement/v1/` | [api-reference/nnef-pfd-management.md](api-reference/nnef-pfd-management.md) |
| QoS Monitoring | TS 29.122 | `/3gpp-as-session-with-qos/v1/` | [api-reference/qos-monitoring.md](api-reference/qos-monitoring.md) |
| BDT Policy Control | TS 29.122 | `/3gpp-bdt/v1/` | [api-reference/bdt-policy.md](api-reference/bdt-policy.md) |
| Analytics Exposure | TS 29.522 | `/3gpp-analyticsexposure/v1/` | [api-reference/analytics-exposure.md](api-reference/analytics-exposure.md) |

---

## Out-of-Scope Features

The following features are **not implemented** in this release:

- **CAPIF (TS 29.222)** — The repository includes 10 CAPIF specification YAML files (`TS29222_CAPIF_*.yaml`) in the `3gpp_specs/` directory for reference. No CAPIF endpoints are functional; no source handlers exist. See the CAPIF note below.
- **TS 29.541 Nnef_SMContext / Nnef_SMService** — `TS29541_Nnef_SMContext.yaml` and `TS29541_Nnef_SMService.yaml` are included in `3gpp_specs/` as specification references. No source handlers are implemented in this release.
- **Non-IP Data Delivery (NIDD)** — Not implemented.
- **UAS NF Functionality** — Not implemented.
- **EAS Deployment Functionality** — Not implemented.
- **NWDAF-backed Analytics** — There is no NWDAF client. The Analytics Exposure API (TS 29.522, `/3gpp-analyticsexposure/v1`) is partially implemented and served entirely from NEF-local state: subscription CRUD and the `/fetch` operation return the AF's own registered subscriptions, not analytics computed by the network. See [Analytics Exposure API](api-reference/analytics-exposure.md).
- **Kubernetes / Helm Deployment** — No Helm charts or Kubernetes manifests are included in this repository.
- **TLS / mTLS** — NEF operates on HTTP/2 cleartext (h2c) only. No TLS termination is performed at the application layer.
- **High Availability / Clustering** — Single-instance deployment only; no state replication or failover is supported.
- **Prometheus / OpenMetrics** — No metrics exposition endpoint is implemented.
- **Subscription Persistence** — All subscription state is held in memory. A process restart clears all active subscriptions.

---

## CAPIF Out-of-Scope Note

> **Note:** CAPIF (TS 29.222) — Not Implemented.
>
> The repository includes 10 CAPIF specification YAML files in `3gpp_specs/` for reference:
> `TS29222_CAPIF_Discover_Service_API.yaml`, `TS29222_CAPIF_Access_Control_Policy_API.yaml`,
> `TS29222_CAPIF_Security_API.yaml`, and 7 additional CAPIF files. These files are included
> as a specification reference only. No CAPIF endpoints are functional in this release and
> no source handlers exist for any CAPIF service.

---

## Quick Navigation

| Document | Description |
|----------|-------------|
| [Getting Started](getting-started.md) | Run NEF with Docker and make your first API call in under 5 minutes |
| [Architecture](ARCHITECTURE.md) | Request path, threading model, concurrency invariants, module map |
| [Build from Source](build.md) | CMake build prerequisites, dependency installation, build options |
| [Configuration Reference](configuration-reference.md) | All `etc/config.yaml` parameters, types, defaults, and constraints |
| [Security](security.md) | JWT, API key, AF whitelist, `insecure_dev_mode`, HTTP/2 hardening |
| [Deployment](deployment.md) | Docker, Docker Compose, environment variables, health checks |
| [API Reference — Overview](api-reference/overview.md) | Common patterns: auth, versioning, HTTP/2, error format, status codes |
| [Monitoring Event API](api-reference/monitoring-event.md) | TS 29.122 — Subscribe to UE events from AMF |
| [Traffic Influence API](api-reference/traffic-influence.md) | TS 29.522 — Control traffic routing policies via PCF |
| [PFD Management API](api-reference/pfd-management.md) | TS 29.122 — Manage Packet Flow Descriptions via UDR |
| [QoS Monitoring API](api-reference/qos-monitoring.md) | TS 29.122 — AS Session with QoS and QoS event monitoring |
| [BDT Policy API](api-reference/bdt-policy.md) | TS 29.122 — Background Data Transfer policy management |
| [Analytics Exposure API](api-reference/analytics-exposure.md) | TS 29.522 — Registry of analytics interest, served from NEF-local state (no NWDAF client; partial) |
| [Call Flows](call-flows.md) | Sequence diagrams for all major operations |
| [Resilience](resilience.md) | Circuit breaker, retry with backoff, rate limiting, graceful shutdown |
| [Troubleshooting](troubleshooting.md) | Common problems, log interpretation, FAQs |

---

## Version History

See [CHANGELOG.md](../CHANGELOG.md) for the full list of changes across all releases.

| Release | Notes |
|---------|-------|
| v2.2.1 | Current release. Relicensed to CSSL v1.0; added RHEL 9 support and dropped RHEL 8. |
| v1.5.1 | Build/CI refactoring; Docker Hub image moved to an Ubuntu 20 base. |
