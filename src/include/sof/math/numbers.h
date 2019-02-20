/*
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Intel Corporation nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Seppo Ingalsuo <seppo.ingalsuo@linux.intel.com>
 *         Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Keyon Jie <yang.jie@linux.intel.com>
 */

#ifndef NUMBERS_H
#define NUMBERS_H

#include <stdint.h>

#define MIN(a, b) ({		\
	typeof(a) __a = (a);	\
	typeof(b) __b = (b);	\
	__a > __b ? __b : __a;	\
})
#define MAX(a, b) ({		\
	typeof(a) __a = (a);	\
	typeof(b) __b = (b);	\
	__a < __b ? __b : __a;	\
})

int gcd(int a, int b); /* Calculate greatest common divisor for a and b */

/* This is a divide function that returns ceil of the quotient.
 * E.g. ceil_divide(9, 3) returns 3, ceil_divide(10, 3) returns 4.
 */
static inline int ceil_divide(int a, int b)
{
	int c;

	c = a / b;

	/* First, we check whether the signs of the params are different.
	 * If they are, we already know the result is going to be negative and
	 * therefore, is going to be already rounded up (truncated).
	 *
	 * If the signs are the same, we check if there was any remainder in
	 * the division by multiplying the number back.
	 */
	if (!((a ^ b) & (1 << ((sizeof(int) * 8) - 1))) && c * b != a)
		c++;

	return c;
}

/* Find indices of equal values in a vector of integer values */
int find_equal_int16(int16_t idx[], int16_t vec[], int n, int vec_length,
	int max_results);

/* Return the smallest value found in a vector */
int16_t find_min_int16(int16_t vec[], int vec_length);

/* Return the largest absolute value found in a vector */
int32_t find_max_abs_int32(int32_t vec[], int vec_length);

/* Count the left shift amount to normalize a 32 bit signed integer value
 * without causing overflow. Input value 0 will result to 31.
 */
int norm_int32(int32_t val);

/* merges two 16-bit values into a single 32-bit value */
#define merge_16b16b(high, low) (((uint32_t)(high) << 16) | \
				 ((low) & 0xFFFF))

/* merges two 4-bit values into a single 8-bit value */
#define merge_4b4b(high, low) (((uint8_t)(high) << 4) | \
			       ((low) & 0xF))

#endif /* NUMBERS_H */
