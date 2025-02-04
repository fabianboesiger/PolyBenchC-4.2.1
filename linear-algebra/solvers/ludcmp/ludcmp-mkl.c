/**
 * This version is stamped on May 10, 2016
 *
 * Contact:
 *   Louis-Noel Pouchet <pouchet.ohio-state.edu>
 *   Tomofumi Yuki <tomofumi.yuki.fr>
 *
 * Web address: http://polybench.sourceforge.net
 */
/* ludcmp.c: this file is part of PolyBench/C */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

/* Include polybench common header. */
#include <polybench.h>

/* Include benchmark-specific header. */
#include "ludcmp.h"

// See: https://www.intel.com/content/www/us/en/develop/documentation/onemkl-windows-developer-guide/top/language-specific-usage-options/mixed-language-programming-with-onemkl/call-blas-funcs-return-complex-values-in-c-code.html#call-blas-funcs-return-complex-values-in-c-code_XREF_EXAMPLE_6_3_USING_CBLAS
#include "mkl.h"
#include <assert.h>

/* Array initialization. */
static
void init_array (int n,
		 DATA_TYPE POLYBENCH_2D(A,NN,NN,n,n),
		 DATA_TYPE POLYBENCH_1D(b,NN,n),
		 DATA_TYPE POLYBENCH_1D(x,NN,n),
		 DATA_TYPE POLYBENCH_1D(y,NN,n))
{
  int i, j;
  DATA_TYPE fn = (DATA_TYPE)n;

  for (i = 0; i < n; i++)
    {
      x[i] = 0;
      y[i] = 0;
      b[i] = (i+1)/fn/2.0 + 4;
    }

  for (i = 0; i < n; i++)
    {
      for (j = 0; j <= i; j++)
	A[i][j] = (DATA_TYPE)(-j % n) / n + 1;
      for (j = i+1; j < n; j++) {
	A[i][j] = 0;
      }
      A[i][i] = 1;
    }

  /* Make the matrix positive semi-definite. */
  /* not necessary for LU, but using same code as cholesky */
  /*
  int r,s,t;
  POLYBENCH_2D_ARRAY_DECL(B, DATA_TYPE, NN, NN, n, n);
  for (r = 0; r < n; ++r)
    for (s = 0; s < n; ++s)
      (POLYBENCH_ARRAY(B))[r][s] = 0;
  for (t = 0; t < n; ++t)
    for (r = 0; r < n; ++r)
      for (s = 0; s < n; ++s)
	(POLYBENCH_ARRAY(B))[r][s] += A[r][t] * A[s][t];
    for (r = 0; r < n; ++r)
      for (s = 0; s < n; ++s)
	A[r][s] = (POLYBENCH_ARRAY(B))[r][s];
  POLYBENCH_FREE_ARRAY(B);
  */

}


/* DCE code. Must scan the entire live-out data.
   Can be used also to check the correctness of the output. */
static
void print_array(int n,
		 DATA_TYPE POLYBENCH_1D(x,NN,n))

{
  int i;

  POLYBENCH_DUMP_START;
  POLYBENCH_DUMP_BEGIN("x");
  for (i = 0; i < n; i++) {
    if (i % 20 == 0) fprintf (POLYBENCH_DUMP_TARGET, "\n");
    fprintf (POLYBENCH_DUMP_TARGET, DATA_PRINTF_MODIFIER, x[i]);
  }
  POLYBENCH_DUMP_END("x");
  POLYBENCH_DUMP_FINISH;
}


/* Main computational kernel. The whole function will be timed,
   including the call and return. */
static
void kernel_ludcmp(int n,
		   DATA_TYPE POLYBENCH_2D(A,NN,NN,n,n),
		   DATA_TYPE POLYBENCH_1D(b,NN,n),
		   DATA_TYPE POLYBENCH_1D(x,NN,n),
		   DATA_TYPE POLYBENCH_1D(y,NN,n))
{
  int ipiv[n];

  #pragma scop
  LAPACKE_dgetrf(CblasRowMajor, n, n, (double *) A, n, ipiv);
  LAPACKE_dgetrs(CblasRowMajor, 'N', n, 1, (const double *) A, n, ipiv, b, 1); 	
  memcpy(x, b, n * sizeof(double));
  #pragma endscop
}


int main(int argc, char** argv)
{
  /* Retrieve problem size. */
  int n = NN;

  /* Variable declaration/allocation. */
  POLYBENCH_2D_ARRAY_DECL(A, DATA_TYPE, NN, NN, n, n);
  POLYBENCH_1D_ARRAY_DECL(b, DATA_TYPE, NN, n);
  POLYBENCH_1D_ARRAY_DECL(x, DATA_TYPE, NN, n);
  POLYBENCH_1D_ARRAY_DECL(y, DATA_TYPE, NN, n);

  printf(openblas_get_config());

  /* Initialize array(s). */
  init_array (n,
	      POLYBENCH_ARRAY(A),
	      POLYBENCH_ARRAY(b),
	      POLYBENCH_ARRAY(x),
	      POLYBENCH_ARRAY(y));

  /* Start timer. */
  polybench_start_instruments;

  /* Run kernel. */
  kernel_ludcmp (n,
		 POLYBENCH_ARRAY(A),
		 POLYBENCH_ARRAY(b),
		 POLYBENCH_ARRAY(x),
		 POLYBENCH_ARRAY(y));

  /* Stop and print timer. */
  polybench_stop_instruments;
  polybench_print_instruments;

  /* Prevent dead-code elimination. All live-out data must be printed
     by the function call in argument. */
  polybench_prevent_dce(print_array(n, POLYBENCH_ARRAY(x)));

  /* Be clean. */
  POLYBENCH_FREE_ARRAY(A);
  POLYBENCH_FREE_ARRAY(b);
  POLYBENCH_FREE_ARRAY(x);
  POLYBENCH_FREE_ARRAY(y);

  return 0;
}
