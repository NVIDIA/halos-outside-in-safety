#!/usr/bin/env bash
# Switch the live RTX post-processing preset while run_actor_sdg.py is running.
#
# The runner polls postprocessing.yaml, so rewriting `active:` is enough — the
# next rendered frames carry the new look. Writes through a temp file in the
# same directory so the runner never reads a half-written config.
#
#   ./set_preset.sh                 # list presets and show the active one
#   ./set_preset.sh tv_noise        # switch
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="${POSTPROCESSING_CONFIG:-$SCRIPT_DIR/../../configs/postprocessing.yaml}"

if [[ ! -f "$CONFIG" ]]; then
    echo "ERROR: config not found: $CONFIG" >&2
    exit 1
fi

# Preset names are the keys indented exactly two spaces under `presets:`.
#
# A preset this misses is a preset the operator cannot select, so the match is
# deliberately loose: names may be quoted (`"off":`, which the config tells you
# to write for YAML's boolean words) or hyphenated, and the value may be inline
# (`baseline: {}`) rather than a nested block. A comment at column 0 does NOT
# end the block — only a real top-level key does.
list_presets() {
    awk '
        /^presets:/ { inside = 1; next }
        /^[^[:space:]#]/ { inside = 0 }
        inside && /^  ["'"'"']?[A-Za-z_][A-Za-z0-9_.-]*["'"'"']?:/ {
            line = $0
            sub(/^  ["'"'"']?/, "", line)
            sub(/["'"'"']?:.*$/, "", line)
            print line
        }
    ' "$CONFIG"
}

# Quotes are stripped so the value compares equal to the name the caller passed.
active_preset() {
    awk '
        /^active[[:space:]]*:/ {
            sub(/^active[[:space:]]*:[[:space:]]*/, "")
            sub(/[[:space:]]*(#.*)?$/, "")
            gsub(/^["'"'"']|["'"'"']$/, "")
            print
            exit
        }
    ' "$CONFIG"
}

PRESETS="$(list_presets)"
PRESET="${1:-}"

if [[ -z "$PRESET" ]]; then
    echo "Active preset: $(active_preset)"
    echo "Available:"
    sed 's/^/  /' <<<"$PRESETS"
    exit 0
fi

if ! grep -qxF "$PRESET" <<<"$PRESETS"; then
    echo "ERROR: unknown preset '$PRESET'" >&2
    echo "Available: $(tr '\n' ' ' <<<"$PRESETS")" >&2
    exit 2
fi

TMP="$(mktemp "$(dirname "$CONFIG")/.postprocessing.yaml.XXXXXX")"
trap 'rm -f "$TMP"' EXIT
# Tolerates `active : baseline` (valid YAML), which an anchored `^active:.*`
# silently left alone. Always quoted, because an unquoted `active: off` parses
# as the boolean False and the runner then rejects the whole file.
sed "s|^active[[:space:]]*:.*|active: \"$PRESET\"|" "$CONFIG" >"$TMP"
# mktemp creates the file 0600; without this the config would land unreadable
# to the container user and show up in git as a mode change.
chmod --reference="$CONFIG" "$TMP"
# The container mounts the whole sil/ directory, not this file, so replacing the
# inode is both safe and atomic — the runner never reads a half-written config.
mv "$TMP" "$CONFIG"
trap - EXIT

# Read the result back rather than trust the substitution: `mv` succeeds just as
# happily when the sed matched nothing, and a script that reports a switch it
# did not make is worse than one that fails.
GOT="$(active_preset)"
if [[ "$GOT" != "$PRESET" ]]; then
    echo "ERROR: failed to set active preset in $CONFIG (still '$GOT')" >&2
    exit 3
fi

echo "Active preset: $PRESET"
