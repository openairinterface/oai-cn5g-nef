<!-- SPDX-License-Identifier: CC-BY-4.0 -->

------------------------------------------------------------------------------

                             OPENAIR-CN-5G
 An implementation of the 5G Core network by the OpenAirInterface community.

------------------------------------------------------------------------------

OPENAIR-CN-5G is an implementation of the 3GPP specifications for the 5G Core Network.
At the moment, it contains the following network elements:

* Access and Mobility Management Function (**AMF**)
* Authentication Server Management Function (**AUSF**)
* Network Repository Function (**NRF**)
* Session Management Function (**SMF**)
* Unified Data Management (**UDM**)
* Unified Data Repository (**UDR**)
* User Plane Function (**UPF**)
* Network Slicing Selection Function (**NSSF**)
* Network Data Analytics Function (**NWDAF**)
* Network Exposure Function (**NEF**)

Each has its own repository: this repository (`oai-cn5g-nef`) is meant for NEF.

# Licence info

The source code is distributed under `Collaborative Standards Software License v1.0 (CSSL v1.0)`.
For more details, visit the [OAI Website](https://openairinterface.org/oai-cssl/).

The full text of `Collaborative Standards Software License v1.0` is also included in the [LICENSE](LICENSE)
file at the root of this repository.

Certain files in the repository are using MIT License and documentation is distributed under
Creative Commons Attribution 4.0 International license.

Details on third-party software can be found in the [NOTICE](NOTICE) file.

# Where to start

The Openair-CN-5G NEF code is written, executed, and tested on UBUNTU server bionic version.
Other Linux distributions support will be added later on.

More details on the supported feature set is available on this [page](docs/FEATURE_SET.md).

# Collaborative work

This source code is managed through a GITLAB server, a collaborative development platform:

*  URL: [https://gitlab.eurecom.fr/oai/cn5g/oai-cn5g-nef](https://gitlab.eurecom.fr/oai/cn5g/oai-cn5g-nef).

Process is explained in [CONTRIBUTING](CONTRIBUTING.md) file.

# Contribution requests

In a general way, anybody who is willing can contribute on any part of the
code in any network component.

Contributions can be simple bugfixes, advices and remarks on the design,
architecture, coding/implementation.

# Release Notes

They are available on the [CHANGELOG](CHANGELOG.md) file.

## Quick start

This is the shortest path from a fresh clone to a running NEF on a
[supported platform](#supported-platforms). Each step is expanded further down the page.

```bash
# 1. Clone, with the three submodules
git clone https://github.com/openairinterface/oai-cn5g-nef.git
cd oai-cn5g-nef
git submodule update --init --recursive

# 2. Install build and runtime dependencies (once per machine, needs sudo)
./build/scripts/build_nef --install-deps --force

# 3. Build
./build/scripts/build_nef --clean --build-type Release --jobs

# 4. Run, logging to stdout
oai_nef -c etc/config.yaml -o
```

The binary lands at `build/nef/build/oai_nef` and is also installed to `/usr/local/bin/oai_nef`,
which is why step 4 finds it on `PATH`.

To check that it came up, ask it:

```bash
curl --http2-prior-knowledge http://127.0.0.1:8080/health
```

A healthy NEF answers `200` with `{"status":"ok", ...}`. The `--http2-prior-knowledge` flag is not
optional: NEF speaks cleartext HTTP/2 (h2c) and nothing else. See
[operational endpoints](docs/api-reference/operational-endpoints.md) for the full response.

Before exposing NEF to anything you care about, read [Security defaults](#security-defaults) — the
shipped `etc/config.yaml` has authentication disabled.

## Documentation

| Document | What it covers |
| -------- | -------------- |
| [Feature set](docs/FEATURE_SET.md) | Which 3GPP NEF capabilities are implemented |
| [Architecture](docs/ARCHITECTURE.md) | Request path, threading model, concurrency invariants, module map |
| [API reference](docs/api-reference/overview.md) | The northbound REST APIs, per service |
| [Generated code](docs/GENERATED-CODE.md) | Which sources are machine-generated, and what is known about regenerating them |
| [Release notes](CHANGELOG.md) | Version history |
| [Contributing](CONTRIBUTING.md) | Contribution and commit process |

## Getting the source

```bash
git clone https://github.com/openairinterface/oai-cn5g-nef.git
cd oai-cn5g-nef
git submodule update --init --recursive
```

Three submodules are used:

| Submodule | Contents |
| --------- | -------- |
| `src/common-src` | shared C++ sources and the generated 3GPP model classes |
| `build/common-build` | build helpers and third-party recipes, including nghttp2 |
| `ci-scripts/common` | shared CI helpers |

> **Updating an existing clone.** The `.gitmodules` section name for the shared sources was
> renamed from `src/oai-cn5g-common-src` to `src/common-src`. Section names key `.git/modules/`,
> so an existing clone must run `git submodule sync --recursive` before
> `git submodule update --init --recursive`. Without the sync it keeps using the old path and
> silently misses the pinned revision.

## Supported platforms

The authoritative list is `check_supported_distribution()` in
`build/common-build/installation/build_helper`:

* Ubuntu 20.04, 22.04, 24.04
* RHEL 9.3 – 9.8
* Rocky Linux 9.3 – 9.6

CI builds container images for Ubuntu (`docker/Dockerfile.nef.ubuntu`) and RHEL 9
(`docker/Dockerfile.nef.rhel9`). A Rocky 8 Dockerfile is also present, but Rocky 8 is not in the
supported-distribution list above.

## Building natively

The build is driven by `build/scripts/build_nef`, which wraps CMake. Run it from anywhere; it
resolves its own paths.

```bash
# once per machine: install build and runtime dependencies
./build/scripts/build_nef --install-deps --force

# build
./build/scripts/build_nef --clean --build-type Release --jobs
```

The binary lands at `build/nef/build/oai_nef` and is also copied to `/usr/local/bin/oai_nef`.

### Build options

`build_nef --help` lists every option. The ones that matter:

| Option | Effect |
| ------ | ------ |
| `-b`, `--build-type` | `Debug`, `Release`, `RelWithDebInfo` (the CMake default) or `MinSizeRel` |
| `-c`, `--clean` | remove `build/nef/build` and build from scratch |
| `-f`, `--force` | non-interactive package installation |
| `-I`, `--install-deps` | install build + runtime dependencies, then exit without building |
| `-i`, `--install-min-deps` | install runtime dependencies only |
| `-j`, `--jobs` | parallel build (`-j$(nproc)`) |
| `-v` / `-V` | verbose make / verbose CMake |

> **Use `Release` for anything you intend to measure.** AddressSanitizer is linked in for
> `Debug` **and** for the default `RelWithDebInfo` (`src/oai-nef/CMakeLists.txt`). That makes
> those binaries very large and their timings meaningless.

The project is C++17. `build/nef/CMakeLists.txt` sets `CMAKE_CXX_STANDARD 17`, and
`src/oai-nef/CMakeLists.txt` re-states `-std=c++17` in all four build-type flag sets.

### Invoking CMake directly

`build_nef` is a convenience wrapper; the CMake entry point is `build/nef/CMakeLists.txt`. This is
the same sequence the script runs:

```bash
mkdir -p build/nef/build && cd build/nef/build
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ..
make -j"$(nproc)" oai_nef
```

CMake options defined by this repository:

| Option | Default | Meaning |
| ------ | ------- | ------- |
| `NEF_BUILD_TESTS` | `OFF` | configure `test/` and register its CTest script guards |
| `NEF_BUILD_H2C_HARNESS` | `OFF` | build the standalone h2c wire-diff harness |
| `NEF_BUILD_GTESTS` | `OFF` | build the GoogleTest binaries — requires a `reflectcpp` target this tree does not provide |
| `NEF_BUILD_STALE_TESTS` | `OFF` | build the tests that do not yet compile against current source |

Defining `NEF_DISABLE_ASYNC_DISPATCH` at all is a hard `FATAL_ERROR`: NEF's HTTP handlers always
dispatch through `nef_app_adapter`.

## Building the container image

```bash
docker build --target oai-nef --tag oai-nef:develop --file docker/Dockerfile.nef.ubuntu .
```

Each Dockerfile is a three-stage build (base → builder → runtime). The runtime image exposes
`80/tcp` and `9090/tcp`, ships `scripts/healthcheck.sh` as its `HEALTHCHECK`, and starts
`oai_nef -c /openair-nef/etc/config.yaml -o` through `entrypoint.py`.

`.dockerignore` excludes the CI artifacts (`archives`, `src/oai_rules_result*`, `*.html`), the
local build outputs (`build/nef/build/`, `build/ext/`, `build/log/`) and `.ai-tmp/`. It
deliberately keeps `build/` as a whole and `.git`. The base stage copies `build/scripts`,
`build/common-build` and `build/nef/CMakeLists.txt` before `COPY .` runs, and the version banner
the NF prints at startup is stamped from `git` at configure time.

## Running

```bash
oai_nef -c etc/config.yaml -o
```

### Command-line flags

| Flag | Meaning |
| ---- | ------- |
| `-c`, `--config <file>` | read the configuration from this file (YAML). This is the only mandatory flag |
| `-o`, `--stdoutlog` | send the application logs to stdout |
| `-r`, `--rotatelog` | send the application logs to a file in the working directory |
| `-h`, `--help` | print help and exit |

### Configuration

`etc/config.yaml` is the template. It carries the log level, the NRF registration switch, the SBI
endpoints of the NRF/AMF/SMF/PCF/UDR/NEF, and the NEF-specific block: the supported northbound
service list, the AF whitelist, and the JWT secret.

NEF's own listening port comes from `nfs.nef.sbi.port` (`8080` in the template) and the API
version prefix from `nfs.nef.sbi.api_version` (`v1`). The server is cleartext HTTP/2 (h2c) only;
there is no application-layer TLS and no separate admin port.

### Security defaults

> With neither a JWT secret nor an AF whitelist configured, NEF is **fail-closed** and denies
> every request. `nef.security.insecure_dev_mode: true` switches it to fail-open and logs a loud
> warning at startup. The shipped template has it enabled — do not deploy it as-is.

See [Authentication](docs/api-reference/overview.md#authentication) for what a caller has to
present once you turn authentication on.

### Liveness and readiness

Two mechanisms, and they check different things:

* `GET /health` — unauthenticated, and the only route exempt from the drain guard and the rate
  limiter, so it keeps answering while NEF is shutting down. Reports `200 {"status":"ok"}` or
  `503 {"status":"draining"}`. See
  [operational endpoints](docs/api-reference/operational-endpoints.md).
* `scripts/healthcheck.sh` — what the container image runs as its `HEALTHCHECK`. It reads the SBI
  port out of `/openair-nef/etc/config.yaml` and checks that something is listening on it. It does
  not call `/health`, so it reports a live process rather than a ready one.

## Tests

The test tree is off by default. Enabling it registers two shell-based CTest guards; they need no
compilation, no GoogleTest and no third-party test dependency.

```bash
mkdir -p build/nef/build && cd build/nef/build
cmake -DCMAKE_BUILD_TYPE=Release -DNEF_BUILD_TESTS=ON ..
ctest --output-on-failure
```

| Test | Pins |
| ---- | ---- |
| `nef_async_dispatch_static_guard` | `nef_app_adapter` stays the sole request-path entry to `nef_app` |
| `nef_cont_policy_mapping_guard` | each routed continuation applies its declared southbound-failure policy |

Both can also be run directly, from the repository root, without configuring CMake at all:

```bash
bash test/nef_async_dispatch_static_guard.sh src
bash test/nef_cont_policy_mapping_guard.sh src
```

### The h2c wire-diff harness

The wire-diff harness is a separate option. It drives the real HTTP/2 server in-process over h2c
and compares every response against a committed baseline, so a refactor of the response path can
be proved to have changed nothing observable:

```bash
mkdir -p build/nef/build && cd build/nef/build
cmake -DCMAKE_BUILD_TYPE=Release -DNEF_BUILD_TESTS=ON -DNEF_BUILD_H2C_HARNESS=ON ..
make -j"$(nproc)" nef_h2c_integration_test
ctest -R nef_h2c --output-on-failure
```

Run the binary with `--update-baseline` to recapture the baseline instead of comparing against it.

### GoogleTest binaries

`NEF_BUILD_GTESTS=ON` fails at configure time unless a `reflectcpp` target is supplied. Nothing in
this tree declares one, so these binaries cannot be built here; see the comment block in
`test/CMakeLists.txt`.

## Repository structure

<pre>
oai-cn5g-nef
├── 3gpp_specs/       3GPP OpenAPI (YAML) inputs and the OpenAPI Generator jar.
│                     See docs/GENERATED-CODE.md -- nothing in the build invokes them.
├── build/
│   ├── common-build/ submodule: build helpers, third-party recipes (nghttp2, spdlog, fmt)
│   ├── nef/          top-level CMakeLists.txt; build output goes to build/nef/build/
│   └── scripts/      build_nef (entry point) and build_helper.nef (dependency lists)
├── ci-scripts/       Jenkins pipeline, image flattening, deployment sanity checks
│   └── common/       submodule: shared CI helpers
├── docker/           Dockerfile.nef.{ubuntu,rhel9,rocky8} -- three-stage image builds
├── docs/             this documentation set (see the table above)
│   ├── api-reference/  one page per northbound service
│   └── images/
├── etc/              config.yaml -- the runtime configuration template
├── scripts/          healthcheck.sh, used by the container HEALTHCHECK
├── src/
│   ├── api-server/   nef_http2_server: the route table, 12 routes and 50 HTTP handlers
│   ├── common/       header-only cross-cutting helpers (validation, rate limiting,
│   │                 JWT, response policy, audit, resilience). No .cpp files.
│   ├── common-src/   submodule: shared OAI sources + the generated 3GPP model classes
│   ├── nef_app/      the business logic: nef_app (8 translation units), nef_app_adapter,
│   │                 nef_client (southbound), the dispatcher and the notification queue
│   └── oai-nef/      main(), CLI options, and the CMakeLists that assembles the binary
└── test/             CTest script guards, the h2c wire-diff harness, GoogleTest binaries
</pre>

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how these fit together.
