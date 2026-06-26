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

log_level_to_int() {
    case "$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')" in
        0|off) printf '0' ;;
        1|critical) printf '1' ;;
        2|error) printf '2' ;;
        3|warn|warning) printf '3' ;;
        4|info) printf '4' ;;
        5|debug) printf '5' ;;
        6|trace) printf '6' ;;
        *) printf '2' ;; # default to error
    esac
}

log_level_to_str() {
    case "$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')" in
        0|off) printf 'off' ;;
        1|critical) printf 'critical' ;;
        2|error) printf 'error' ;;
        3|warn|warning) printf 'warn' ;;
        4|info) printf 'info' ;;
        5|debug) printf 'debug' ;;
        6|trace) printf 'trace' ;;
        *) printf 'error' ;; # default to error
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
    --arg torrent_verify_log_path "${TRANSMISSION_VERIFY_LOG_PATH:-/config/verify.log}" \
    --argjson message_level "$(log_level_to_int "${TRANSMISSION_LOG_LEVEL:-error}")" \
    '.["download-dir"] = $download_dir
    | .["incomplete-dir"] = $incomplete_dir
    | .["incomplete-dir-enabled"] = $incomplete_dir_enabled
    | .["watch-dir"] = $watch_dir
    | .["watch-dir-enabled"] = $watch_dir_enabled
    | .["rpc-enabled"] = $rpc_enabled
    | .["rpc-bind-address"] = $rpc_bind_address
    | .["rpc-port"] = $rpc_port
    | .["rpc-whitelist"] = $rpc_whitelist
    | .["rpc-whitelist-enabled"] = $rpc_whitelist_enabled
    | .["peer-port"] = $peer_port
    | .["torrent-quick-verify-enabled"] = $torrent_quick_verify_enabled
    | .["torrent-quick-verify-fallback-enabled"] = $torrent_quick_verify_fallback_enabled
    | .["torrent-verify-log-path"] = $torrent_verify_log_path
    | .["message-level"] = $message_level
    ' \
    "$settings_file" > "$tmp_file"

mv "$tmp_file" "$settings_file"

if [ "${1#-}" != "$1" ]; then
    set -- transmission-daemon "$@"
fi

if [ "$1" = "transmission-daemon" ]; then
    if [ "${TRANSMISSION_LOG_LEVEL:-}" ]; then
        log_level_str="$(log_level_to_str "${TRANSMISSION_LOG_LEVEL}")"
        set -- "$@" --log-level="${log_level_str}"
    fi

    if [ "${TRANSMISSION_LOG_FILE:-}" ]; then
        set -- "$@" --logfile="${TRANSMISSION_LOG_FILE}"
    fi
fi

exec "$@"
