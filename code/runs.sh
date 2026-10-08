#!/usr/bin/env bash
set -euo pipefail

# Run from the directory containing ./wireroute.
input_dir="${INPUT_DIR:-inputs/timeinput}"

inputs=(
    "$input_dir/few_wires.txt"
    "$input_dir/medium_wires.txt"
    "$input_dir/abundant_wires.txt"
)

modes=(W A)
threads=(1 2 4 8)
runs=3

# Edit these independently.
# In W mode, -b affects scheduling only if your code uses it.
within_batches=(16)
across_batches=(16)

sa_prob=0.1
sa_iters=5

# Require a GHC machine numbered 26 through 86.
host=$(hostname -s)
if [[ "$host" =~ ^ghc([0-9]+)$ ]]; then
    ghc_number=$((10#${BASH_REMATCH[1]}))
    if ((ghc_number < 26 || ghc_number > 86)); then
        echo "Run on ghc26 through ghc86; current host: $host" >&2
        exit 1
    fi
else
    echo "Run on ghc26 through ghc86; current host: $host" >&2
    exit 1
fi

if [[ ! -x ./wireroute ]]; then
    echo "./wireroute is missing or not executable." >&2
    exit 1
fi

for input in "${inputs[@]}"; do
    if [[ ! -f "$input" ]]; then
        echo "Missing input: $input" >&2
        exit 1
    fi
done

# A unique directory preserves previous benchmark results.
out_dir=$(mktemp -d "benchmark_${host}_XXXXXX")
mkdir -p "$out_dir/logs" outputs

raw_csv="$out_dir/raw.csv"
summary_csv="$out_dir/summary.csv"

echo "host,mode,input,batch_size,threads,run,init_sec,compute_sec,total_sec" \
    > "$raw_csv"

echo "host,mode,input,batch_size,threads,runs,mean_init_sec,mean_compute_sec,mean_total_sec,compute_speedup,total_speedup" \
    > "$summary_csv"

printf "%-2s %-19s %5s %7s %12s %12s %11s %11s\n" \
    "M" "Input" "Batch" "Threads" "Compute(s)" "Total(s)" "Compute(x)" "Total(x)"

for mode in "${modes[@]}"; do
    if [[ "$mode" == W ]]; then
        batches=("${within_batches[@]}")
    else
        batches=("${across_batches[@]}")
    fi

    for input in "${inputs[@]}"; do
        name=$(basename "$input" .txt)

        for b in "${batches[@]}"; do
            baseline_compute=""
            baseline_total=""

            # Keep 1 first so its baseline is available immediately.
            for n in "${threads[@]}"; do
                samples="$out_dir/logs/${mode}_${name}_b${b}_n${n}_times.csv"
                : > "$samples"

                for ((run = 1; run <= runs; run++)); do
                    log="$out_dir/logs/${mode}_${name}_b${b}_n${n}_run${run}.log"

                    echo "Running $mode $name: n=$n b=$b run=$run/$runs" >&2

                    if ! ./wireroute \
                        -f "$input" \
                        -n "$n" \
                        -m "$mode" \
                        -b "$b" \
                        -p "$sa_prob" \
                        -i "$sa_iters" \
                        > "$log" 2>&1; then
                        echo "Execution failed. See $log" >&2
                        exit 1
                    fi

                    # Match even when ANSI color codes precede the text.
                    if ! grep -Fq 'Validate Passed: no mismatches.' "$log"; then
                        echo "Validation failed or missing. See $log" >&2
                        exit 1
                    fi

                    init_sec=$(awk \
                        '/^Initialization time \(sec\):/ {print $4; exit}' \
                        "$log")

                    compute_sec=$(awk \
                        '/^Computation time \(sec\):/ {print $4; exit}' \
                        "$log")

                    if [[ ! "$init_sec" =~ ^[0-9]+([.][0-9]+)?$ ||
                          ! "$compute_sec" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
                        echo "Missing or invalid timing values. See $log" >&2
                        exit 1
                    fi

                    total_sec=$(awk -v a="$init_sec" -v b="$compute_sec" \
                        'BEGIN {printf "%.10f", a+b}')

                    printf '%s,%s,%s\n' \
                        "$init_sec" "$compute_sec" "$total_sec" >> "$samples"

                    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                        "$host" "$mode" "$name" "$b" "$n" "$run" \
                        "$init_sec" "$compute_sec" "$total_sec" >> "$raw_csv"
                done

                # Average the three runs.
                read -r mean_init mean_compute mean_total < <(
                    awk -F, '
                        {init += $1; comp += $2; total += $3}
                        END {
                            printf "%.10f %.10f %.10f\n",
                                   init/NR, comp/NR, total/NR
                        }
                    ' "$samples"
                )

                if [[ "$n" == 1 ]]; then
                    baseline_compute="$mean_compute"
                    baseline_total="$mean_total"
                fi

                read -r compute_speedup total_speedup < <(
                    awk \
                        -v bc="$baseline_compute" \
                        -v bt="$baseline_total" \
                        -v c="$mean_compute" \
                        -v t="$mean_total" \
                        'BEGIN {
                            if (c <= 0 || t <= 0) {
                                print "NA NA"
                            } else {
                                printf "%.6f %.6f\n", bc/c, bt/t
                            }
                        }'
                )

                # Save and display each configuration as soon as it finishes.
                printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                    "$host" "$mode" "$name" "$b" "$n" "$runs" \
                    "$mean_init" "$mean_compute" "$mean_total" \
                    "$compute_speedup" "$total_speedup" >> "$summary_csv"

                printf "%-2s %-19s %5s %7s %12s %12s %11s %11s\n" \
                    "$mode" "$name" "$b" "$n" \
                    "$mean_compute" "$mean_total" \
                    "$compute_speedup" "$total_speedup"
            done
        done
    done
done

echo
echo "Summary with speedups: $summary_csv"
echo "Individual runs:      $raw_csv"
echo "Full logs:            $out_dir/logs"