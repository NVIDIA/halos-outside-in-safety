#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Shared, startup-only SDM instance validation for launch_psf.sh.
# Never source an operator config: its contents are data, not shell commands.

declare -a SDM_APPS=() SDM_CONFIGS=() SDM_PORTS=() SDM_LOGS=()
declare -a PSF_SDM_VALUES=()
PSF_SDM_CONFIG_LIST=""
PSF_SDM_PORT_LIST=""

# Translate the existing ATL flags once; reject conflicting old/new inputs.
psf_sdm_legacy_args() {
    local app="$1" configs="$2" ports="$3" zone1="$4" zone2="$5" port2="$6"
    if [[ -n "$zone1" || -n "$zone2" ]]; then
        if [[ -n "$configs" ]]; then
            echo "Error: do not mix --sdm-config with --atl-zone*-conf" >&2
            return 1
        fi
        case "$app" in
            atl)
                if [[ -n "$zone2" ]]; then
                    echo "Error: zone-two config requires two ATL instances" >&2; return 1
                fi
                configs="$zone1" ;;
            atl_dual|atl,atl) configs="$zone1,$zone2" ;;
            *) echo "Error: use --sdm-config for PXC and mixed combinations" >&2; return 1 ;;
        esac
    fi
    if [[ -n "$port2" ]]; then
        if [[ "$app" != atl_dual && "$app" != atl,atl ]]; then
            echo "Error: zone-two port requires two ATL instances" >&2; return 1
        fi
        if [[ "$ports" == *,* ]]; then
            echo "Error: do not mix a port list with the legacy zone-two port" >&2; return 1
        fi
        ports="${ports:-12345},$port2"
    fi
    PSF_SDM_CONFIG_LIST="$configs"
    PSF_SDM_PORT_LIST="$ports"
}

psf_sdm_split_list() {
    local value="$1" label="$2" count="$3"
    if [[ -z "$value" || "$value" == ,* || "$value" == *, || "$value" == *,,* || "$value" == *$'\n'* ]]; then
        echo "Error: ${label} must contain ${count} non-empty comma-separated value(s)" >&2
        return 1
    fi
    IFS=, read -r -a PSF_SDM_VALUES <<< "$value"
    if [[ ${#PSF_SDM_VALUES[@]} -ne "$count" ]]; then
        echo "Error: ${label} needs ${count} value(s), in the same order as --app" >&2
        return 1
    fi
}

psf_sdm_binary() {
    case "$1" in
        atl) printf '%s/atl/atl_sdm\n' "$2" ;;
        pxc) printf '%s/proximity/proximity_sdm\n' "$2" ;;
        *) echo "Error: unsupported SDM '$1'" >&2; return 1 ;;
    esac
}

psf_sdm_default_mapping() {
    case "$1" in
        atl|atl_dual|atl,atl) printf '%s/atl/event_mapping_atl.pb.txt\n' "$2" ;;
        pxc|pxc,pxc|atl,pxc|pxc,atl) printf '%s/proximity/proximity_event_mapping.pb.txt\n' "$2" ;;
        *) return 1 ;;
    esac
}

psf_sdm_event_codes() {
    # The binary validates the complete key schema. This startup check only
    # detects cross-instance overlap; health/fault subscriptions remain shared.
    awk '
        { sub(/#.*/, ""); n = split($0, fields, "="); if (n != 2) next;
          value = fields[2]; gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
          if (value ~ /^EVENT_[0-9]+$/) print value; }
    ' "$1"
}

# Called after the launcher's common config paths have been resolved.
psf_sdm_check_log_inputs() {
    local log input resolved
    for input in "$@"; do
        [[ -z "$input" ]] && continue
        resolved="$(readlink -m -- "$input")" || return 1
        for log in ${SDM_LOGS[@]+"${SDM_LOGS[@]}"}; do
            if [[ "$log" == "$resolved" || "$log" -ef "$input" ]]; then
                echo "Error: an SDM log must not overwrite a configuration or mapping file" >&2
                return 1
            fi
        done
    done
}

psf_sdm_prepare() {
    local app="$1" configs="$2" ports="$3" logs="$4" log_dir="$5" need_configs="$6"
    local i j port path code other same_type=0
    case "$app" in
        atl|pxc) SDM_APPS=("$app") ;;
        atl_dual|atl,atl) SDM_APPS=(atl atl) ;;
        atl,pxc) SDM_APPS=(atl pxc) ;;
        pxc,atl) SDM_APPS=(pxc atl) ;;
        pxc,pxc) SDM_APPS=(pxc pxc) ;;
        *) echo "Error: unsupported SDM combination '$app'" >&2; return 1 ;;
    esac
    local count=${#SDM_APPS[@]}
    SDM_CONFIGS=()
    SDM_PORTS=()
    SDM_LOGS=()
    if [[ "$need_configs" == 1 ]]; then
        psf_sdm_split_list "$configs" --sdm-config "$count" || return 1
        SDM_CONFIGS=("${PSF_SDM_VALUES[@]}")
        for ((i=0; i<count; ++i)); do
            path="${SDM_CONFIGS[$i]}"
            if [[ ! -f "$path" || ! -r "$path" ]]; then
                echo "Error: SDM $((i+1)) config '$path' is not a readable file" >&2
                return 1
            fi
            SDM_CONFIGS[$i]="$(readlink -f -- "$path")" || return 1
        done
    elif [[ -n "$configs" ]]; then
        echo "Error: SDM configs must be supplied to the launcher that starts the SDMs" >&2
        return 1
    fi

    if [[ -z "$ports" && "$count" == 1 ]]; then
        ports=12345
    fi
    psf_sdm_split_list "$ports" 'command receiver ports' "$count" || return 1
    for port in "${PSF_SDM_VALUES[@]}"; do
        if [[ ! "$port" =~ ^[0-9]{1,5}$ ]] || ((10#$port < 1 || 10#$port > 65535)); then
            echo "Error: command receiver ports must be decimal integers in 1..65535" >&2
            return 1
        fi
        # Canonical decimal avoids collisions hidden by leading zeros.
        SDM_PORTS+=("$((10#$port))")
    done
    if [[ "$count" == 2 && "${SDM_PORTS[0]}" == "${SDM_PORTS[1]}" ]]; then
        echo "Error: each SDM needs a different command receiver port" >&2
        return 1
    fi

    if [[ -n "$logs" ]]; then
        psf_sdm_split_list "$logs" --sdm-log-file "$count" || return 1
        SDM_LOGS=("${PSF_SDM_VALUES[@]}")
    else
        if [[ "$count" == 2 && "${SDM_APPS[0]}" == "${SDM_APPS[1]}" ]]; then
            same_type=1
        fi
        for ((i=0; i<count; ++i)); do
            path="${SDM_APPS[$i]}_sdm"
            if [[ "$same_type" == 1 ]]; then path+="_zone$((i+1))"; fi
            SDM_LOGS+=("${path}.log")
        done
    fi
    for ((i=0; i<count; ++i)); do
        path="${SDM_LOGS[$i]}"
        if [[ "$path" != /* ]]; then path="${log_dir}/${path}"; fi
        SDM_LOGS[$i]="$(readlink -m -- "$path")" || return 1
        if [[ -e "${SDM_LOGS[$i]}" && ! -f "${SDM_LOGS[$i]}" ]]; then
            echo "Error: SDM log path is not a regular file: ${SDM_LOGS[$i]}" >&2
            return 1
        fi
        for other in "$log_dir/psf.log" "$log_dir/saim.log"; do
            if [[ "${SDM_LOGS[$i]}" == "$(readlink -m -- "$other")" || "${SDM_LOGS[$i]}" -ef "$other" ]]; then
                echo "Error: an SDM needs its own log, not a shared control-plane/monitor log" >&2
                return 1
            fi
        done
        for other in ${SDM_CONFIGS[@]+"${SDM_CONFIGS[@]}"}; do
            if [[ "${SDM_LOGS[$i]}" == "$other" || "${SDM_LOGS[$i]}" -ef "$other" ]]; then
                echo "Error: an SDM log must not overwrite a configuration file" >&2
                return 1
            fi
        done
        for ((j=0; j<i; ++j)); do
            if [[ "${SDM_LOGS[$i]}" == "${SDM_LOGS[$j]}" || "${SDM_LOGS[$i]}" -ef "${SDM_LOGS[$j]}" ]]; then
                echo "Error: each SDM needs a different log file" >&2
                return 1
            fi
        done
    done

    if [[ "$need_configs" == 1 && "$count" == 2 ]]; then
        local first_codes second_codes
        first_codes="$(psf_sdm_event_codes "${SDM_CONFIGS[0]}")" || return 1
        second_codes="$(psf_sdm_event_codes "${SDM_CONFIGS[1]}")" || return 1
        while IFS= read -r code; do
            [[ -z "$code" ]] && continue
            while IFS= read -r other; do
                if [[ "$code" == "$other" ]]; then
                    echo "Error: SDM configs overlap on ${code}; use distinct application event codes" >&2
                    return 1
                fi
            done <<< "$second_codes"
        done <<< "$first_codes"
    fi
}
