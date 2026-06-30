#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:?repo root required}

server_cpp="$repo_root/api-server/nef-http2-server.cpp"
server_h="$repo_root/api-server/nef-http2-server.h"
adapter_cpp="$repo_root/nef_app/nef_app_adapter.cpp"
adapter_h="$repo_root/nef_app/nef_app_adapter.hpp"
nef_app_h="$repo_root/nef_app/nef_app.hpp"

if grep -R -n "NEF_DISABLE_ASYNC_DISPATCH" "$server_cpp" "$server_h" "$nef_app_h"; then
  echo "NEF_DISABLE_ASYNC_DISPATCH must not appear in server request path or nef_app comments" >&2
  exit 1
fi

if grep -E -n "m_nef_app->(set_request_bearer_token|clear_request_bearer_token|handle_)" "$server_cpp"; then
  echo "nef-http2-server.cpp must not directly call request-path nef_app handlers or token APIs" >&2
  exit 1
fi

if grep -R -n "m_async\|async==false\|async == false" "$adapter_cpp" "$adapter_h"; then
  echo "nef_app_adapter inline synchronous mode must not be reintroduced" >&2
  exit 1
fi
