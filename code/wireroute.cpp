/**
 * Parallel VLSI Wire Routing via OpenMP
 * Name 1(andrew_id 1), Name 2(andrew_id 2)
 */

#include "wireroute.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <omp.h>
#include <unistd.h>

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

    // Student code end
    const double init_time =
        std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - init_start)
            .count();
    std::cout << "Initialization time (sec): " << std::fixed << std::setprecision(10) << init_time << '\n';

    const auto compute_start = std::chrono::steady_clock::now();

    /* TODO (student code start): Implement the wire routing algorithm here and
      feel free to structure the algorithm into different functions.
      Don't use global variables.
      Use OpenMP to parallelize the algorithm.
    */

    // initialize wires
    // Within wires
    if (parallel_mode == 'W') {
        // within wires
    } else {
        // across wires
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
    validate_wire_t w;

    return w;
}
