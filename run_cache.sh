#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
export OMP_DYNAMIC=FALSE

# Override these using environment variables if needed.
PROGRAM="${PROGRAM:-./wireroute}"
INPUT_DIR="${INPUT_DIR:-inputs/timeinput}"
RUNS="${RUNS:-3}"
BATCH_SIZE="${BATCH_SIZE:-16}"

MODES=(W A)
INPUTS=(few_wires medium_wires abundant_wires)
THREADS=(1 2 4 8)

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

command -v perf >/dev/null || fail "perf is not installed."
[[ -x "$PROGRAM" ]] || fail "Cannot execute $PROGRAM."
[[ "$RUNS" =~ ^[1-9][0-9]*$ ]] || fail "RUNS must be positive."
[[ "$BATCH_SIZE" =~ ^[1-9][0-9]*$ ]] || fail "BATCH_SIZE must be positive."

HOST_NAME="$(hostname -s)"

# Ensure assignment measurements are taken on an eligible GHC machine.
if [[ "$HOST_NAME" =~ ^ghc([0-9]+)$ ]]; then
    HOST_NUMBER=$((10#${BASH_REMATCH[1]}))
    if (( HOST_NUMBER < 26 || HOST_NUMBER > 86 )); then
        fail "Run this on ghc26 through ghc86."
    fi
else
    fail "Run this on ghc26 through ghc86; current host: $HOST_NAME."
fi

for input in "${INPUTS[@]}"; do
    [[ -r "$INPUT_DIR/$input.txt" ]] ||
        fail "Cannot read $INPUT_DIR/$input.txt."
done

# A new directory avoids overwriting previous measurements.
RESULT_DIR="$(mktemp -d "./cache_results_${HOST_NAME}_XXXXXX")"
mkdir -p "$RESULT_DIR/logs"

RAW_CSV="$RESULT_DIR/raw.csv"
SUMMARY_CSV="$RESULT_DIR/summary.csv"

printf '%s\n' \
    'host,mode,input,batch_size,threads,run,total_cache_misses,cache_misses_per_thread' \
    > "$RAW_CSV"

printf '%s\n' \
    'host,mode,input,batch_size,threads,runs,mean_total_cache_misses,mean_cache_misses_per_thread' \
    > "$SUMMARY_CSV"

for mode in "${MODES[@]}"; do
    for input in "${INPUTS[@]}"; do
        for threads in "${THREADS[@]}"; do
            samples="$RESULT_DIR/logs/${mode}_${input}_${threads}_samples.txt"
            : > "$samples"

            for ((run = 1; run <= RUNS; run++)); do
                stem="$RESULT_DIR/logs/${mode}_${input}_${threads}_run${run}"
                program_log="${stem}.program.log"
                perf_log="${stem}.perf.csv"

                printf 'mode=%s input=%s threads=%s run=%s/%s\n' \
                    "$mode" "$input" "$threads" "$run" "$RUNS"

                if ! perf stat \
                    --no-big-num \
                    -x ';' \
                    -e cache-misses \
                    -o "$perf_log" \
                    -- "$PROGRAM" \
                    -f "$INPUT_DIR/$input.txt" \
                    -n "$threads" \
                    -m "$mode" \
                    -b "$BATCH_SIZE" \
                    > "$program_log" 2>&1; then
                    fail "Program or perf failed. See $program_log and $perf_log."
                fi

                # Substring matching also handles ANSI color codes.
                if ! grep -Fq 'Validate Passed: no mismatches.' "$program_log"; then
                    fail "Validation did not pass. See $program_log."
                fi

                # perf -x output:
                # counter value ; unit ; event name ; ...
                # Reject unavailable counters instead of treating them as zero.
                if ! misses="$(
                    awk -F ';' '
                        $3 ~ /cache-misses/ {
                            value = $1
                            gsub(/[[:space:]]/, "", value)

                            if (value !~ /^[0-9]+([.][0-9]+)?$/) {
                                invalid = 1
                            } else {
                                total += value
                                found = 1
                            }
                        }
                        END {
                            if (!found || invalid)
                                exit 1
                            printf "%.0f\n", total
                        }
                    ' "$perf_log"
                )"; then
                    fail "Cache-miss counter unavailable or unreadable. See $perf_log."
                fi

                per_thread="$(
                    awk -v count="$misses" -v n="$threads" \
                        'BEGIN { printf "%.6f", count / n }'
                )"

                printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
                    "$HOST_NAME" "$mode" "$input" "$BATCH_SIZE" \
                    "$threads" "$run" "$misses" "$per_thread" \
                    >> "$RAW_CSV"

                printf '%s\n' "$misses" >> "$samples"
            done

            averages="$(
                awk -v n="$threads" '
                    { sum += $1 }
                    END {
                        mean = sum / NR
                        printf "%.6f,%.6f", mean, mean / n
                    }
                ' "$samples"
            )"

            printf '%s,%s,%s,%s,%s,%s,%s\n' \
                "$HOST_NAME" "$mode" "$input" "$BATCH_SIZE" \
                "$threads" "$RUNS" "$averages" \
                >> "$SUMMARY_CSV"

            printf '  Mean total misses, mean per-thread misses: %s\n' \
                "$averages"
        done
    done
done

printf '\nRaw results: %s\nSummary: %s\n' "$RAW_CSV" "$SUMMARY_CSV"