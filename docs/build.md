# Building OAI NEF from Source

## 1. Supported Platforms

| Operating System | Version | Status |
|------------------|---------|--------|
| Ubuntu | 20.04 LTS (Focal Fossa) | Tested — CI-validated |
| Ubuntu | 22.04 LTS (Jammy Jellyfish) | Tested — CI-validated |
| Ubuntu | 18.04 LTS (Bionic Beaver) | Supported |
| Red Hat Enterprise Linux | 8 | Supported |
| Rocky Linux | 8 | Supported |

Docker images are available for Ubuntu, RHEL 8, and Rocky Linux 8 (see §8).

## 2. Prerequisites

The following tools must be present **before** running the build script. Library dependencies (libssl, Boost, nghttp2, libevent, spdlog, fmt, and others) are installed automatically by the build script during the `--install-deps` phase.

| Tool | Minimum Version | Notes |
|------|-----------------|-------|
| `cmake` | 3.0.2 | CMake 3.13+ recommended for `--build` parallel flag support |
| `g++` or `clang++` | GCC 8 / Clang 7 | C++17 standard (`-std=c++17`) is enforced by `CMakeLists.txt` |
| `git` | Any recent | Required to initialize the `common-src` and `common-build` submodules |
| `sudo` / root | — | Needed for the `--install-deps` package installation step |

## 3. Clone the Repository

Clone with `--recurse-submodules` to initialize both the `build/common-build` and `src/common-src` submodules in a single step:

```bash
git clone https://gitlab.eurecom.fr/oai/cn5g/oai-cn5g-nef.git
cd oai-cn5g-nef
git submodule update --init --recursive
```

If you cloned without the flag, initialize submodules manually:

```bash
git submodule update --init --recursive
```

Verify that `src/common-src/` and `build/common-build/` are populated before proceeding.

## 4. Install Dependencies

The build script handles all library and tool dependencies. Run the following once per machine (or per new OS image):

```bash
cd build/scripts
sudo ./build_nef --install-deps --force
```

The `--force` flag reinstalls packages even if they appear to be already present. This is recommended the first time and after an OS upgrade. The script installs:

- Build tools: `build-essential`, `cmake`, `pkg-config`
- TLS / crypto: `libssl-dev`, `libgnutls28-dev`
- Boost: `libboost-all-dev`
- HTTP/2: `libnghttp2-dev` (also built from source when the distro version is too old)
- Event loop: `libevent-dev`, `libevent-pthreads-*`
- Logging: `libspdlog-dev`, `libfmt-dev`
- Config parsing: `libconfig++-dev`, `libyaml-cpp-dev`

## 5. Build the Binary

```bash
cd build/scripts
./build_nef --build-type Release --jobs $(nproc)
```

A successful build produces:

```
build/nef/build/oai_nef
```

The binary is statically linked against libraries built from source (nghttp2, spdlog, fmt, pistache) and dynamically linked against system libraries. Run `ldd build/nef/build/oai_nef` to inspect runtime dependencies.

## 6. Build Options Reference

All options are passed to `build/scripts/build_nef`.

| Option | Argument | Description |
|--------|----------|-------------|
| `--build-type` | `Debug` \| `Release` \| `RelWithDebInfo` \| `MinSizeRel` | CMake build type. Default: `RelWithDebInfo` (optimized + debug info). Use `Debug` for development; `Release` for production images. |
| `--jobs` | `N` | Number of parallel compile jobs passed to `make -j N`. Omit to let `make` choose. Use `$(nproc)` for maximum parallelism. |
| `--verbose` | — | Prints each build step as it executes (equivalent to `make VERBOSE=1` output summary). |
| `--Verbose` | — | Full CMake verbose mode: shows every compiler invocation with all flags. Useful for diagnosing include path or flag problems. |
| `--clean` | — | Removes the CMake build directory before configuring. Use after changing CMakeLists.txt or switching build type. |
| `--auto-test` | — | Enables the automated test suite (CTest) after the build. Runs unit tests found under `build/nef/`. |
| `--install-deps` | — | Installs the full set of build-time and runtime dependencies via the system package manager, then builds from source any libraries not available as packages. |
| `--install-min-deps` | — | Installs runtime dependencies only — the minimal set needed to run a pre-built binary. Used in the Docker final-stage image. |
| `--force` | — | Combined with `--install-deps` or `--install-min-deps`: forces reinstallation even when packages are already present. |

## 7. Run the Binary Directly

After building, start NEF with a configuration file:

```bash
./build/nef/build/oai_nef \
  --config etc/config.yaml \
  --log-stdout
```

### CLI Options

| Option | Argument | Description |
|--------|----------|-------------|
| `--config` | `<path>` | Path to the YAML configuration file. **Required.** |
| `--log-stdout` | — | Write log output to standard output in addition to (or instead of) the rotating log file. |
| `--log-rot-file` | `<path>` | Write logs to a rotating file at the given path. Can be combined with `--log-stdout`. |

NEF binds to the interface and port configured in `nef.sbi.interface_name` and `nef.sbi.port` (default `8080`). Verify it is running:

```bash
curl -s http://localhost:8080/health | python3 -m json.tool
```

Expected response: `{"status": "OK"}` with HTTP 200.

## 8. Build with Docker

Docker images are built using multi-stage Dockerfiles to keep the final runtime image small. Three variants are provided:

| Dockerfile | Target Platform |
|------------|----------------|
| `docker/Dockerfile.nef.ubuntu` | Ubuntu 20.04 or 22.04 (set via `BASE_IMAGE` arg) |
| `docker/Dockerfile.nef.rhel8` | Red Hat Enterprise Linux 8 |
| `docker/Dockerfile.nef.rocky8` | Rocky Linux 8 |

### Build the Ubuntu Image

```bash
# Default: Ubuntu 20.04 (focal)
docker build \
  -f docker/Dockerfile.nef.ubuntu \
  -t oai-nef:latest \
  .

# Override to Ubuntu 22.04 (jammy)
docker build \
  -f docker/Dockerfile.nef.ubuntu \
  --build-arg BASE_IMAGE=ubuntu:jammy \
  -t oai-nef:jammy \
  .
```

### Multi-Stage Build Overview

The Dockerfile defines three stages:

**Stage 1 — `oai-nef-base`**

Starts from the base OS image (`ubuntu:focal` by default). Installs `git`, configures git settings, copies only the build scripts and the top-level `CMakeLists.txt`, then runs:

```bash
./build_nef --install-deps --force
```

This stage caches all dependency installation. Rebuilding source code does not invalidate this layer unless `build/scripts/build_nef` or `CMakeLists.txt` changes.

**Stage 2 — `oai-nef-builder`**

Extends `oai-nef-base`. Copies the full source tree and runs:

```bash
./build_nef --clean --Verbose --build-type Release --jobs
```

After a successful build the binary is renamed from `nef` to `oai_nef` and the Docker entrypoint script is patched to reference the correct paths.

**Stage 3 — `oai-nef` (final runtime image)**

Starts fresh from the base OS image. Installs only the runtime libraries needed to execute the binary (Boost, libevent, libnettle, libcurl-gnutls, etc. — version wildcards cover both Ubuntu 20 and 22). Copies from the builder stage:

- `oai_nef` binary
- `entrypoint.py` (Jinja2-based config templating entry point)
- `healthcheck.sh`
- Runtime shared libraries built from source (`libnghttp2.so.14`, `libpistache.so`, `libspdlog.so`, `libfmt.so`)
- Default configuration template (`etc/nef.conf`)

The final image exposes ports `8080/tcp` (SBI API) and `9090/tcp` and ships a `HEALTHCHECK` that calls `healthcheck.sh` every 10 seconds.

### Run the Container

```bash
docker run --rm \
  -p 8080:8080 \
  -v $(pwd)/etc/config.yaml:/openair-nef/etc/nef.conf:ro \
  oai-nef:latest
```

## 9. CMake Build System

The root CMake entry point for the NEF binary is `build/nef/CMakeLists.txt`. Key facts:

- **C++ standard**: C++17 is enforced with `set(CMAKE_CXX_STANDARD 17)` and `set(CMAKE_CXX_STANDARD_REQUIRED ON)`.
- **Source subdirectories** added via `add_subdirectory`:
  - `src/oai-nef` — entry point (`main.cpp`, `options.cpp`)
  - `src/nef_app` — core business logic
  - `src/api-server` — HTTP/2 server
  - `src/common-src` — shared infrastructure submodule (config, logger)
- **External libraries** are located via `find_package` (Boost, OpenSSL, libevent) or via pkg-config (nghttp2, spdlog, fmt). Libraries not available as system packages are built from source by the `build_nef` script and installed under `/usr/local`.
- **Build artifacts** land in `build/nef/build/`. The final binary is `build/nef/build/oai_nef`.
- **Test targets** are registered via `gtest_discover_tests` in `test/CMakeLists.txt`. The following automated test executables are available:
  - `nef_jwt_test` — unit tests for JWT signing and validation (`src/nef_app/nef_jwt_auth.*`)
  - `nef_request_dispatcher_test` — unit tests for the bounded MPSC dispatch queue (`src/nef_app/nef_request_dispatcher.hpp`): verifies task execution, queue-full back-pressure, stop/drain ordering, bearer-token thread-local isolation, undersize-constructor warning, and post-stop dispatch rejection

  Run all tests after a successful build:

  ```bash
  cd build/nef/build
  ctest --output-on-failure
  ```

  Or run a single target directly:

  ```bash
  ./build/nef/build/nef_request_dispatcher_test
  ```

  Both targets require C++20 (`cxx_std_20`) and link against the `NEF` static library, `reflectcpp`, and `GTest::gtest_main`.
- **CTest** integration is available when `--auto-test` is passed to the build script; test executables are placed alongside the main binary.
