#!/usr/bin/env bash
# index-cli — Remote client for index server
# Usage: index-cli <host> <port> <token> [command...]
#
# Interactive:  index-cli myserver.local 9077 <token>
# One-shot:     index-cli myserver.local 9077 <token> SEND reviewer "review this code"

set -euo pipefail

HOST="${1:?Usage: index-cli <host> <port> <token> [cmd...]}"
PORT="${2:?Usage: index-cli <host> <port> <token> [cmd...]}"
TOKEN="${3:?Usage: index-cli <host> <port> <token> [cmd...]}"
shift 3

# Use a persistent TCP connection via bash /dev/tcp or nc
if [[ $# -gt 0 ]]; then
    # One-shot mode
    CMD="$*"
    {
        echo "AUTH $TOKEN"
        sleep 0.2
        echo "$CMD"
        sleep 1
        echo "QUIT"
    } | nc -q 2 "$HOST" "$PORT" 2>/dev/null | tail -n +2
else
    # Interactive mode
    echo "Connecting to $HOST:$PORT..."
    exec 3<>/dev/tcp/"$HOST"/"$PORT" 2>/dev/null || {
        # Fallback to nc for systems without /dev/tcp
        echo "Using nc fallback. Type commands, Ctrl-C to exit."
        {
            echo "AUTH $TOKEN"
            cat  # stdin passthrough
        } | nc "$HOST" "$PORT"
        exit
    }

    # Read banner
    read -r banner <&3
    echo "$banner"

    # Authenticate
    echo "AUTH $TOKEN" >&3
    read -r auth_resp <&3
    echo "$auth_resp"

    if [[ "$auth_resp" != "OK"* ]]; then
        echo "Authentication failed."
        exec 3<&-
        exit 1
    fi

    # REPL
    echo "Connected. Commands: SEND LIST STATUS ASK TOKENS HELP QUIT"
    while true; do
        printf "> "
        read -r line || break
        [[ -z "$line" ]] && continue

        echo "$line" >&3
        read -r resp <&3
        echo "$resp"

        [[ "$line" == "QUIT" || "$line" == "quit" ]] && break
    done

    exec 3<&-
fi
