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

// Have to use power of two block sizes as MPI_Type_create_darray behaves
// strangely otherwise
#ifndef LU_BLOCK_SIZE
#define LU_BLOCK_SIZE 256
#endif

#define SMALL_BLOCK_SIZE ((GEMM_BLOCK_SIZE < LU_BLOCK_SIZE) ? GEMM_BLOCK_SIZE : LU_BLOCK_SIZE)

void lu(int n, double *A) {
#pragma scop
  int world_size;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  int psizes[2] = {0, 0};
  MPI_Dims_create(world_size, 2, psizes);

  int n_old = n;
  n = LU_BLOCK_SIZE * (n / LU_BLOCK_SIZE);

  MPI_Datatype *dist_types = (MPI_Datatype *) malloc(world_size * sizeof(MPI_Datatype));

  for (int i = 0; i < world_size; i++) {
    int sizes[2] = {n, n};
    int distribs[2] = {MPI_DISTRIBUTE_CYCLIC, MPI_DISTRIBUTE_CYCLIC};
    int dargs[2] = {LU_BLOCK_SIZE, LU_BLOCK_SIZE};

    MPI_Type_create_darray(world_size, i, 2,
                           sizes, distribs, dargs, psizes,
                           MPI_ORDER_C, MPI_DOUBLE, &dist_types[i]);
    MPI_Type_commit(&dist_types[i]);
  }

  int dist_size;
  MPI_Type_size(dist_types[rank], &dist_size);

  double *B_ = (double *) malloc(dist_size);

  if (!B_) {
    printf("Error: Out of memory :(\n");
    exit(-1);
  }

  int position = 0;
  MPI_Pack(A, 1, dist_types[rank], B_, dist_size, &position, MPI_COMM_WORLD) ;

  int m = n / psizes[0];

  double *B;
  int ldb;
  pad_matrix(m, m, B_, m, &B, &ldb);

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

  int chunk_size = LU_BLOCK_SIZE * psizes[0];

  int ro(int bk) {
    int chunk_idx = bk / psizes[0];
    int block_idx = bk % psizes[0];

    int result = chunk_idx * LU_BLOCK_SIZE;
    if (row_rank < block_idx) result += LU_BLOCK_SIZE;
    return result;
  }

  int co(int bk) {
    int chunk_idx = bk / psizes[0];
    int block_idx = bk % psizes[0];

    int result = chunk_idx * LU_BLOCK_SIZE;
    if (col_rank < block_idx) result += LU_BLOCK_SIZE;
    return result;
  }

  int n_blocks = n / LU_BLOCK_SIZE;

  int ldl = LDA_MULTIPLE * (LU_BLOCK_SIZE / LDA_MULTIPLE + 1);

  double *U_p = (double *) malloc(LU_BLOCK_SIZE * m * sizeof(double));
  double *L_p = (double *) malloc(m * ldl * sizeof(double));

  double *LU_k = (double *) malloc(LU_BLOCK_SIZE * LU_BLOCK_SIZE * sizeof(double));
  double *U_k = (double *) malloc(LU_BLOCK_SIZE * m * sizeof(double));
  double *L_k = (double *) malloc(m * ldl * sizeof(double));

  double *q = (double *) malloc(LU_BLOCK_SIZE * sizeof(double));

  MPI_Request *row_request = (MPI_Request *) malloc(sizeof(MPI_Request));
  MPI_Request *col_request = (MPI_Request *) malloc(sizeof(MPI_Request));

  #pragma omp parallel
  for (int bk = 0; bk < n_blocks; bk++) {
    int block_idx = bk % psizes[0];

    int ro_k = ro(bk);
    int ro_n = ro(bk + 1);

    int co_k = co(bk);
    int co_n = co(bk + 1);

    if (bk > 0 && row_rank == block_idx && col_rank == block_idx) {
      gemm(LU_BLOCK_SIZE, LU_BLOCK_SIZE, LU_BLOCK_SIZE,
            -1, L_p, ldl,
            U_p, m - ro_k,
            1, B + co_k * ldb + ro_k, ldb);
    }

    #pragma omp barrier

    #pragma omp master
    if (row_rank == block_idx || col_rank == block_idx){
      if (row_rank == block_idx && col_rank == block_idx) {
        for (int k = ro_k; k < ro_k + LU_BLOCK_SIZE; k++) {
          double B_kk = B[k * ldb + k];
          for (int i = k + 1; i < co_k + LU_BLOCK_SIZE; i++) B[i * ldb + k] /= B_kk;

          for (int i = k + 1; i < co_k + LU_BLOCK_SIZE; i++) {
            for (int j = k + 1; j < ro_k + LU_BLOCK_SIZE; j++) {
              B[i * ldb + j] -= B[i * ldb + k] * B[k * ldb + j];
            }
          }
        }

        for (int i = co_k; i < co_k + LU_BLOCK_SIZE; i++) {
          for (int j = ro_k; j < ro_k + LU_BLOCK_SIZE; j++)
            LU_k[(i - co_k) * LU_BLOCK_SIZE + (j - ro_k)] = B[i * ldb + j];
        }
      }

      if (row_rank == block_idx) {
        MPI_Ibcast(LU_k, LU_BLOCK_SIZE * LU_BLOCK_SIZE, MPI_DOUBLE, block_idx, col_comm, col_request);
      }

      if (col_rank == block_idx) {
        MPI_Ibcast(LU_k, LU_BLOCK_SIZE * LU_BLOCK_SIZE, MPI_DOUBLE, block_idx, row_comm, row_request);
      }
    }

    if (bk > 0) {
      if (col_rank == block_idx) {
        gemm(LU_BLOCK_SIZE, m - ro_n, LU_BLOCK_SIZE,
              -1, L_p, ldl,
              U_p + (ro_n - ro_k), m - ro_k,
              1, B + co_k * ldb + ro_n, ldb);
      }

      if (row_rank == block_idx) {
        gemm(m - co_n, LU_BLOCK_SIZE, LU_BLOCK_SIZE,
              -1, L_p + (co_n - co_k) * ldl, ldl,
              U_p, m - ro_k,
              1, B + co_n * ldb + ro_k, ldb);
      }
    }

    #pragma omp master
    {
      if (row_rank == block_idx) MPI_Wait(col_request, MPI_STATUS_IGNORE);
      if (col_rank == block_idx) MPI_Wait(row_request, MPI_STATUS_IGNORE);
    }

    #pragma omp barrier

    if (col_rank == block_idx) {
      #pragma omp for
      for (int u = ro_n; u < m - SMALL_BLOCK_SIZE + 1; u += SMALL_BLOCK_SIZE) {
        for (int k = co_k; k < co_k + LU_BLOCK_SIZE; k++) {
          for (int i = k + 1; i < co_k + LU_BLOCK_SIZE; i++) {
            for (int j = u; j < u + SMALL_BLOCK_SIZE; j++) {
              B[i * ldb + j] -= LU_k[(i - co_k) * LU_BLOCK_SIZE + (k - co_k)] * B[k * ldb + j];
            }
          }
        }

        for (int i = co_k; i < co_k + LU_BLOCK_SIZE; i++) {
          for (int j = u; j < u + SMALL_BLOCK_SIZE; j++) {
            U_k[(i - co_k) * (m - ro_n) + (j - ro_n)] = B[i * ldb + j];
          }
        }
      }
    }

    if (row_rank == block_idx) {
      #pragma omp master
      for (int k = ro_k; k < ro_k + LU_BLOCK_SIZE; k++)
        q[k - ro_k] = 1 / LU_k[(k - ro_k) * LU_BLOCK_SIZE + (k - ro_k)];

      #pragma omp barrier

      #pragma omp for schedule(static, GEMM_BLOCK_SIZE)
      for (int i = co_n; i < m; i ++) {
        for (int k = ro_k; k < ro_k + LU_BLOCK_SIZE; k++) {
          B[i * ldb + k] *= q[k - ro_k];

          double Bik = B[i * ldb + k];
          for (int j = k + 1; j < ro_k + LU_BLOCK_SIZE; j++) {
            B[i * ldb + j] -= Bik * LU_k[(k - ro_k) * LU_BLOCK_SIZE + (j - ro_k)];
          }
        }

        for (int j = ro_k; j < ro_k + LU_BLOCK_SIZE; j++) {
          L_k[(i - co_n) * ldl + (j - ro_k)] = B[i * ldb + j];
        }
      }
    }

    #pragma omp barrier

    #pragma omp master
    {
      MPI_Ibcast(L_k, (m - co_n) * ldl, MPI_DOUBLE,
                 block_idx, row_comm, row_request);

      MPI_Ibcast(U_k, LU_BLOCK_SIZE * (m - ro_n), MPI_DOUBLE,
                 block_idx, col_comm, col_request);
    }

    if (bk > 0) {
      gemm(m - co_n, m - ro_n, LU_BLOCK_SIZE,
            -1, L_p + (co_n - co_k) * ldl, ldl,
            U_p + (ro_n - ro_k), m - ro_k,
            1, B + co_n * ldb + ro_n, ldb);
    }

    #pragma omp barrier

    #pragma omp master
    {
      MPI_Wait(row_request, MPI_STATUS_IGNORE);
      MPI_Wait(col_request, MPI_STATUS_IGNORE);

      swap(&U_p, &U_k);
      swap(&L_p, &L_k);
    }

    #pragma omp barrier
  }

  free(q);

  for (int i = 0; i < m; i++) {
    for (int j = 0; j < m; j++) {
      B_[i * m + j] = B[i * ldb + j];
    }
  }

  MPI_Request *recv_requests;
  if (rank == 0) {
    recv_requests = (MPI_Request *) malloc(world_size * sizeof(MPI_Request));
    for (int i = 0; i < world_size; i++) {
      MPI_Irecv(A, 1, dist_types[i],
                i, 0, MPI_COMM_WORLD, &recv_requests[i]);
    }
  }

  MPI_Request send_request;
  MPI_Isend(B_, dist_size / sizeof(double), MPI_DOUBLE,
            0, 0, MPI_COMM_WORLD, &send_request);

  if (rank == 0) {
    MPI_Waitall(world_size, recv_requests, MPI_STATUSES_IGNORE);
    free(recv_requests);
  }

  MPI_Wait(&send_request, MPI_STATUS_IGNORE);

  if (rank == 0) {
    for (int k = n; k < n_old; k++) {
      double A_kk = A[k * n + k];
      for (int i = k + 1; i < n; i++) A[i * n + k] /= A_kk;

      for (int i = k + 1; i < n; i++) {
        for (int j = k + 1; j < n; j++) {
          A[i * n + j] -= A[i * n + k] * A[k * n + j];
        }
      }
    }
  }

  free(dist_types);

  free(B_);
  free(B);

  free(LU_k);
  free(U_k);
  free(L_k);

  free(U_p);
  free(L_p);
  #pragma endscop
}
