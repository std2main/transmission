#!/bin/sh

set -eu

bool_json() {
    value="$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')"
    case "$value" in
        1|true|yes|on)
            printf 'true'
            ;;
        0|false|no|off|'')
            printf 'false'
            ;;
        *)
            printf 'Invalid boolean value: %s\n' "$1" >&2
            exit 1
            ;;
    esac
}

int_json() {
    value="${1:-}"
    case "$value" in
        ''|*[!0-9]*)
            printf 'Invalid integer value: %s\n' "$value" >&2
            exit 1
            ;;
        *)
            printf '%s' "$value"
            ;;
    esac
}

config_dir="${TRANSMISSION_HOME:-/config}"
download_dir="${TRANSMISSION_DOWNLOAD_DIR:-/downloads}"
incomplete_dir="${TRANSMISSION_INCOMPLETE_DIR:-/downloads/incomplete}"
watch_dir="${TRANSMISSION_WATCH_DIR:-/watch}"
settings_file="${config_dir}/settings.json"

mkdir -p "$config_dir" "$download_dir" "$incomplete_dir" "$watch_dir"

if [ ! -f "$settings_file" ]; then
    printf '{}\n' > "$settings_file"
fi

tmp_file="$(mktemp)"

jq \
    --arg download_dir "$download_dir" \
    --arg incomplete_dir "$incomplete_dir" \
    --arg watch_dir "$watch_dir" \
    --arg rpc_bind_address "${TRANSMISSION_RPC_BIND_ADDRESS:-0.0.0.0}" \
    --arg rpc_whitelist "${TRANSMISSION_RPC_WHITELIST:-127.0.0.1,::1}" \
    --argjson incomplete_dir_enabled "$(bool_json "${TRANSMISSION_INCOMPLETE_DIR_ENABLED:-false}")" \
    --argjson watch_dir_enabled "$(bool_json "${TRANSMISSION_WATCH_DIR_ENABLED:-false}")" \
    --argjson rpc_enabled "$(bool_json "${TRANSMISSION_RPC_ENABLED:-true}")" \
    --argjson rpc_whitelist_enabled "$(bool_json "${TRANSMISSION_RPC_WHITELIST_ENABLED:-false}")" \
    --argjson peer_port "$(int_json "${TRANSMISSION_PEER_PORT:-51413}")" \
    --argjson rpc_port "$(int_json "${TRANSMISSION_RPC_PORT:-9091}")" \
    --argjson torrent_quick_verify_enabled "$(bool_json "${TRANSMISSION_QUICK_VERIFY_ENABLED:-false}")" \
    --argjson torrent_quick_verify_fallback_enabled "$(bool_json "${TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED:-false}")" \
    '
    .download_dir = $download_dir
    | .incomplete_dir = $incomplete_dir
    | .incomplete_dir_enabled = $incomplete_dir_enabled
    | .watch_dir = $watch_dir
    | .watch_dir_enabled = $watch_dir_enabled
    | .rpc_enabled = $rpc_enabled
    | .rpc_bind_address = $rpc_bind_address
    | .rpc_port = $rpc_port
    | .rpc_whitelist = $rpc_whitelist
    | .rpc_whitelist_enabled = $rpc_whitelist_enabled
    | .peer_port = $peer_port
    | .torrent_quick_verify_enabled = $torrent_quick_verify_enabled
    | .torrent_quick_verify_fallback_enabled = $torrent_quick_verify_fallback_enabled
    ' \
    "$settings_file" > "$tmp_file"

mv "$tmp_file" "$settings_file"

exec "$@"
