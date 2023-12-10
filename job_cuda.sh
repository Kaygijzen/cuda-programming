#!/bin/sh
#SBATCH --time=00:15:00
#SBATCH -N 1
#SBATCH --gres=gpu:1

mpirun -np 1 bin/cgc_cuda /var/scratch/bwn200/HPC_data/spring_data_m.npy /var/scratch/bwn200/HPC_data/spring_labels_m_3x20.txt --max-iterations 25 --output "cuda.txt"