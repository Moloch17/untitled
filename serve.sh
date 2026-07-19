#!/usr/bin/env bash
# Starts the servers and drops you into the console.
#
#   ./serve.sh            # start (if needed) and open the console
#   ./serve.sh --rebuild  # rebuild the binaries first
#
# The console is a separate process talking to the servers over TCP, so typing
# in it can never stall the simulation.
set -euo pipefail
cd "$(dirname "$0")"

if [[ "${1:-}" == "--rebuild" ]]; then
    cmake --build --preset linux --target dbserver authserver worldserver console
    shift
fi

docker compose up -d

printf 'waiting for the servers'
for _ in $(seq 1 60); do
    if docker compose exec -T postgres pg_isready -U untitled -d untitled >/dev/null 2>&1; then
        printf '\n'
        break
    fi
    printf '.'
    sleep 1
done

exec ./build/bin/console "$@"
