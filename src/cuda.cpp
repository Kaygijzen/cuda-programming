#include <chrono>
#include <iostream>
#include <mpi.h>

#include "common.h"
#include "cuda/module.h"

std::pair<std::vector<int>, std::vector<int>> calculate_scatter(int n, int size) {
    int count = n / size;
    int remainder = n % size;
    auto counts = std::vector<int>(size, count);
    auto displacements = std::vector<int>(size, 0);

    for (int i = 0; i < size; i++) {
        if (i < remainder) {
            // The first 'remainder' ranks get 'count + 1' tasks each
            counts[i] += 1;
            displacements[i] = i * (count + 1);
        } else {
            // The remaining 'size - remainder' ranks get 'count' task each
            displacements[i] = i * count + remainder;
        }
    }

    return std::make_pair(counts, displacements);
}

/**
 * This function returns a matrix of size (num_row_labels, num_col_labels)
 * that stores the average value for each combination of row label and
 * column label. In other words, the entry at coordinate (x, y) is the
 * average over all input values having row label x and column label y.
 */
std::vector<float> calculate_cluster_average(
    int num_rows,
    int num_cols,
    int num_row_labels,
    int num_col_labels,
    const float* matrix,
    const int* row_labels,
    const int* col_labels,
    int row_displacement,
    int num_rows_recv) {

    int num_clusters = num_row_labels * num_col_labels;
    auto local_cluster_sum =
        std::vector<double>(num_clusters, 0.0);
    auto local_cluster_size = std::vector<int>(num_clusters, 0);

    auto cluster_ids = std::vector<int>(num_rows_recv*num_cols);

    call_cluster_id_kernel(
        num_rows,
        num_cols,
        num_col_labels,
        row_labels,
        col_labels,
        cluster_ids.data(),
        row_displacement,
        num_rows_recv);

    for (int i = 0; i < num_rows_recv; i++) {
        for (int j = 0; j < num_cols; j++) {
            auto item = matrix[(i + row_displacement) * num_cols + j];
            int c = cluster_ids[i * num_cols + j];
            
            local_cluster_sum[c] += item;
            local_cluster_size[c] += 1;
        }
    }

    auto cluster_sum =
        std::vector<double>(num_clusters, 0.0);
    auto cluster_size = std::vector<int>(num_clusters, 0);

    for (int i = 0; i < cluster_sum.size(); i++) {
        MPI_Allreduce(
            local_cluster_sum.data(),
            cluster_sum.data(),
            cluster_sum.size(),
            MPI_DOUBLE,
            MPI_SUM,
            MPI_COMM_WORLD);
        MPI_Allreduce(
            local_cluster_size.data(),
            cluster_size.data(),
            cluster_size.size(),
            MPI_INT,
            MPI_SUM,
            MPI_COMM_WORLD);
    }

    auto cluster_avg = std::vector<float>(num_clusters);

    call_cluster_average_kernel(
        num_row_labels,
        num_col_labels,
        cluster_sum.data(),
        cluster_size.data(),
        cluster_avg.data());

    return cluster_avg;
}

/**
 * Perform one iteration of the co-clustering algorithm. This function updates
 * the labels in both `row_labels` and `col_labels`, and returns the total
 * number of labels that changed (i.e., the number of rows and columns that
 * were reassigned to a different label).
 */
std::pair<int, double> cluster_serial_iteration(
    int num_rows,
    int num_cols,
    int num_row_labels,
    int num_col_labels,
    const float* matrix,
    label_type* row_labels,
    label_type* col_labels,
    int rank,
    const int* row_counts,
    const int* row_displacements,
    const int* col_counts,
    const int* col_displacements) {

    int num_rows_recv = row_counts[rank];
    int row_displacement = row_displacements[rank];

    //// SECTION: calculate_cluster_average
    // Calculate the average value per cluster
    auto cluster_avg = calculate_cluster_average(
        num_rows,
        num_cols,
        num_row_labels,
        num_col_labels,
        matrix,
        row_labels,
        col_labels,
        row_displacement,
        num_rows_recv);

    //// SECTION: update_row_labels
    auto scatter_row_labels = std::vector<label_type>(num_rows_recv, 0);
    MPI_Scatterv(row_labels,
                row_counts,
                row_displacements,
                MPI_INT,
                scatter_row_labels.data(),
                num_rows_recv,
                MPI_INT,
                0,
                MPI_COMM_WORLD);

    // Update labels along the rows
    auto [num_rows_updated, _] = call_update_row_labels_kernel(
        num_rows,
        num_cols,
        num_row_labels,
        num_col_labels,
        matrix,
        scatter_row_labels.data(),
        col_labels,
        cluster_avg.data(),
        row_displacement,
        num_rows_recv);

    // Synchronize row_labels and num_rows_updated
    MPI_Allgatherv(scatter_row_labels.data(),
                   num_rows_recv,
                   MPI_INT,
                   row_labels,
                   row_counts,
                   row_displacements,
                   MPI_INT,
                   MPI_COMM_WORLD);
    MPI_Allreduce(&num_rows_updated, &num_rows_updated, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);

    //// SECTION: update_col_labels
    int num_cols_recv = col_counts[rank];
    auto scatter_col_labels = std::vector<label_type>(num_cols_recv, 0);
    MPI_Scatterv(col_labels,
                col_counts,
                col_displacements,
                MPI_INT,
                scatter_col_labels.data(),
                num_cols_recv,
                MPI_INT,
                0,
                MPI_COMM_WORLD);

    int col_displacement = col_displacements[rank];

    // Update the labels along the columns using the CUDA kernel TODO: num_cols_updated and total_dist
    auto [num_cols_updated, total_dist] = call_update_col_labels_kernel(
        num_rows,
        num_cols,
        num_row_labels,
        num_col_labels,
        matrix,
        row_labels,
        scatter_col_labels.data(),
        cluster_avg.data(),
        col_displacement,
        num_cols_recv);

    // Synchronize col_labels, num_cols_updated and total_dist
    MPI_Allgatherv(scatter_col_labels.data(),
                   num_cols_recv,
                   MPI_INT,
                   col_labels,
                   col_counts,
                   col_displacements,
                   MPI_INT,
                   MPI_COMM_WORLD);

    MPI_Allreduce(&num_cols_updated, &num_cols_updated, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&total_dist, &total_dist, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    return {num_rows_updated + num_cols_updated, total_dist};
}

/**
 * Repeatedly calls `cluster_serial_iteration` to iteratively update the
 * labels along the rows and columns. This function performs
 * `max_iterations` iterations or until convergence.
 */
void cluster_serial(
    int num_rows,
    int num_cols,
    int num_row_labels,
    int num_col_labels,
    float* matrix,
    label_type* row_labels,
    label_type* col_labels,
    int max_iterations = 25) {
    int iteration = 0;
    auto before = std::chrono::high_resolution_clock::now();

    int size, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // Calculate values to scatter the row_labels and col_labels on using our helper function.
    std::vector<int> row_counts, row_displacements, col_counts, col_displacements;
    auto row_scatter = calculate_scatter(num_rows, size);
    row_counts = row_scatter.first;
    row_displacements = row_scatter.second;

    auto col_scatter = calculate_scatter(num_cols, size);
    col_counts = col_scatter.first;
    col_displacements = col_scatter.second;

    while (iteration < max_iterations) {
        auto [num_updated, total_dist] = cluster_serial_iteration(
            num_rows,
            num_cols,
            num_row_labels,
            num_col_labels,
            matrix,
            row_labels,
            col_labels,
            rank,
            row_counts.data(),
            row_displacements.data(),
            col_counts.data(),
            col_displacements.data());

        iteration++;

        if (rank == 0) {
            auto average_dist = total_dist / (num_rows * num_cols);
            std::cout << "iteration " << iteration << ": " << num_updated
                    << " labels were updated, average error is " << average_dist
                    << "\n";
        }

        if (num_updated == 0) {
            break;
        }
    }

    auto after = std::chrono::high_resolution_clock::now();
    auto time_seconds = std::chrono::duration<double>(after - before).count();
    if (rank == 0) {
        std::cout << "clustering time total: " << time_seconds << " seconds\n";
        std::cout << "clustering time per iteration: " << (time_seconds / iteration)
                << " seconds\n";
    }
}

int main(int argc, const char* argv[]) {
    MPI_Init(NULL, NULL);

    std::string output_file;
    std::vector<float> matrix;
    std::vector<label_type> row_labels, col_labels;
    int num_rows = 0, num_cols = 0;
    int num_row_labels = 0, num_col_labels = 0;
    int max_iter = 0;

    auto before = std::chrono::high_resolution_clock::now();

    // Parse arguments
    if (!parse_arguments(
            argc,
            argv,
            &num_rows,
            &num_cols,
            &num_row_labels,
            &num_col_labels,
            &matrix,
            &row_labels,
            &col_labels,
            &output_file,
            &max_iter)) {
        return EXIT_FAILURE;
    }

    // Cluster labels
    cluster_serial(
        num_rows,
        num_cols,
        num_row_labels,
        num_col_labels,
        matrix.data(),
        row_labels.data(),
        col_labels.data(),
        max_iter);

    int rank; 
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    
    if (rank == 0) {
        // Write resulting labels
        write_labels(
        output_file,
        num_rows,
        num_cols,
        row_labels.data(),
        col_labels.data());

        auto after = std::chrono::high_resolution_clock::now();
        auto time_seconds = std::chrono::duration<double>(after - before).count();

        std::cout << "total execution time: " << time_seconds << " seconds\n";
    }

    return EXIT_SUCCESS;
}