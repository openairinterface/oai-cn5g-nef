<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# RELEASE NOTES:

## v2.2.1 -- April 2026

* Licensing and documentation
  - Re-license the project from OAI Public License v1.1 to CSSL v1.0
  - Re-license documentation under CC-BY-4.0 and orchestration/CI assets under MIT
  - Add `NOTICE`, `LICENSES/`, and related contribution/documentation updates
* Add support for RHEL 9 and drop support for RHEL 8

## v1.5.1 -- May 2023

* Code Refactoring for:
  * Logging mechanism (runtime log level selection)
  * Installation / build scripts
  * Continuous Integration scripts
* Published image on Docker-Hub is using now Ubuntu-20 as base image
  * We will soon obsolete the build system for Ubuntu18.04

## v1.5.0 -- January 2023

* feat(fqdn): giving some time for FQDN resolution
* Docker image improvements
* Fixed docker exit by catching SIGTERM
* release mode does not use libasan anymore --> allocation of 20T virtual memory is no longer done
* Ubuntu22 and cgroup2 support

## v1.4.0 -- April 2022

* Initial release

