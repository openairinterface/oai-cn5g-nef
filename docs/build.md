# Building OAI NEF from Source

This page covers native and Docker builds of the NEF binary. The [README](../README.md) is the
authoritative quick reference for the same commands; where this page and the README overlap, the
README wins. Read it first if you only need the shortest path from clone to running binary.

## 1. Supported Platforms

The authoritative list is `check_supported_distribution()` in the shared build helper
(`build/common-build/installation/build_helper`), summarised in the
[README](../README.md#supported-platforms):

| Operating System | Versions |
|------------------|----------|
| Ubuntu | 20.04, 22.04, 24.04 |
| RHEL | 9.3 – 9.8 |
| Rocky Linux | 9.3 – 9.6 |

RHEL 8 support was dropped in v2.2.1 (see [CHANGELOG.md](../CHANGELOG.md)); RHEL 9 was added in the
same release. Container images are built for Ubuntu (`docker/Dockerfile.nef.ubuntu`) and RHEL 9
(`docker/Dockerfile.nef.rhel9`). A Rocky 8 Dockerfile (`docker/Dockerfile.nef.rocky8`) is also
present, but Rocky 8 is not in the supported-distribution list above.

## 2. Prerequisites

Only a handful of tools must be present before running the build script. The script installs every
library dependency itself (see §4).

| Tool | Notes |
|------|-------|
| `cmake` | `build/nef/CMakeLists.txt` sets `cmake_minimum_required(VERSION 3.0.2)`. |
| `g++` or `clang++` | The project is C++17 (`CMAKE_CXX_STANDARD 17`, enforced). |
| `git` | Required to initialise the three submodules. |
| `sudo` / root | Needed for the `--install-deps` package-installation step. |

## 3. Clone the Repository

```bash
git clone https://github.com/openairinterface/oai-cn5g-nef.git
cd oai-cn5g-nef
git submodule update --init --recursive
```

Three submodules are used (see `.gitmodules`):

| Submodule | Contents |
|-----------|----------|
| `src/common-src` | shared C++ sources and the generated 3GPP model classes |
| `build/common-build` | build helpers and third-party recipes (nghttp2, spdlog, fmt, yaml-cpp, pistache) |
| `ci-scripts/common` | shared CI helpers |

If you cloned an older working copy, the `.gitmodules` section for the shared sources was renamed
from `src/oai-cn5g-common-src` to `src/common-src`. Run `git submodule sync --recursive` before
`git submodule update --init --recursive`; without the sync the clone keeps using the old path and
silently misses the pinned revision. See [README — Getting the source](../README.md#getting-the-source).

## 4. Install Dependencies

The build script installs all build and runtime dependencies. Run it once per machine (or per new
OS image):

```bash
./build/scripts/build_nef --install-deps --force
```

`--install-deps` (`-I`) installs the dependencies and then exits **without building**. `--force`
(`-f`) makes package installation non-interactive.

The exact package set lives in `build/scripts/build_helper.nef`. In outline, the script installs
compilers and build tools plus system libraries (OpenSSL/nettle, Boost, libcurl, libevent,
libxml2, and others) via the platform package manager, and builds `fmt`, `spdlog`, `pistache`,
`nghttp2` and `yaml-cpp` from source. NEF reads its configuration from YAML at runtime (via
`YAML::LoadFile`); it does not use libconfig for its configuration.

## 5. Build the Binary

```bash
# Release build, parallel jobs
./build/scripts/build_nef --clean --build-type Release --jobs

# Debug build
./build/scripts/build_nef --clean --build-type Debug --jobs
```

A successful build produces the binary at:

```
build/nef/build/oai_nef
```

It is also installed to `/usr/local/bin/oai_nef`, so it is on `PATH`.

> **Use `Release` for anything you intend to measure.** AddressSanitizer is linked into `Debug`
> and into the default `RelWithDebInfo` builds (`src/oai-nef/CMakeLists.txt`), which makes those
> binaries large and their timings meaningless.

## 6. Build Options Reference

All options are passed to `build/scripts/build_nef`. `build_nef --help` lists them; the ones that
matter:

| Option | Argument | Description |
|--------|----------|-------------|
| `-b`, `--build-type` | `Debug` \| `Release` \| `RelWithDebInfo` \| `MinSizeRel` | CMake build type. The CMake default when none is given is `RelWithDebInfo`. |
| `-c`, `--clean` | — | Remove `build/nef/build` and build from scratch. Use after changing `CMakeLists.txt` or switching build type. |
| `-f`, `--force` | — | Non-interactive package installation (used with `--install-deps` / `--install-min-deps`). |
| `-I`, `--install-deps` | — | Install build + runtime dependencies, then exit without building. |
| `-i`, `--install-min-deps` | — | Install runtime dependencies only — the minimal set to run a pre-built binary. Used in the Docker final stage. |
| `-j`, `--jobs` | — | Parallel build (`-j$(nproc)`). |
| `-v`, `--verbose` | — | Verbose make (`VERBOSE=1`). |
| `-V`, `--Verbose` | — | Verbose CMake configuration output. |

## 7. Run the Binary Directly

After building, start NEF with a configuration file:

```bash
oai_nef -c etc/config.yaml -o
```

### CLI Options

| Flag | Argument | Description |
|------|----------|-------------|
| `-c`, `--config` | `<file>` | Read the configuration from this YAML file. This is the only mandatory flag. |
| `-o`, `--stdoutlog` | — | Send the application logs to stdout. |
| `-r`, `--rotatelog` | — | Send the application logs to a rotating file in the working directory. |
| `-h`, `--help` | — | Print help and exit. |

NEF binds to the interface and port from the config file — `nfs.nef.sbi.interface_name` and
`nfs.nef.sbi.port` (`8080` in the template) — and uses the API version prefix from
`nfs.nef.sbi.api_version` (`v1`). The server is cleartext HTTP/2 (h2c) only; there is no
application-layer TLS and no separate admin port. Verify it is running:

```bash
curl --http2-prior-knowledge http://NEF_ADDR:8080/health
```

A healthy NEF answers `200` with `{"status":"ok", ...}`. See
[Operational Endpoints](api-reference/operational-endpoints.md) for the full response.

## 8. Build with Docker

Each Dockerfile is a three-stage build (base → builder → runtime) that keeps the final image
small. Three variants are provided:

| Dockerfile | Target Platform |
|------------|-----------------|
| `docker/Dockerfile.nef.ubuntu` | Ubuntu (`ubuntu:focal` by default; override with `BASE_IMAGE`) |
| `docker/Dockerfile.nef.rhel9` | Red Hat Enterprise Linux 9 |
| `docker/Dockerfile.nef.rocky8` | Rocky Linux 8 |

### Build the Ubuntu Image

```bash
# Default base: ubuntu:focal (20.04)
docker build \
  --target oai-nef \
  --file docker/Dockerfile.nef.ubuntu \
  --tag oai-nef:develop \
  .

# Override to Ubuntu 22.04 (jammy)
docker build \
  --target oai-nef \
  --file docker/Dockerfile.nef.ubuntu \
  --build-arg BASE_IMAGE=ubuntu:jammy \
  --tag oai-nef:jammy \
  .
```

### Multi-Stage Build Overview

**Stage 1 — `oai-nef-base`**

Starts from the base OS image (`ubuntu:focal` by default). Copies only the build scripts,
`build/common-build`, and the top-level `build/nef/CMakeLists.txt`, then runs
`./build_nef --install-deps --force`. This stage caches dependency installation, so rebuilding
source does not invalidate it unless the build scripts or that `CMakeLists.txt` change.

**Stage 2 — `oai-nef-builder`**

Extends `oai-nef-base`, copies the full source tree, and runs:

```bash
./build_nef --clean --Verbose --build-type Release --jobs
```

The binary is built directly as `oai_nef` at `build/nef/build/oai_nef`. The stage then patches the
shared `entrypoint.py` for NEF (replacing the `openair-nf-root-folder` and `nf-config-file`
placeholders with `openair-nef` and `config.yaml`).

**Stage 3 — `oai-nef` (final runtime image)**

Starts fresh from the base OS image and installs only the runtime libraries. Copies from the
builder stage:

- the `oai_nef` binary (into `/openair-nef/bin`)
- `entrypoint.py` and `scripts/healthcheck.sh`
- the from-source shared libraries (`libnghttp2.so.14`, `libnghttp2_asio.so.1`, `libpistache.so`,
  `libspdlog.so`, `libfmt.so`)
- the configuration template `etc/config.yaml`

The final image exposes `80/tcp` and `9090/tcp`, and ships a `HEALTHCHECK` that runs
`healthcheck.sh` every 10 seconds. Its entrypoint is `entrypoint.py`, and its command is
`oai_nef -c /openair-nef/etc/config.yaml -o`.

### Run the Container

The image ships a working `config.yaml`. To run it as-is:

```bash
docker run --rm -p 8080:8080 oai-nef:develop
```

To supply your own configuration, mount it and set `MOUNT_CONFIG=yes` so the entrypoint uses the
file verbatim rather than templating it:

```bash
docker run --rm \
  -p 8080:8080 \
  -e MOUNT_CONFIG=yes \
  -v "$(pwd)/etc/config.yaml:/openair-nef/etc/config.yaml:ro" \
  oai-nef:develop
```

## 9. CMake Build System

The CMake entry point for the NEF binary is `build/nef/CMakeLists.txt`. Key facts:

- **C++ standard**: C++17, set with `CMAKE_CXX_STANDARD 17` and `CMAKE_CXX_STANDARD_REQUIRED True`.
- **Default build type**: `RelWithDebInfo` when none is specified.
- **Sources**: it includes `src/oai-nef/CMakeLists.txt` (which assembles the `oai_nef` binary:
  `main.cpp`, `options.cpp`, and the app/api-server/common-src sources) and adds `src/nef_app`.
- **Build artifacts** land in `build/nef/build/`; the final binary is `build/nef/build/oai_nef`.

### CMake options defined by this repository

| Option | Default | Meaning |
|--------|---------|---------|
| `NEF_BUILD_TESTS` | `OFF` | Configure the `test/` tree and register its CTest script guards. |
| `NEF_BUILD_H2C_HARNESS` | `OFF` | Build the standalone h2c wire-diff harness. |
| `NEF_BUILD_GTESTS` | `OFF` | Build the GoogleTest binaries — requires a `reflectcpp` target this tree does not provide. |
| `NEF_BUILD_STALE_TESTS` | `OFF` | Build the tests that do not yet compile against current source. |

### Tests

The test tree is off by default. Enabling `NEF_BUILD_TESTS` registers two shell-based CTest
guards; they need no compilation, no GoogleTest and no third-party test dependency. Both guard
scripts (`test/nef_async_dispatch_static_guard.sh` and `test/nef_cont_policy_mapping_guard.sh`)
are present in the repository.

```bash
mkdir -p build/nef/build && cd build/nef/build
cmake -DCMAKE_BUILD_TYPE=Release -DNEF_BUILD_TESTS=ON ..
ctest --output-on-failure
```

| Test | Pins |
|------|------|
| `nef_async_dispatch_static_guard` | `nef_app_adapter` stays the sole request-path entry to `nef_app`. |
| `nef_cont_policy_mapping_guard` | each routed continuation applies its declared southbound-failure policy. |

Both guards can also run directly from the repository root, without configuring CMake:

```bash
bash test/nef_async_dispatch_static_guard.sh src
bash test/nef_cont_policy_mapping_guard.sh src
```

### The h2c wire-diff harness

The harness drives the real HTTP/2 server in-process over h2c and compares every response against
a committed baseline, so a response-path refactor can be shown to change nothing observable:

```bash
mkdir -p build/nef/build && cd build/nef/build
cmake -DCMAKE_BUILD_TYPE=Release -DNEF_BUILD_TESTS=ON -DNEF_BUILD_H2C_HARNESS=ON ..
make -j"$(nproc)" nef_h2c_integration_test
ctest -R nef_h2c --output-on-failure
```

Run the binary with `--update-baseline` to recapture the baseline instead of comparing.

### GoogleTest binaries

`NEF_BUILD_GTESTS=ON` fails at configure time. Every GoogleTest executable in
`test/CMakeLists.txt` links a `reflectcpp` target, and nothing in this tree declares one (no
`find_package`, no `FetchContent`); they also require C++20 while the NF builds as C++17. These
binaries therefore cannot be built here. See the comment block in `test/CMakeLists.txt`.
