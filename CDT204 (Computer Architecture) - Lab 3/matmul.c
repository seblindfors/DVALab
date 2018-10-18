/*
	CDT204 - Computer Architecture (2017)
	Lab Assignment 3: matrix multiplcation
	Author: Sebastian Lindfors
*/

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <immintrin.h> // AVX instruction set

/*
	This program performs experimental evaluations of different methods used to multiply two large (10^6 cells) matrices.
	The purpose of the program is to find the best method to perform these calculations, and scientifcally discuss the results.

	This program also contains code that measures the multiplication time,
	and it can also verify the correctness of the output of each multiplication by using the function:
		compare_matrices.

	The two multiplied matrices are N x N square matrices.
	The value of N is sufficient to create a large matrix that doesn't fit in the cache memory of any modern computer.
	Setting N to a large value guarantees that the whole matrix is not loaded in the cache memory.
	Thus, many loads and stores are done from/to the main memory to/from the cache memory.
	If N = 1000, then the size of every matrix is 1000 * 1000 = 10^6 integers or about 4 MB (32 bits/4 bytes per integer).

	Results are denoted in two separate timers:
	first result has compiler optimization disabled, second result is set to maximize speed.
*/


#define N 1000

// Compare the matrices, and return 1 if they are equal, 0 otherwise
int compare_matrices(int mat1[N][N], int mat2[N][N]) {
	int i, j;
	for (i = 0; i < N; ++i) for (j = 0; j < N; ++j)
		if (mat1[i][j] != mat2[i][j])
			return 0;
	return 1;
}

// Algorithm 1: row-major order (4.285620 s, 0.502490 s)
void version1(int mat1[N][N], int mat2[N][N], int result[N][N]) {
	int i, j, k;
	for (i = 0; i < N; ++i) {
		for (j = 0; j < N; ++j) {
			// Compute the value for result[i][j]. Initialize it to 0, then
			// run through row i of mat1 and column j of mat2 in parallel and
			// multiply their elements pairwise and sum up the products.

			for (k = 0; k < N; ++k)
				result[i][j] += mat1[i][k] * mat2[k][j];
		}
	}
}

// Algorithm 2: column-major order (3.236650 s, 0.502120 s)
void version2(int mat1[N][N], int mat2[N][N], int result[N][N]) {
	int i, j, k;
	for (j = 0; j < N; ++j)
		for (i = 0; i < N; ++i)
			for (k = 0; k < N; ++k)
				result[i][j] += mat1[i][k] * mat2[k][j];
}

// Algorithm 3: cell priority (2.256460 s, 0.502110 s)
void version3(int mat1[N][N], int mat2[N][N], int result[N][N]) {
	int i, j, k;
	for (i = 0; i < N; ++i)
		for (j = 0; j < N; ++j)
			for (k = 0; k < N; ++k)
				result[i][k] += mat1[i][j] * mat2[j][k];
}

// Algorithm 4: I'm a genius (1.919360 s, 0.332810)
/*
	This version uses a local cell to do the multiplications with mat2.
	This results in an algorithm where all 3 matrices are iterated on row first.
	This should be faster if arrays are stored in row-major order.
*/
void version4(int mat1[N][N], int mat2[N][N], int result[N][N]) {
	int i, j, k, c; // c for cell
	for (i = 0; i < N; ++i)
		for (j = 0; j < N; ++j) {
			c = mat1[i][j];
			for (k = 0; k < N; ++k)
				result[i][k] += c * mat2[j][k];
		}
}

// Algorithm 5: AVX instructions (0.420020 s, 0.139220 s)
#define VECTORIZE 8 // AVX can process 8 values in parallell.
/*
	Why I chose AVX over SSE:
	I chose AVX because it allows me to use instructions expecting 256 bits, which basic SSE
	doesn't. 256/32 = 8, so it will be able to do multiplication and addition on 8 indices in mat2/result at a
	time. If the matrices were smaller, or equal to 4x4 elements, I would use SSE with 128 bit instructions
	because there's no benefit in using AVX if the vector isn't full. Maybe you could potentially load two rows
	in that case and AVX would still be useful, but my general answer is that the matrices are large, therefore AVX.

	Which algorithm I chose to convert:
	I converted my own algorithm, because it's the closest to row first you can get, AND it had the fastest speed.
	On my desktop computer, the combination of my own algorithm and AVX instructions yielded results that were 
	close to 5 times faster than the original algorithm, on which the others are based.
*/
void version5(int mat1[N][N], int mat2[N][N], int result[N][N]) {
	int i, j, k;
	__m256i vA, vB, vR;

	for (i = 0; i < N; ++i)
		for (k = 0; k < N; ++k) {
			// (1) vA: local cell to use for multiplications (only 1 int), row first, like version4.
			// (2) for loop: vectorize 8 values (256/32 = 8)
			vA = _mm256_set1_epi32(mat1[i][k]);
			for (j = 0; j < N; j += VECTORIZE) {
				// (1) load 8 ints from mat2, row first
				// (2) load 8 ints from result, row first
				// (3) (8 consecutive ints of result) += (cell * (8 consecutive ints from mat2))
				// (4) store the 8 updated ints to result, from index j+0 to j+7.
				vB = _mm256_loadu_si256(&mat2[k][j]);
				vR = _mm256_loadu_si256(&result[i][j]);
				vR = _mm256_add_epi32(vR, _mm256_mullo_epi32(vA, vB));
				_mm256_storeu_si256(&result[i][j], vR);
			}
		}
}

// The matrices. mat_ref is used for reference. If the multiplication is done correctly,
// mat_r should equal mat_ref.
int mat_a[N][N], mat_b[N][N], mat_r[N][N], mat_ref[N][N];

// Call this before performing the operation (and do *not* include the time to
// return from this function in your measurements). It fills mat_a and mat_b with
// random integer values in the range [0..9].
void init_matrices() {
	int i, j;
	srand(0xBADB0LL);
	for (i = 0; i < N; ++i) for (j = 0; j < N; ++j) {
		mat_a[i][j] = rand() % 10;
		mat_b[i][j] = rand() % 10;
		mat_r[i][j] = 0;
		mat_ref[i][j] = 0;
	}
}

void runtest(void * f(int mat1[N][N], int mat2[N][N], int result[N][N]),
	int version, double clocks[5], int mat1[N][N], int mat2[N][N], int result[N][N]) {
	
	// Initialize the matrices
	init_matrices();

	clock_t t0, t1;
//	printf("Started matrix multiplication using version %d.\n", version);
	// Take the time
	t0 = clock();

	// Run the selected algorithm
	f(mat1, mat2, mat_r);

	// Take the time again
	t1 = clock();

//	printf("Finished in %lf seconds.\n", (double)(t1 - t0) / CLOCKS_PER_SEC);
	clocks[version - 1] += ((double)(t1 - t0) / CLOCKS_PER_SEC);

	/* Check that mat_r is correct. For this the reference matrix mat_ref is computed
	// using the basic() implementation, and then mat_r is compared to mat_ref. */
	printf("Checking resulting matrix.\n");
	version1(mat_a, mat_b, mat_ref);
	if (!compare_matrices(mat_r, mat_ref))
		printf("Error: mat_r does not match the reference matrix!\n");
	else
		printf("Correct!\n");
}

int main(void) {
#ifdef _MSC_VER
	system("pause"); // Put this here to allow the program to load up without skewing results.
#endif
	// Clocks to calculate average speeds.
	double clocks[5] = {0,0,0,0,0};
	int iterations = 1, i;

	// Run the algorithms
	for (i = 0; i < iterations; ++i) {
		runtest(version1, 1, clocks, mat_a, mat_b, mat_r);
		runtest(version2, 2, clocks, mat_a, mat_b, mat_r);
		runtest(version3, 3, clocks, mat_a, mat_b, mat_r);
		runtest(version4, 4, clocks, mat_a, mat_b, mat_r);
		runtest(version5, 5, clocks, mat_a, mat_b, mat_r);
	}

	printf("Testing complete, %d iterations.\n", iterations);
	for (i = 0; i < 5; ++i)
		printf("[%d] %lf seconds.\n", i+1, clocks[i] / iterations);
	
	// If using Visual Studio, do not close the console window immediately
#ifdef _MSC_VER
	system("pause");
#endif

	return 0;
}

/*  Average results (seconds) for 100 iterations:
    raw       optimized  algorithm  
    4.285620  0.502490   version1
    3.236650  0.502120   version2
    2.256460  0.502110   version3
    1.919360  0.332810   version4
    0.420020  0.139220   version5
*/
