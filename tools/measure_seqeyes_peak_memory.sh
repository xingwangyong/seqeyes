#!/usr/bin/env bash
set -u

SEQEYES_BIN_DEFAULT="/autofs/cluster/berkin/xingwang/share/seqeyes/linux/rocky_8/seqeyes"
TIMEOUT_SECONDS_DEFAULT=60
REPEATS_DEFAULT=3
KILL_AFTER_SECONDS_DEFAULT=5

usage() {
    cat <<'EOF'
Usage:
  tools/measure_seqeyes_peak_memory.sh SEQ_DIR [OUT_CSV] [SEQEYES_BIN]

Measures peak memory for every *.seq file under SEQ_DIR. Each file is run
3 times by default, with each run limited to 60 seconds by timeout.

Output is CSV and can be loaded in MATLAB with:
  T = readtable('seqeyes_peak_memory.csv');

Environment overrides:
  SEQEYES_TIMEOUT_SECONDS   default: 60
  SEQEYES_REPEATS           default: 3
  SEQEYES_KILL_AFTER_SECONDS default: 5

Columns:
  seq_file, seq_name, run, timeout_seconds, elapsed_seconds,
  max_rss_kb, max_rss_mb, exit_code, timed_out, log_file
EOF
}

csv_escape() {
    local value=${1//\"/\"\"}
    printf '"%s"' "$value"
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
out_csv=${2:-seqeyes_peak_memory.csv}
seqeyes_bin=${3:-$SEQEYES_BIN_DEFAULT}
timeout_seconds=${SEQEYES_TIMEOUT_SECONDS:-$TIMEOUT_SECONDS_DEFAULT}
repeats=${SEQEYES_REPEATS:-$REPEATS_DEFAULT}
kill_after_seconds=${SEQEYES_KILL_AFTER_SECONDS:-$KILL_AFTER_SECONDS_DEFAULT}

if [[ ! -d "$seq_dir" ]]; then
    echo "SEQ_DIR is not a directory: $seq_dir" >&2
    exit 2
fi

if [[ ! -x "$seqeyes_bin" ]]; then
    echo "SeqEyes executable is not found or not executable: $seqeyes_bin" >&2
    exit 2
fi

if [[ ! -x /usr/bin/time ]]; then
    echo "Required command is missing or not executable: /usr/bin/time" >&2
    exit 2
fi

if ! command -v timeout >/dev/null 2>&1; then
    echo "Required command is missing: timeout" >&2
    exit 2
fi

case "$timeout_seconds" in
    ''|*[!0-9]*) echo "SEQEYES_TIMEOUT_SECONDS must be a positive integer" >&2; exit 2 ;;
esac
case "$repeats" in
    ''|*[!0-9]*) echo "SEQEYES_REPEATS must be a positive integer" >&2; exit 2 ;;
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

printf 'seq_file,seq_name,run,timeout_seconds,elapsed_seconds,max_rss_kb,max_rss_mb,exit_code,timed_out,log_file\n' > "$out_csv"

while IFS= read -r -d '' seq_file; do
    seq_name=$(basename "$seq_file")
    safe_name=${seq_name//[^A-Za-z0-9_.-]/_}

    for ((run = 1; run <= repeats; run++)); do
        time_file=$(mktemp)
        log_file="$log_dir/${safe_name}.run${run}.log"
        start_ns=$(date +%s%N)

        /usr/bin/time -f '%M' -o "$time_file" \
            timeout -k "${kill_after_seconds}s" "${timeout_seconds}s" \
            "$seqeyes_bin" "$seq_file" >"$log_file" 2>&1
        exit_code=$?

        end_ns=$(date +%s%N)
        elapsed_seconds=$(awk -v start="$start_ns" -v stop="$end_ns" 'BEGIN { printf "%.3f", (stop - start) / 1000000000 }')
        max_rss_kb=$(tail -n 1 "$time_file" 2>/dev/null)
        rm -f "$time_file"

        if [[ ! "$max_rss_kb" =~ ^[0-9]+$ ]]; then
            max_rss_kb=""
            max_rss_mb=""
        else
            max_rss_mb=$(awk -v kb="$max_rss_kb" 'BEGIN { printf "%.3f", kb / 1024 }')
        fi

        timed_out=0
        if [[ $exit_code -eq 124 || $exit_code -eq 137 ]]; then
            timed_out=1
        fi

        {
            csv_escape "$seq_file"; printf ','
            csv_escape "$seq_name"; printf ','
            printf '%d,%d,%s,%s,%s,%d,%d,' \
                "$run" "$timeout_seconds" "$elapsed_seconds" \
                "${max_rss_kb:-NaN}" "${max_rss_mb:-NaN}" "$exit_code" "$timed_out"
            csv_escape "$log_file"; printf '\n'
        } >> "$out_csv"

        printf 'Measured %s run %d/%d: max_rss=%s KB exit=%d timeout=%d\n' \
            "$seq_name" "$run" "$repeats" "${max_rss_kb:-NaN}" "$exit_code" "$timed_out" >&2
    done
done < "$tmp_list"

echo "Wrote CSV: $out_csv" >&2
