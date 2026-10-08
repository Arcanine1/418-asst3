/**
 * Parallel VLSI Wire Routing via OpenMP
 * ayushgar
 */

#include "wireroute.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib> // Integer overloads
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>


#include <omp.h>
#include <unistd.h>

namespace {
struct Params {
    int num_threads;
    double SA_prob;
    int SA_iters;
    int batch_size = 1;
};
} // namespace

void print_stats(const std::vector<std::vector<int>>& occupancy) {
    int max_occupancy = 0;
    long long total_cost = 0;

    for (const auto& row : occupancy) {
        for (const int count : row) {
            max_occupancy = std::max(max_occupancy, count);
            total_cost += count * count;
        }
    }

    std::cout << "Max occupancy: " << max_occupancy << '\n';
    std::cout << "Total cost: " << total_cost << '\n';
}

/* This function write the output into 2 files
(1) It write occupancy grids into a file
(2) It convert wires from Wire to validate_wire_t by to_validate_format
(2) It write wires into another file
*/
void write_output(const std::vector<Wire>& wires, const int num_wires, const std::vector<std::vector<int>>& occupancy,
                  const int dim_x, const int dim_y, std::string wires_output_file_path = "outputs/wire_output.txt",
                  std::string occupancy_output_file_path = "outputs/occ_output.txt") {

    std::ofstream out_occupancy(occupancy_output_file_path, std::fstream::out);
    if (!out_occupancy) {
        std::cerr << "Unable to open file: " << occupancy_output_file_path << '\n';
        exit(EXIT_FAILURE);
    }
    out_occupancy << dim_x << ' ' << dim_y << '\n';

    for (const auto& row : occupancy) {
        for (size_t i = 0; i < row.size(); ++i)
            out_occupancy << row[i] << (i == row.size() - 1 ? "" : " ");
        out_occupancy << '\n';
    }
    out_occupancy.close();

    std::ofstream out_wires(wires_output_file_path, std::fstream::out);
    if (!out_wires) {
        std::cerr << "Unable to open file: " << wires_output_file_path << '\n';
        exit(EXIT_FAILURE);
    }

    out_wires << dim_x << ' ' << dim_y << '\n';
    out_wires << num_wires << '\n';

    for (const auto& wire : wires) {
        // NOTICE: we convert to keypoint representation here, using
        // to_validate_format which need to be defined in the bottom of this file
        validate_wire_t keypoints = wire.to_validate_format();
        for (int i = 0; i < keypoints.num_pts; ++i) {
            out_wires << keypoints.p[i].x << ' ' << keypoints.p[i].y;
            if (i < keypoints.num_pts - 1)
                out_wires << ' ';
        }
        out_wires << '\n';
    }

    out_wires.close();
}

bool wires_in_straight_line(const Wire& wire) { return (wire.start_x == wire.end_x) || (wire.start_y == wire.end_y); }

unsigned int number_of_possible_wires(const Wire& wire) {
    if (wires_in_straight_line(wire))
        return 1;

    // Formula from handout
    const unsigned int delta_x = std::abs(wire.start_x - wire.end_x);
    const unsigned int delta_y = std::abs(wire.start_y - wire.end_y);
    return delta_x + delta_y + 2 * (delta_x - 1) * (delta_y - 1);
}

void get_ith_wire(Wire& wire, const unsigned int index) {
    assert(index < number_of_possible_wires(wire));
    if (wires_in_straight_line(wire))
        return;

    const int delta_x = wire.end_x - wire.start_x;
    const int delta_y = wire.end_y - wire.start_y;
    const unsigned int distance_x = std::abs(delta_x);
    const unsigned int distance_y = std::abs(delta_y);

    // Assumes coordinates and distances fit in int.
    const int step_x = delta_x > 0 ? 1 : -1;
    const int step_y = delta_y > 0 ? 1 : -1;

    if (index < distance_x) {
        // Horizontal -> vertical -> horizontal
        const int offset = static_cast<int>(index) + 1;

        wire.mid_x = wire.start_x + step_x * offset;
        wire.mid_y = wire.start_y;
        wire.move_x_start = true;
        wire.move_x_end = true;

    } else if (index < distance_x + distance_y) {
        // Vertical -> horizontal -> vertical
        const int offset = static_cast<int>(index - distance_x) + 1;

        wire.mid_x = wire.start_x;
        wire.mid_y = wire.start_y + step_y * offset;
        wire.move_x_start = false;
        wire.move_x_end = false;

    } else {
        // Three bends
        const auto bend_index = index - (distance_x + distance_y);
        const auto interior_count = (distance_x - 1) * (distance_y - 1);
        const auto position_index = bend_index % interior_count;
        const int x_offset = static_cast<int>(position_index / (distance_y - 1)) + 1;
        const int y_offset = static_cast<int>(position_index % (distance_y - 1)) + 1;
        const bool vertical_first = bend_index >= interior_count;

        wire.mid_x = wire.start_x + step_x * x_offset;
        wire.mid_y = wire.start_y + step_y * y_offset;
        wire.move_x_start = !vertical_first;
        wire.move_x_end = vertical_first;
    }
}

// visit returns if we should continue visiting other cells otherwise we can just halt the iteration early
template <typename Func> void for_each_wire_cell(const Wire& wire, Func visit) {

    // Includes the segment's start, but excludes its end. Returns if we should continue
    // visiting other cells
    auto visit_segment = [&](int start_x, int start_y, int end_x, int end_y) {
        // wire in straight line
        const bool horizontal = (start_y == end_y);
        const bool vertical = (start_x == end_x);
        assert(horizontal || vertical);

        const int step_x = horizontal ? (end_x > start_x ? 1 : -1) : 0;
        const int step_y = vertical ? (end_y > start_y ? 1 : -1) : 0;

        while (start_x != end_x || start_y != end_y) {
            bool continue_visiting_cells = visit(start_x, start_y);
            if (!continue_visiting_cells) {
                return false;
            }
            start_x += step_x;
            start_y += step_y;
        }
        return true;
    };

    if (wires_in_straight_line(wire)) {
        visit_segment(wire.start_x, wire.start_y, wire.end_x, wire.end_y);
    } else {
        const int bend1_x = wire.move_x_start ? wire.mid_x : wire.start_x;
        const int bend1_y = wire.move_x_start ? wire.start_y : wire.mid_y;
        const int bend2_x = wire.move_x_end ? wire.mid_x : wire.end_x;
        const int bend2_y = wire.move_x_end ? wire.end_y : wire.mid_y;

        if (!visit_segment(wire.start_x, wire.start_y, bend1_x, bend1_y))
            return;
        if (!visit_segment(bend1_x, bend1_y, wire.mid_x, wire.mid_y))
            return;
        if (!visit_segment(wire.mid_x, wire.mid_y, bend2_x, bend2_y))
            return;
        if (!visit_segment(bend2_x, bend2_y, wire.end_x, wire.end_y))
            return;
    }
    // always need to visit the end point of the wire
    visit(wire.end_x, wire.end_y);
}

void add_or_remove_wire_from_occupancy(const Wire& wire, std::vector<std::vector<int>>& occupancy, const bool add,
                                       const bool atomic) {
    const int delta = add ? 1 : -1;
    for_each_wire_cell(wire, [&](int x, int y) {
        if (atomic) {
            #pragma omp atomic update
            occupancy[y][x] += delta;
        } else {
            occupancy[y][x] += delta;
        }
        return true;
    });
}

int test_wire_addition_to_occupancy(const Wire& wire, std::vector<std::vector<int>>& occupancy, int cost_to_beat) {
    // Here we compute the cost of adding the wire to occupancy matrix by first adding its whole length
    // and then adding 2*x for each cell. We do this as opposed to adding 2x+1 in each cell to get
    // more aggressive pruning.
    int acc = std::abs(wire.end_x - wire.start_x) + std::abs(wire.end_y - wire.start_y) + 1;

    if (acc >= cost_to_beat) {
        return acc;
    }

    for_each_wire_cell(wire, [&](int x, int y) {
        acc += 2 * occupancy[y][x];
        return acc < cost_to_beat;
    });

    return acc;
}

void parallelize_within_wires_single_iter(std::vector<Wire>& wires, std::vector<std::vector<int>>& occupancy,
                                          const Params& params) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> real_dist(0.0, 1.0);

    unsigned int number_possible_wires;
    int min_cost;
    int starting_cost;

    #pragma omp parallel num_threads(params.num_threads)
    {
        for (auto& wire : wires) {
            #pragma omp single
            {
                number_possible_wires = number_of_possible_wires(wire);
                add_or_remove_wire_from_occupancy(wire, occupancy, false, false);

                starting_cost = std::numeric_limits<int>::max();

                // Simulated Annealing
                if (real_dist(rng) < params.SA_prob) {
                    std::uniform_int_distribution<int> int_dist(0, number_possible_wires - 1);
                    const int wire_index = int_dist(rng);
                    get_ith_wire(wire, wire_index);

                    // Skip candidate search for this wire.
                    number_possible_wires = 0;
                } else {
                    starting_cost = test_wire_addition_to_occupancy(wire, occupancy, std::numeric_limits<int>::max());
                }

                min_cost = starting_cost;
            }

            int local_best_index = -1;
            int local_best_cost = starting_cost;
            Wire temp_wire = wire;

            #pragma omp for schedule(static)
            for (unsigned int i = 0; i < number_possible_wires; i++) {
                get_ith_wire(temp_wire, i);
                int cost = test_wire_addition_to_occupancy(temp_wire, occupancy, local_best_cost);

                if (local_best_cost > cost) {
                    local_best_cost = cost;
                    local_best_index = i;
                }
            }

            if (local_best_index != -1 && local_best_cost < min_cost) {
                min_cost = local_best_cost;
                get_ith_wire(wire, local_best_index);
            }

            #pragma omp barrier

            #pragma omp single
            {
                add_or_remove_wire_from_occupancy(wire, occupancy, true, false);
            }
        }
    }
}

void parallelize_across_wires_single_iter(std::vector<Wire>& wires, std::vector<std::vector<int>>& occupancy,
                                          const Params& params) {


    #pragma omp parallel num_threads(params.num_threads)
    {
        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> real_dist(0.0, 1.0);
        unsigned int num_wires = wires.size();
        unsigned int number_of_batches = (num_wires + params.batch_size - 1) / params.batch_size;
        #pragma omp for schedule(dynamic, 1)
        for (unsigned int batch = 0; batch < number_of_batches; batch++) {
            unsigned int start_wire = batch * params.batch_size;
            int end_wire = std::min(start_wire + params.batch_size, num_wires);
            std::vector<int> best_wires(params.batch_size);

            for (int i = start_wire; i < end_wire; i++) {
                Wire current_wire = wires[i];
                Wire& wire = wires[i];
                add_or_remove_wire_from_occupancy(wire, occupancy, false, true);
                const unsigned int number_possible_wires = number_of_possible_wires(wire);

                // Simulated Annealing
                if (real_dist(rng) < params.SA_prob) {
                    std::uniform_int_distribution<int> int_dist(0, number_possible_wires - 1);
                    const int wire_index = int_dist(rng);
                    best_wires[i - start_wire] = wire_index;
                    continue;
                }

                int min_cost = test_wire_addition_to_occupancy(wire, occupancy, std::numeric_limits<int>::max());
                int local_best_index = -1;
                for (unsigned int i = 0; i < number_possible_wires; i++) {
                    get_ith_wire(wire, i);

                    int cost = test_wire_addition_to_occupancy(wire, occupancy, min_cost);
                    if (min_cost > cost) {
                        min_cost = cost;
                        local_best_index = i;
                    }
                }
                best_wires[i - start_wire] = local_best_index;
                wire = current_wire; // restore the original wire before moving to the next wire
            }
            for (int i = start_wire; i < end_wire; i++) {
                Wire& wire = wires[i];
                if (best_wires[i - start_wire] == -1) {
                    add_or_remove_wire_from_occupancy(wire, occupancy, true, true);
                    continue;
                }
                int local_best_index = best_wires[i - start_wire];
                get_ith_wire(wire, local_best_index);
                add_or_remove_wire_from_occupancy(wire, occupancy, true, true);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    const auto init_start = std::chrono::steady_clock::now();

    std::string input_filename;
    int num_threads = 0;
    double SA_prob = 0.1;
    int SA_iters = 5;
    char parallel_mode = '\0';
    int batch_size = 1;

    int opt;
    while ((opt = getopt(argc, argv, "f:n:p:i:m:b:")) != -1) {
        switch (opt) {
        case 'f':
            input_filename = optarg;
            break;
        case 'n':
            num_threads = atoi(optarg);
            break;
        case 'p':
            SA_prob = atof(optarg);
            break;
        case 'i':
            SA_iters = atoi(optarg);
            break;
        case 'm':
            parallel_mode = *optarg;
            break;
        case 'b':
            batch_size = atoi(optarg);
            break;
        default:
            std::cerr << "Usage: " << argv[0]
                      << " -f input_filename -n num_threads [-p SA_prob] [-i "
                         "SA_iters] -m parallel_mode -b batch_size\n";
            exit(EXIT_FAILURE);
        }
    }

    // Check if required options are provided
    if (empty(input_filename) || num_threads <= 0 || SA_iters <= 0 || (parallel_mode != 'A' && parallel_mode != 'W') ||
        batch_size <= 0) {
        std::cerr << "Usage: " << argv[0]
                  << " -f input_filename -n num_threads [-p SA_prob] [-i SA_iters] "
                     "-m parallel_mode -b batch_size\n";
        exit(EXIT_FAILURE);
    }

    std::cout << "Number of threads: " << num_threads << '\n';
    std::cout << "Simulated annealing probability parameter: " << SA_prob << '\n';
    std::cout << "Simulated annealing iterations: " << SA_iters << '\n';
    std::cout << "Input file: " << input_filename << '\n';
    std::cout << "Parallel mode: " << parallel_mode << '\n';
    std::cout << "Batch size: " << batch_size << '\n';

    std::ifstream fin(input_filename);

    if (!fin) {
        std::cerr << "Unable to open file: " << input_filename << ".\n";
        exit(EXIT_FAILURE);
    }

    int dim_x, dim_y;
    int num_wires;

    /* Read the grid dimension and wire information from file */
    fin >> dim_x >> dim_y >> num_wires;

    std::vector<Wire> wires(num_wires);
    std::vector occupancy(dim_y, std::vector<int>(dim_x));
    std::cout << "Question Spec: dim_x=" << dim_x << ", dim_y=" << dim_y << ", number of wires=" << num_wires << '\n';

    // TODO (student code start): Read the wire information from file,
    // you may need to change this if you define the wire structure differently.
    for (auto& wire : wires) {
        fin >> wire.start_x >> wire.start_y >> wire.end_x >> wire.end_y;
        wire.move_x_start = true;
        wire.move_x_end = false;
        wire.mid_x = wire.end_x;
        wire.mid_y = wire.start_y;
    }

    /* Initialize any additional data structures needed in the algorithm */
    Params params{.num_threads = num_threads, .SA_prob = SA_prob, .SA_iters = SA_iters, .batch_size = batch_size};
    for (const auto& wire : wires) {
        add_or_remove_wire_from_occupancy(wire, occupancy, true, false);
    }

    // Student code end
    const double init_time =
        std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - init_start)
            .count();
    std::cout << "Initialization time (sec): " << std::fixed << std::setprecision(10) << init_time << '\n';

    const auto compute_start = std::chrono::steady_clock::now();

    // initialize wires
    // Within wires
    for (int i = 0; i < params.SA_iters; ++i) {
        if (parallel_mode == 'W') {
            parallelize_within_wires_single_iter(wires, occupancy, params);
        } else {
            parallelize_across_wires_single_iter(wires, occupancy, params);
        }
    }

    // Student code end
    // DON'T CHANGE THE FOLLOWING CODE
    const double compute_time =
        std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - compute_start)
            .count();
    std::cout << "Computation time (sec): " << compute_time << '\n';

    /* wire to run check on wires and occupancy */
    wr_checker checker(wires, occupancy);
    checker.validate();

    /* Write wires and occupancy matrix to files */
    print_stats(occupancy);
    write_output(wires, num_wires, occupancy, dim_x, dim_y);
}

/* TODO (student): implement to_validate_format to convert Wire to
  validate_wire_t keypoint representation in order to run checker and
  write output
*/
validate_wire_t Wire::to_validate_format(void) const {
    validate_wire_t w{};
    int number_of_points = 1;
    const int bend1_x = move_x_start ? mid_x : start_x;
    const int bend1_y = move_x_start ? start_y : mid_y;
    const int bend2_x = move_x_end ? mid_x : end_x;
    const int bend2_y = move_x_end ? end_y : mid_y;
    w.p[0].x = start_x;
    w.p[0].y = start_y;

    if (start_x != bend1_x || start_y != bend1_y) {
        w.p[number_of_points].x = bend1_x;
        w.p[number_of_points].y = bend1_y;
        number_of_points++;
    }

    if (bend1_x != mid_x || bend1_y != mid_y) {
        w.p[number_of_points].x = mid_x;
        w.p[number_of_points].y = mid_y;
        number_of_points++;
    }

    if (mid_x != bend2_x || mid_y != bend2_y) {
        w.p[number_of_points].x = bend2_x;
        w.p[number_of_points].y = bend2_y;
        number_of_points++;
    }

    if (bend2_x != end_x || bend2_y != end_y) {
        w.p[number_of_points].x = end_x;
        w.p[number_of_points].y = end_y;
        number_of_points++;
    }

    w.num_pts = number_of_points;
    return w;
}
