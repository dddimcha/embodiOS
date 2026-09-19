#!/usr/bin/env bash
#
# download-models.sh - Download GGUF models for embodiOS
#
# Usage:
#   scripts/download-models.sh [model ...]
#   scripts/download-models.sh            # default: smollm
#   scripts/download-models.sh smollm     # SmolLM-135M-Instruct Q4_K_M (default model)
#   scripts/download-models.sh tinyllama  # TinyLlama-1.1B-Chat Q4_K_M (optional)
#   scripts/download-models.sh all        # all known models
#
# Downloads go to <repo>/models/ and are verified against the size and
# sha256 recorded in models/manifest.json. hf-mirror.com is tried first
# (huggingface.co may be unreachable in some networks), with
# huggingface.co as fallback.
#
# Note: tensorblock/TinyLlama-1.1B-Chat-v1.0-GGUF does not provide a
# Q4_K_M file, so TinyLlama is fetched from TheBloke instead.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
MODELS_DIR="$REPO_DIR/models"

log()  { echo "[download-models] $*"; }
warn() { echo "[download-models] WARNING: $*" >&2; }
die()  { echo "[download-models] ERROR: $*" >&2; exit 1; }

# model key -> filename|size|sha256|mirror_url|hf_url
model_info() {
    case "$1" in
        smollm)
            echo "smollm-135m-instruct-q4_k_m.gguf|105453984|4e5042ffdf8aa6edb75f2b13177c124a01f411bc7289f9aa14be16b47acac5bb|https://hf-mirror.com/mjdousti/SmolLM-135M-Instruct-Q4_K_M-GGUF/resolve/main/smollm-135m-instruct-q4_k_m.gguf|https://huggingface.co/mjdousti/SmolLM-135M-Instruct-Q4_K_M-GGUF/resolve/main/smollm-135m-instruct-q4_k_m.gguf"
            ;;
        tinyllama)
            echo "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf|668788096|9fecc3b3cd76bba89d504f29b616eedf7da85b96540e490ca5824d3f7d2776a0|https://hf-mirror.com/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf|https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
            ;;
        *)
            return 1
            ;;
    esac
}

verify_file() {
    local path="$1" want_size="$2" want_sha="$3"

    local size
    size=$(stat -c %s "$path" 2>/dev/null || stat -f %z "$path")
    if [ "$size" != "$want_size" ]; then
        warn "size mismatch: got $size, want $want_size"
        return 1
    fi

    if command -v sha256sum >/dev/null 2>&1; then
        local sha
        sha=$(sha256sum "$path" | awk '{print $1}')
        if [ "$sha" != "$want_sha" ]; then
            warn "sha256 mismatch: got $sha, want $want_sha"
            return 1
        fi
    else
        warn "sha256sum not available, skipping hash check"
    fi
    return 0
}

download_one() {
    local key="$1"
    local info filename want_size want_sha mirror_url hf_url
    info=$(model_info "$key") || die "unknown model '$key' (known: smollm, tinyllama, all)"
    IFS='|' read -r filename want_size want_sha mirror_url hf_url <<< "$info"

    mkdir -p "$MODELS_DIR"
    local dest="$MODELS_DIR/$filename"

    if [ -f "$dest" ]; then
        if verify_file "$dest" "$want_size" "$want_sha"; then
            log "$key: $filename already present and verified, skipping"
            return 0
        fi
        warn "$key: existing $filename failed verification, re-downloading"
        rm -f "$dest"
    fi

    local url
    for url in "$mirror_url" "$hf_url"; do
        log "$key: downloading $filename"
        log "  from $url"
        if curl -fL --connect-timeout 15 -o "$dest.part" "$url"; then
            if verify_file "$dest.part" "$want_size" "$want_sha"; then
                mv "$dest.part" "$dest"
                log "$key: OK -> $dest (size + sha256 verified)"
                return 0
            fi
            warn "$key: downloaded file failed verification from this source"
        else
            warn "$key: download failed from this source"
        fi
        rm -f "$dest.part"
    done

    die "$key: all sources failed"
}

main() {
    local targets=("$@")
    if [ ${#targets[@]} -eq 0 ]; then
        targets=(smollm)
    fi

    local t
    for t in "${targets[@]}"; do
        if [ "$t" = "all" ]; then
            download_one smollm
            download_one tinyllama
        else
            download_one "$t"
        fi
    done

    log "done. Models are in: $MODELS_DIR"
}

main "$@"
