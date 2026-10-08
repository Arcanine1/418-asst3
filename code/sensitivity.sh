#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C
export OMP_DYNAMIC=FALSE

PROGRAM="${PROGRAM:-./wireroute}"
INPUT_ROOT="${INPUT_ROOT:-/code/inputs}"
RUNS="${RUNS:-3}"
BATCH_SIZE="${BATCH_SIZE:-16}"
SA_ITERS="${SA_ITERS:-5}"

# Fixed probability for problem-size experiments.
SIZE_PROB="${SIZE_PROB:-0.1}"

PROBABILITIES=(0.01 0.1 0.5)
THREADS=(1 8)

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

[[ -x "$PROGRAM" ]] || fail "Cannot execute $PROGRAM."
[[ "$RUNS" =~ ^[1-9][0-9]*$ ]] || fail "RUNS must be positive."
[[ "$BATCH_SIZE" =~ ^[1-9][0-9]*$ ]] ||
    fail "BATCH_SIZE must be positive."
[[ "$SA_ITERS" =~ ^[1-9][0-9]*$ ]] ||
    fail "SA_ITERS must be positive."

HOST_NAME="$(hostname -s)"

if [[ "$HOST_NAME" =~ ^ghc([0-9]+)$ ]]; then
    HOST_NUMBER=$((10#${BASH_REMATCH[1]}))
    if (( HOST_NUMBER < 26 || HOST_NUMBER > 86 )); then
        fail "Run on ghc26 through ghc86."
    fi
else
    fail "Run on ghc26 through ghc86; current host: $HOST_NAME."
fi

MEDIUM_INPUT="$INPUT_ROOT/timeinput/medium_wires.txt"
GRID_DIR="$INPUT_ROOT/problemsize/gridsize"
WIRE_DIR="$INPUT_ROOT/problemsize/numwires"

[[ -r "$MEDIUM_INPUT" ]] || fail "Cannot read $MEDIUM_INPUT."
[[ -d "$GRID_DIR" ]] || fail "Missing directory: $GRID_DIR."
[[ -d "$WIRE_DIR" ]] || fail "Missing directory: $WIRE_DIR."

# Find all .txt inputs, including any in nested directories.
mapfile -d '' -t GRID_INPUTS < <(
    find "$GRID_DIR" -type f -name '*.txt' -print0 | sort -z
)
mapfile -d '' -t WIRE_INPUTS < <(
    find "$WIRE_DIR" -type f -name '*.txt' -print0 | sort -z
)

(( ${#GRID_INPUTS[@]} > 0 )) || fail "No .txt inputs in $GRID_DIR."
(( ${#WIRE_INPUTS[@]} > 0 )) || fail "No .txt inputs in $WIRE_DIR."

RESULT_DIR="$(mktemp -d "./sensitivity_${HOST_NAME}_XXXXXX")"
mkdir -p "$RESULT_DIR/logs"

# Required by the program's default output paths.
mkdir -p outputs

RAW_CSV="$RESULT_DIR/raw.csv"

printf '%s\n' \
    'host,study,input,dim_x,dim_y,num_wires,probability,batch_size,sa_iters,threads,run,compute_sec' \
    > "$RAW_CSV"

SUMMARY_HEADER='host,study,input,dim_x,dim_y,num_wires,probability,batch_size,sa_iters,runs,mean_compute_1_sec,mean_compute_8_sec,compute_speedup_8'

for study in probability gridsize numwires; do
    printf '%s\n' "$SUMMARY_HEADER" > "$RESULT_DIR/$study.csv"
done

# Read the first three whitespace-separated values from an input:
# dim_x, dim_y, number of wires.
read_dimensions() {
    awk '
        {
            for (i = 1; i <= NF; i++) {
                values[++count] = $i
                if (count == 3) {
                    printf "%s %s %s\n", values[1], values[2], values[3]
                    exit
                }
            }
        }
    ' "$1"
}

CASE_ID=0

benchmark_case() {
    local study="$1"
    local input="$2"
    local probability="$3"

    local dim_x dim_y num_wires
    local threads run stem log clean_log time_sec mean
    local baseline="" parallel=""
    local csv_input speedup
    local -a samples

    [[ -r "$input" ]] || fail "Cannot read $input."

    read -r dim_x dim_y num_wires < <(read_dimensions "$input")
    [[ "$dim_x" =~ ^[0-9]+$ &&
       "$dim_y" =~ ^[0-9]+$ &&
       "$num_wires" =~ ^[0-9]+$ ]] ||
        fail "Invalid input header: $input."

    CASE_ID=$((CASE_ID + 1))

    # Quote input paths correctly for CSV.
    csv_input="${input//\"/\"\"}"

    for threads in "${THREADS[@]}"; do
        samples=()

        for ((run = 1; run <= RUNS; run++)); do
            stem="$RESULT_DIR/logs/case${CASE_ID}_n${threads}_run${run}"
            log="${stem}.log"
            clean_log="${stem}.clean.log"

            printf '%s: input=%s P=%s threads=%s run=%s/%s\n' \
                "$study" "$(basename "$input")" "$probability" \
                "$threads" "$run" "$RUNS"

            if ! "$PROGRAM" \
                -f "$input" \
                -n "$threads" \
                -m A \
                -b "$BATCH_SIZE" \
                -p "$probability" \
                -i "$SA_ITERS" \
                > "$log" 2>&1; then
                fail "Program failed. See $log."
            fi

            # Remove ANSI color codes before parsing.
            sed -E $'s/\033\\[[0-9;]*m//g' "$log" > "$clean_log"

            if ! grep -Fq 'Validate Passed: no mismatches.' "$clean_log"; then
                fail "Validation did not pass. See $log."
            fi

            time_sec="$(
                awk '
                    /Computation time \(sec\):/ {
                        value = $0
                        sub(/^.*Computation time \(sec\):[[:space:]]*/, "", value)
                        split(value, parts, /[[:space:]]+/)
                        print parts[1]
                        exit
                    }
                ' "$clean_log"
            )"

            if [[ ! "$time_sec" =~ ^[0-9]+([.][0-9]+)?([eE][-+]?[0-9]+)?$ ]]; then
                fail "Cannot parse computation time. See $log."
            fi

            if ! awk -v t="$time_sec" 'BEGIN { exit !(t > 0) }'; then
                fail "Computation time must be positive. See $log."
            fi

            samples+=("$time_sec")

            printf '%s,%s,"%s",%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                "$HOST_NAME" "$study" "$csv_input" \
                "$dim_x" "$dim_y" "$num_wires" "$probability" \
                "$BATCH_SIZE" "$SA_ITERS" "$threads" "$run" "$time_sec" \
                >> "$RAW_CSV"
        done

        mean="$(
            printf '%s\n' "${samples[@]}" |
                awk '{ sum += $1 } END { printf "%.10f", sum / NR }'
        )"

        if [[ "$threads" == 1 ]]; then
            baseline="$mean"
        else
            parallel="$mean"
        fi
    done

    speedup="$(
        awk -v serial="$baseline" -v parallel="$parallel" \
            'BEGIN { printf "%.6f", serial / parallel }'
    )"

    # Write a summary immediately after each 1-thread/8-thread pair.
    printf '%s,%s,"%s",%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$HOST_NAME" "$study" "$csv_input" \
        "$dim_x" "$dim_y" "$num_wires" "$probability" \
        "$BATCH_SIZE" "$SA_ITERS" "$RUNS" \
        "$baseline" "$parallel" "$speedup" \
        >> "$RESULT_DIR/$study.csv"

    printf '  Mean 1-thread: %ss | Mean 8-thread: %ss | Speedup: %sx\n\n' \
        "$baseline" "$parallel" "$speedup"
}

# (a) Probability sensitivity: fixed medium_wires input.
for probability in "${PROBABILITIES[@]}"; do
    benchmark_case probability "$MEDIUM_INPUT" "$probability"
done

# (b) Grid-size sensitivity: fixed probability.
for input in "${GRID_INPUTS[@]}"; do
    benchmark_case gridsize "$input" "$SIZE_PROB"
done

# (b) Wire-count sensitivity: fixed probability.
for input in "${WIRE_INPUTS[@]}"; do
    benchmark_case numwires "$input" "$SIZE_PROB"
done

printf 'Finished. Results saved in %s\n' "$RESULT_DIR"
printf '  probability.csv\n  gridsize.csv\n  numwires.csv\n  raw.csv\n'