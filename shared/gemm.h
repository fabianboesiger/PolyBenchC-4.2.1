#include <immintrin.h>

#ifndef GEMM_BLOCK_SIZE
#define GEMM_BLOCK_SIZE 48
#endif

inline __attribute__((always_inline)) void block_mul(int ni, int nj, double beta, double *C, int ldc) {
  for (int i = 0; i < ni; i++) {
    for (int j = 0; j < nj; j++) {
      C[i * ldc + j] *= beta;
    }
  }
}

#define RI 3
#define RJ 4

#if GEMM_BLOCK_SIZE % RI != 0
#error RI needs to divide GEMM_BLOCK_SIZE
#endif

#if GEMM_BLOCK_SIZE % (4 * RJ) != 0
#error 4 * RJ needs to divide GEMM_BLOCK_SIZE
#endif

inline __attribute__((always_inline)) void micro_mm(
    int nk,
    double alpha, double *A, int lda,
    double *B, int ldb,
    double *C, int ldc)
{
  __m256d valpha = _mm256_set1_pd(alpha);

  __m256d sums[RI][RJ] = {_mm256_set1_pd(0)};

  for (int k = 0; k < nk; k++) {
    for (int j = 0; j < RJ; j++) {
      __m256d b = _mm256_loadu_pd(&B[k * ldb + j * 4]);
      for (int i = 0; i < RI; i++) {
        __m256d a = _mm256_set1_pd(A[i * lda + k]);
        sums[i][j] = _mm256_fmadd_pd(a, b, sums[i][j]);
      }
    }
  }

  for (int i = 0; i < RI; i++) {
    for (int j = 0; j < RJ; j++) {
      __m256d c = _mm256_loadu_pd(&C[i * ldc + 4 * j]);
      c = _mm256_fmadd_pd(valpha, sums[i][j], c);
      _mm256_storeu_pd(&C[i * ldc + 4 * j], c);
    }
  }
}

inline __attribute__((always_inline)) void fast_mini_mm(
    int nk,
    double alpha, double *A, int lda,
    double *B, int ldb,
    double *C, int ldc)
{
  for (int i = 0; i < GEMM_BLOCK_SIZE - RI + 1; i += RI) {
    for (int j = 0; j < GEMM_BLOCK_SIZE - 4 * RJ + 1; j += 4 * RJ) {
      micro_mm(nk, alpha, &A[i * lda], lda, &B[j], ldb, &C[i * ldc + j], ldc);
    }
  }
}

inline __attribute ((always_inline)) void mini_mm(
    int ni, int nj, int nk,
    double alpha, double *A, int lda,
    double *B, int ldb,
    double *C, int ldc)
{
	for (int i = 0; i < ni; i++) {
		for (int j = 0; j < nj; j++) {
			double sum = 0;
			for (int k = 0; k < nk; k++) {
				sum += A[i * lda + k] * B[k * ldb + j];
			}
			C[i * ldc + j] += alpha * sum;
		}
	}
}

void mm(
    int ni, int nj, int nk,
    double alpha, double *A, int lda,
    double *B, int ldb,
    double beta, double *C, int ldc)
{
  #pragma omp for schedule(static, 1)
  for (int i = 0; i < ni - GEMM_BLOCK_SIZE + 1; i += GEMM_BLOCK_SIZE) {
    for (int j = 0; j < nj - GEMM_BLOCK_SIZE + 1; j += GEMM_BLOCK_SIZE) {
      block_mul(GEMM_BLOCK_SIZE, GEMM_BLOCK_SIZE, beta, &C[i * ldc + j], ldc);

      int k = 0;
      for (; k < nk - GEMM_BLOCK_SIZE + 1; k += GEMM_BLOCK_SIZE) {
        fast_mini_mm(
	  			GEMM_BLOCK_SIZE,
          alpha, &A[i * lda + k], lda,
          &B[k * ldb + j], ldb,
          &C[i * ldc + j], ldc
        );
      }

      fast_mini_mm(
        nk - k,
        alpha, &A[i * lda + k], lda,
        &B[k * ldb + j], ldb,
        &C[i * ldc + j], ldc
      );
    }
  }

  #pragma omp for
  for (int i = 0; i < GEMM_BLOCK_SIZE * (ni / GEMM_BLOCK_SIZE); i += GEMM_BLOCK_SIZE) {
		int j = GEMM_BLOCK_SIZE * (nj / GEMM_BLOCK_SIZE);

		block_mul(GEMM_BLOCK_SIZE, nj - j, beta, &C[i * ldc + j], ldc);

		int k = 0;
		for (; k < nk - GEMM_BLOCK_SIZE + 1; k += GEMM_BLOCK_SIZE) {
			mini_mm(
				GEMM_BLOCK_SIZE, nj - j, GEMM_BLOCK_SIZE,
				alpha, &A[i * lda + k], lda,
				&B[k * ldb + j], ldb,
				&C[i * ldc + j], ldc
			);
		}

		mini_mm(
			GEMM_BLOCK_SIZE, nj - j, nk - k,
			alpha, &A[i * lda + k], lda,
			&B[k * ldb + j], ldb,
			&C[i * ldc + j], ldc
		);
  }

  #pragma omp for
  for (int j = 0; j < GEMM_BLOCK_SIZE * (nj / GEMM_BLOCK_SIZE); j += GEMM_BLOCK_SIZE) {
		int i = GEMM_BLOCK_SIZE * (ni / GEMM_BLOCK_SIZE);

		block_mul(ni - i, GEMM_BLOCK_SIZE, beta, &C[i * ldc + j], ldc);

		int k = 0;
		for (; k < nk - GEMM_BLOCK_SIZE + 1; k += GEMM_BLOCK_SIZE) {
			mini_mm(
				ni - i, GEMM_BLOCK_SIZE, GEMM_BLOCK_SIZE,
				alpha, &A[i * lda + k], lda,
				&B[k * ldb + j], ldb,
				&C[i * ldc + j], ldc
			);
		}

		mini_mm(
			ni - i, GEMM_BLOCK_SIZE, nk - k,
			alpha, &A[i * lda + k], lda,
			&B[k * ldb + j], ldb,
			&C[i * ldc + j], ldc
		);
  }
	
	#pragma omp single
	{
		int i = GEMM_BLOCK_SIZE * (ni / GEMM_BLOCK_SIZE);
		int j = GEMM_BLOCK_SIZE * (nj / GEMM_BLOCK_SIZE);

		block_mul(ni - i, nj - j, beta, &C[i * ldc + j], ldc);

		int k = 0;
		for (; k < nk - GEMM_BLOCK_SIZE + 1; k += GEMM_BLOCK_SIZE) {
			mini_mm(
				ni - i, nj - j, GEMM_BLOCK_SIZE,
				alpha, &A[i * lda + k], lda,
				&B[k * ldb + j], ldb,
				&C[i * ldc + j], ldc
			);
		}

		mini_mm(
			ni - i, nj - j, nk - k,
			alpha, &A[i * lda + k], lda,
			&B[k * ldb + j], ldb,
			&C[i * ldc + j], ldc
		);
	}
}

#define LDA_MULTIPLE 57

void pad_matrix(int ni, int nj, double *A, int lda, double **new_A, int *new_lda) {
  *new_lda = LDA_MULTIPLE * (nj / LDA_MULTIPLE + 1);
  *new_A = (double *) malloc(ni * (*new_lda) * sizeof(double));

  if (!new_A) {
    printf("Ran out of memory :(\n");
    exit(-1);
  }

  for (int i = 0; i < ni; i++) {
    memcpy(&(*new_A)[i * (*new_lda)], &A[i * lda], lda * sizeof(double));
  }
}

void padded_mm(
    int ni, int nj, int nk,
    double alpha, double *A, int lda,
    double *B, int ldb,
    double beta, double *C, int ldc)
{
  double *padded_A;
  int padded_lda;

  double *padded_B;
  int padded_ldb;

  double *padded_C;
  int padded_ldc;

  #pragma omp single \
    copyprivate(padded_A, padded_lda, padded_B, padded_ldb, padded_C, padded_ldc)
  {
    pad_matrix(ni, nk, A, lda, &padded_A, &padded_lda);
    pad_matrix(nk, nj, B, ldb, &padded_B, &padded_ldb);
    pad_matrix(ni, nj, C, ldc, &padded_C, &padded_ldc);
  }

  #pragma omp barrier

  mm(ni, nj, nk,
      alpha, padded_A, padded_lda,
      padded_B, padded_ldb,
      beta, padded_C, padded_ldc);

  #pragma omp barrier

  #pragma omp master
  {
    for (int i = 0; i < ni; i++) {
      for (int j = 0; j < nj; j++) {
        C[i * ldc + j] = padded_C[i * padded_ldc + j];
      }
    }

    free(padded_A);
    free(padded_B);
    free(padded_C);
  }
}


void gemm(
    int ni, int nj, int nk,
    double alpha, double *A, int lda,
    double *B, int ldb,
    double beta, double *C, int ldc)
{
  if (ni == 0 || nj == 0 || nk == 0) return;

#ifndef PAD_MATRICES
    mm(ni, nj, nk,
        alpha, A, lda,
        B, ldb,
        beta, C, ldc);
#else
    padded_mm(ni, nj, nk,
        alpha, A, lda,
        B, ldb,
        beta, C, ldc);
#endif
}
