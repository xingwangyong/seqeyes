#!/usr/bin/env bash
set -u

SEQEYES_BIN_DEFAULT="/autofs/cluster/berkin/xingwang/share/seqeyes/linux/rocky_8/seqeyes"
TIMEOUT_SECONDS_DEFAULT=30
KILL_AFTER_SECONDS_DEFAULT=5

usage() {
    cat <<'EOF'
Usage:
  tools/measure_seqeyes_load_time.sh SEQ_DIR [OUT_CSV] [SEQEYES_BIN]

Runs SeqEyes once for every *.seq file under SEQ_DIR, limits each run to
30 seconds with timeout, and extracts totalMs from the last "type=load" log
line printed by SeqEyes.

Your SeqEyes log level must allow info/debug output, otherwise total_ms will
be NaN because the "type=load" line is not printed to stderr.

Output is CSV and can be loaded in MATLAB with:
  T = readtable('seqeyes_load_time.csv');

Environment overrides:
  SEQEYES_TIMEOUT_SECONDS    default: 30
  SEQEYES_KILL_AFTER_SECONDS default: 5

Columns:
  seq_file, seq_name, timeout_seconds, elapsed_seconds, total_ms,
  parse_ms, decode_ms, render_data_ms, replot_ms, exit_code, timed_out,
  found_load_log, log_file
EOF
}

csv_escape() {
    local value=${1//\"/\"\"}
    printf '"%s"' "$value"
}

extract_log_field() {
    local line=$1
    local key=$2
    awk -v key="$key" '
        {
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == key) {
                    print kv[2]
                    exit
                }
            }
        }
    ' <<< "$line"
}

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then
    usage
    exit 0
fi

if [[ $# -lt 1 || $# -gt 3 ]]; then
    usage >&2
    exit 2
fi

seq_dir=$1
out_csv=${2:-seqeyes_load_time.csv}
seqeyes_bin=${3:-$SEQEYES_BIN_DEFAULT}
timeout_seconds=${SEQEYES_TIMEOUT_SECONDS:-$TIMEOUT_SECONDS_DEFAULT}
kill_after_seconds=${SEQEYES_KILL_AFTER_SECONDS:-$KILL_AFTER_SECONDS_DEFAULT}

if [[ ! -d "$seq_dir" ]]; then
    echo "SEQ_DIR is not a directory: $seq_dir" >&2
    exit 2
fi

if [[ ! -x "$seqeyes_bin" ]]; then
    echo "SeqEyes executable is not found or not executable: $seqeyes_bin" >&2
    exit 2
fi

if ! command -v timeout >/dev/null 2>&1; then
    echo "Required command is missing: timeout" >&2
    exit 2
fi

case "$timeout_seconds" in
    ''|*[!0-9]*) echo "SEQEYES_TIMEOUT_SECONDS must be a positive integer" >&2; exit 2 ;;
esac
case "$kill_after_seconds" in
    ''|*[!0-9]*) echo "SEQEYES_KILL_AFTER_SECONDS must be a positive integer" >&2; exit 2 ;;
esac

mkdir -p "$(dirname "$out_csv")"
log_dir="$(dirname "$out_csv")/$(basename "$out_csv" .csv)_logs"
mkdir -p "$log_dir"

tmp_list=$(mktemp)
trap 'rm -f "$tmp_list"' EXIT

find "$seq_dir" -type f -name '*.seq' -print0 | sort -z > "$tmp_list"

if [[ ! -s "$tmp_list" ]]; then
    echo "No .seq files found under: $seq_dir" >&2
    exit 1
fi

printf 'seq_file,seq_name,timeout_seconds,elapsed_seconds,total_ms,parse_ms,decode_ms,render_data_ms,replot_ms,exit_code,timed_out,found_load_log,log_file\n' > "$out_csv"

while IFS= read -r -d '' seq_file; do
    seq_name=$(basename "$seq_file")
    safe_name=${seq_name//[^A-Za-z0-9_.-]/_}
    log_file="$log_dir/${safe_name}.log"
    start_ns=$(date +%s%N)

    timeout -k "${kill_after_seconds}s" "${timeout_seconds}s" \
        "$seqeyes_bin" "$seq_file" >"$log_file" 2>&1
    exit_code=$?

    end_ns=$(date +%s%N)
    elapsed_seconds=$(awk -v start="$start_ns" -v stop="$end_ns" 'BEGIN { printf "%.3f", (stop - start) / 1000000000 }')

    timed_out=0
    if [[ $exit_code -eq 124 || $exit_code -eq 137 ]]; then
        timed_out=1
    fi

    load_line=$(grep 'type=load ' "$log_file" | tail -n 1 || true)
    found_load_log=0
    total_ms=""
    parse_ms=""
    decode_ms=""
    render_data_ms=""
    replot_ms=""
    if [[ -n "$load_line" ]]; then
        found_load_log=1
        total_ms=$(extract_log_field "$load_line" "totalMs")
        parse_ms=$(extract_log_field "$load_line" "parseMs")
        decode_ms=$(extract_log_field "$load_line" "decodeMs")
        render_data_ms=$(extract_log_field "$load_line" "renderDataMs")
        replot_ms=$(extract_log_field "$load_line" "replotMs")
    fi

    {
        csv_escape "$seq_file"; printf ','
        csv_escape "$seq_name"; printf ','
        printf '%d,%s,%s,%s,%s,%s,%s,%d,%d,%d,' \
            "$timeout_seconds" "$elapsed_seconds" \
            "${total_ms:-NaN}" "${parse_ms:-NaN}" "${decode_ms:-NaN}" \
            "${render_data_ms:-NaN}" "${replot_ms:-NaN}" \
            "$exit_code" "$timed_out" "$found_load_log"
        csv_escape "$log_file"; printf '\n'
    } >> "$out_csv"

    printf 'Measured %s: totalMs=%s exit=%d timeout=%d log=%d\n' \
        "$seq_name" "${total_ms:-NaN}" "$exit_code" "$timed_out" "$found_load_log" >&2
done < "$tmp_list"

echo "Wrote CSV: $out_csv" >&2
