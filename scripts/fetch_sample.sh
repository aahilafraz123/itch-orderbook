#!/usr/bin/env bash
# Download a full-day NASDAQ TotalView-ITCH 5.0 sample file from NASDAQ's public
# EMI site, and turn it into something itch_stats / the benchmarks can read.
#
# Usage:
#   scripts/fetch_sample.sh download   [DATE]   download DATE.NASDAQ_ITCH50.gz (resumable) and verify it
#   scripts/fetch_sample.sh prefix MB  [DATE]   decompress just the first MB megabytes
#   scripts/fetch_sample.sh decompress [DATE]   decompress the whole day (several times the .gz size)
#
# DATE is MMDDYYYY and defaults to 12302019, the smallest full-day file (3.5 GB).
# Other dates on the site: 01302019 01302020 03272019 07302019 08302019 10302019.
#
# Files go in data/ at the repo root, which is git-ignored.
#
# `prefix` works while `download` is still running: gzip decompresses from the
# start of the file, so the first N MB are usable as soon as they've arrived.
# Message framing is preserved; the last message may be cut in half, which
# itch_stats reports as "truncated" rather than an error.

set -euo pipefail

BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
DEFAULT_DATE="12302019"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_DIR="$REPO_ROOT/data"

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

check_date() {
    [[ "$1" =~ ^[0-9]{8}$ ]] || { echo "error: DATE must be MMDDYYYY, got '$1'" >&2; exit 2; }
}

cmd_download() {
    local date="${1:-$DEFAULT_DATE}"
    check_date "$date"
    local name="$date.NASDAQ_ITCH50.gz"
    local gz="$DATA_DIR/$name"
    mkdir -p "$DATA_DIR"

    echo "Downloading $BASE_URL/$name"
    echo "        to  $gz"
    # -C - resumes a partial download instead of starting over.
    # --retry handles brief network drops on a multi-GB transfer.
    curl --fail --location --retry 5 --retry-delay 2 -C - -o "$gz" "$BASE_URL/$name"

    # Every .gz ends with a CRC32 of its contents; gzip -t recomputes it.
    # (NASDAQ lists .md5sum files, but the server doesn't actually serve them.)
    echo "Verifying gzip integrity (CRC32) ..."
    gzip -t "$gz"
    echo "OK: $gz ($(du -h "$gz" | cut -f1))"
}

cmd_prefix() {
    local mb="${1:-}"
    [[ "$mb" =~ ^[0-9]+$ && "$mb" -gt 0 ]] || { echo "error: prefix needs a size in MB" >&2; usage; }
    local date="${2:-$DEFAULT_DATE}"
    check_date "$date"
    local gz="$DATA_DIR/$date.NASDAQ_ITCH50.gz"
    local out="$DATA_DIR/$date.NASDAQ_ITCH50.prefix${mb}MB"
    [[ -f "$gz" ]] || { echo "error: $gz not found; run '$0 download $date' first" >&2; exit 1; }

    local bytes=$((mb * 1024 * 1024))
    # head exits after N bytes, which makes gzip die of SIGPIPE; that's expected.
    # gzip also complains if the .gz is still downloading. Neither is a problem,
    # so ignore the pipeline status and check the output size instead.
    set +o pipefail
    gzip -dc "$gz" 2>/dev/null | head -c "$bytes" > "$out"
    set -o pipefail

    local got
    got=$(wc -c < "$out" | tr -d ' ')
    if [[ "$got" -lt "$bytes" ]]; then
        echo "warning: only $got of $bytes bytes available (download still in progress?)" >&2
    fi
    echo "OK: $out ($got bytes)"
}

cmd_decompress() {
    local date="${1:-$DEFAULT_DATE}"
    check_date "$date"
    local gz="$DATA_DIR/$date.NASDAQ_ITCH50.gz"
    local out="$DATA_DIR/$date.NASDAQ_ITCH50"
    [[ -f "$gz" ]] || { echo "error: $gz not found; run '$0 download $date' first" >&2; exit 1; }

    echo "Decompressing $gz -> $out (keeps the .gz) ..."
    gzip -dc "$gz" > "$out.partial"
    mv "$out.partial" "$out"   # only appears under its real name once complete
    echo "OK: $out ($(du -h "$out" | cut -f1))"
}

case "${1:-}" in
    download)   shift; cmd_download "$@" ;;
    prefix)     shift; cmd_prefix "$@" ;;
    decompress) shift; cmd_decompress "$@" ;;
    *)          usage ;;
esac
