/**
 * This version is stamped on May 10, 2016
 *
 * Contact:
 *   Louis-Noel Pouchet <pouchet.ohio-state.edu>
 *   Tomofumi Yuki <tomofumi.yuki.fr>
 *
 * Web address: http://polybench.sourceforge.net
 */
/* lu.c: this file is part of PolyBench/C */

#include <mpi.h>
#include <pthread.h>

#include <gemm.h>
#define PARALLEL_GEMM
#include <gemm.h>

/* Include polybench common header. */
#include <polybench.h>

void print(int n, double *A) {
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) printf("%.2lf ", A[i * n + j]);
    printf("\n");
  }
}

void swap(double **a, double **b) {
  void *tmp = *a;
  *a = *b;
  *b = tmp;
}

/* Main computational kernel. The whole function will be timed,
   including the call and return. */
void lu(int n, double *A)
{
#pragma scop
  int world_size;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  int psizes[2] = {0, 0};
  MPI_Dims_create(world_size, 2, psizes);

  int block_size = 40;
  while (n % (psizes[0] * block_size) != 0) block_size -= 1;

  MPI_Datatype *dist_types = (MPI_Datatype *) malloc(world_size * sizeof(MPI_Datatype));

  for (int i = 0; i < world_size; i++) {
    int sizes[2] = {n, n};
    int distribs[2] = {MPI_DISTRIBUTE_CYCLIC, MPI_DISTRIBUTE_CYCLIC};
    int dargs[2] = {block_size, block_size};

    MPI_Type_create_darray(world_size, i, 2,
                           sizes, distribs, dargs, psizes,
                           MPI_ORDER_C, MPI_DOUBLE, &dist_types[i]);
    MPI_Type_commit(&dist_types[i]);
  }

  int dist_size;
  MPI_Type_size(dist_types[rank], &dist_size);

  double *B = (double *) malloc(dist_size);

  int position = 0;
  MPI_Pack(A, 1, dist_types[rank], B, dist_size, &position, MPI_COMM_WORLD) ;

  int m = n / psizes[0];

  int row_idx = rank / psizes[0];
  int col_idx = rank % psizes[0];

  MPI_Comm row_comm;
  MPI_Comm_split(MPI_COMM_WORLD, row_idx, rank, &row_comm);

  MPI_Comm col_comm;
  MPI_Comm_split(MPI_COMM_WORLD, col_idx, rank, &col_comm);

  int row_rank;
  MPI_Comm_rank(row_comm, &row_rank);

  int col_rank;
  MPI_Comm_rank(col_comm, &col_rank);

  int chunk_size = block_size * psizes[0];


  int ro(int bk) {
    int chunk_idx = bk / psizes[0];
    int block_idx = bk % psizes[0];

    int result = chunk_idx * block_size;
    if (row_rank < block_idx) result += block_size;
    return result;
  }

  int co(int bk) {
    int chunk_idx = bk / psizes[0];
    int block_idx = bk % psizes[0];

    int result = chunk_idx * block_size;
    if (col_rank < block_idx) result += block_size;
    return result;
  }

  int n_blocks = n / block_size;

  double *U_p = (double *) malloc(block_size * m * sizeof(double));
  double *L_p = (double *) malloc(m * block_size * sizeof(double));

  double *LU_k = (double *) malloc(block_size * block_size * sizeof(double));
  double *U_k = (double *) malloc(block_size * m * sizeof(double));
  double *L_k = (double *) malloc(m * block_size * sizeof(double));

  double *q = (double *) malloc(block_size * sizeof(double));

  #pragma omp parallel
  for (int bk = 0; bk < n_blocks; bk++) {
    int block_idx = bk % psizes[0];

    int ro_k = ro(bk);
    int ro_n = ro(bk + 1);

    int co_k = co(bk);
    int co_n = co(bk + 1);

    if (bk > 0) {
      if (row_rank == block_idx && col_rank == block_idx) {
        #pragma omp master
        gemm(block_size, block_size, block_size,
              -1, L_p, block_size,
              U_p, m - ro_k,
              1, B + co_k * m + ro_k, m);
      }

      if (col_rank == block_idx) {
        pgemm(block_size, m - ro_n, block_size,
              -1, L_p, block_size,
              U_p + (ro_n - ro_k), m - ro_k,
              1, B + co_k * m + ro_n, m);
      }

      if (row_rank == block_idx) {
        pgemm(m - co_n, block_size, block_size,
              -1, L_p + (co_n - co_k) * block_size, block_size,
              U_p, m - ro_k,
              1, B + co_n * m + ro_k, m);
      }
    }

    #pragma omp master
    if (row_rank == block_idx || col_rank == block_idx){
      #pragma task
      {
        if (row_rank == block_idx && col_rank == block_idx) {
          for (int k = ro_k; k < ro_k + block_size; k++) {
            double B_kk = B[k * m + k];
            for (int i = k + 1; i < co_k + block_size; i++) B[i * m + k] /= B_kk;

            for (int i = k + 1; i < co_k + block_size; i++) {
              for (int j = k + 1; j < ro_k + block_size; j++) {
                B[i * m + j] -= B[i * m + k] * B[k * m + j];
              }
            }
          }

          for (int i = co_k; i < co_k + block_size; i++) {
            for (int j = ro_k; j < ro_k + block_size; j++)
              LU_k[(i - co_k) * block_size + (j - ro_k)] = B[i * m + j];
          }
        }

        MPI_Request row_request, col_request;
        if (row_rank == block_idx) {
          MPI_Ibcast(LU_k, block_size * block_size, MPI_DOUBLE, block_idx, col_comm, &col_request);
        }

        if (col_rank == block_idx) {
          MPI_Ibcast(LU_k, block_size * block_size, MPI_DOUBLE, block_idx, row_comm, &row_request);
        }

        if (row_rank == block_idx) MPI_Wait(&col_request, MPI_STATUS_IGNORE);
        if (col_rank == block_idx) MPI_Wait(&row_request, MPI_STATUS_IGNORE);
      }
    }
    
    #pragma omp taskwait

    if (col_rank == block_idx) {
      #pragma omp for
      for (int j = ro_n; j < m; j++) {
        for (int k = co_k; k < co_k + block_size; k++) {
          for (int i = k + 1; i < co_k + block_size; i++) {
            double LUik = LU_k[(i - co_k) * block_size + (k - co_k)];
              B[i * m + j] -= LUik * B[k * m + j];
          }
        }

        for (int i = co_k; i < co_k + block_size; i++) {
          U_k[(i - co_k) * (m - ro_n) + (j - ro_n)] = B[i * m + j];
        }
      }
    }

    if (row_rank == block_idx) {
      #pragma omp master
      for (int k = ro_k; k < ro_k + block_size; k++)
        q[k - ro_k] = 1 / LU_k[(k - ro_k) * block_size + (k - ro_k)];

      #pragma omp for schedule(static, BI)
      for (int i = co_n; i < m; i ++) {
        for (int k = ro_k; k < ro_k + block_size; k++) {
          B[i * m + k] *= q[k - ro_k];

          double Bik = B[i * m + k];
          for (int j = k + 1; j < ro_k + block_size; j++) {
            B[i * m + j] -= Bik * LU_k[(k - ro_k) * block_size + (j - ro_k)];
          }
        }

        for (int j = ro_k; j < ro_k + block_size; j++) {
          L_k[(i - co_n) * block_size + (j - ro_k)] = B[i * m + j];
        }
      }
    }

    #pragma omp barrier

    #pragma omp master
    {
      #pragma omp task
      {
        MPI_Request row_request;
        MPI_Ibcast(L_k, (m - co_n) * block_size, MPI_DOUBLE,
                   block_idx, row_comm, &row_request);

        MPI_Request col_request;
        MPI_Ibcast(U_k, block_size * (m - ro_n), MPI_DOUBLE,
                   block_idx, col_comm, &col_request);

        MPI_Wait(&row_request, MPI_STATUS_IGNORE);
        MPI_Wait(&col_request, MPI_STATUS_IGNORE);
      }
    }

    if (bk > 0) {
      pgemm(m - co_n, m - ro_n, block_size,
            -1, L_p + (co_n - co_k) * block_size, block_size,
            U_p + (ro_n - ro_k), m - ro_k,
            1, B + co_n * m + ro_n, m);
    }

    #pragma omp taskwait

    #pragma omp master
    {
      swap(&U_p, &U_k);
      swap(&L_p, &L_k);
    }
  }

  free(q);

  // MPI_Request *recv_requests;
  // if (rank == 0) {
  //   recv_requests = (MPI_Request *) malloc(world_size * sizeof(MPI_Request));
  //   for (int i = 0; i < world_size; i++) {
  //     MPI_Irecv(A, 1, dist_types[i],
  //               i, 0, MPI_COMM_WORLD, &recv_requests[i]);
  //   }
  // }

  // MPI_Request send_request;
  // MPI_Isend(B, dist_size / sizeof(double), MPI_DOUBLE,
  //           0, 0, MPI_COMM_WORLD, &send_request);

  // if (rank == 0) {
  //   MPI_Waitall(world_size, recv_requests, MPI_STATUSES_IGNORE);
  //   free(recv_requests);
  // }

  // MPI_Wait(&send_request, MPI_STATUS_IGNORE);

  free(dist_types);

  free(B);

  free(LU_k);
  free(U_k);
  free(L_k);

  free(U_p);
  free(L_p);
  #pragma endscop
}
