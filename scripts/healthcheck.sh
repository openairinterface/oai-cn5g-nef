#!/bin/bash
set -eo pipefail

STATUS=0
NEF_INTERFACE_NAME_FOR_SBI=$(yq '.nfs.nef.sbi.interface_name // .local.sbi.interface_name' /openair-nef/etc/config.yaml 2>/dev/null || echo "eth0")
NEF_INTERFACE_PORT_FOR_SBI=$(yq '.nfs.nef.sbi.port // .local.sbi.port' /openair-nef/etc/config.yaml 2>/dev/null || echo "8080")

NEF_IP_SBI_INTERFACE=$(ifconfig "$NEF_INTERFACE_NAME_FOR_SBI" 2>/dev/null | grep inet | awk '{print $2}' | head -1)

if [[ -z "$NEF_IP_SBI_INTERFACE" ]]; then
    # Fall back to checking any listening port if interface lookup fails
    NEF_IP_SBI_INTERFACE="0.0.0.0"
fi

NEF_SBI_PORT_STATUS=$(netstat -tnpl 2>/dev/null | grep -o "$NEF_IP_SBI_INTERFACE:$NEF_INTERFACE_PORT_FOR_SBI" || true)

if [[ -z "$NEF_SBI_PORT_STATUS" ]]; then
    # Also check wildcard bind
    NEF_SBI_PORT_STATUS=$(netstat -tnpl 2>/dev/null | grep ":$NEF_INTERFACE_PORT_FOR_SBI " || true)
fi

if [[ -z "$NEF_SBI_PORT_STATUS" ]]; then
    STATUS=1
    echo "Healthcheck error: UNHEALTHY SBI TCP/HTTP port $NEF_INTERFACE_PORT_FOR_SBI is not listening."
fi

exit $STATUS
