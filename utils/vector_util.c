// Copyright (C) 2019 Alibaba Group Holding Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ===========================================================================

#include "vector_util.h"
#include <assert.h>
#include <string.h>

#ifdef __AVX__
#define USE_AVX
#endif

float fvec_L2sqr_ref (const float * x,
                     const float * y,
                     size_t d) {
    size_t i;
    float res;
    res = 0;
    for (i = 0; i < d; i++) {
        const float tmp = x[i] - y[i];
       res += tmp * tmp;
    }
    return res;
}

float fvec_inner_product_ref (const float * x,
                             const float * y,
                             size_t d) {
    size_t i;
    float res;
    res = 0;
    for (i = 0; i < d; i++)
       res += x[i] * y[i];
    return res;
}

float fvec_norm_L2sqr_ref (const float *x, size_t d) {
    size_t i;
    double res;
    res = 0;
    for (i = 0; i < d; i++)
       res += x[i] * x[i];
    return res;
}

void fvec_L2sqr_ny_ref (float * dis,
                    const float * x,
                    const float * y,
                    size_t d, size_t ny) {
    size_t i;
    for (i = 0; i < ny; i++) {
        dis[i] = fvec_L2sqr (x, y, d);
        y += d;
    }
}

#ifdef __SSE__

// reads 0 <= d < 4 floats as __m128
static inline __m128 masked_read (int d, const float *x) {
    __attribute__((__aligned__(16))) float buf[4];

    assert (0 <= d && d < 4);
    memset((void*)buf, 0, sizeof(buf));
    switch (d) {
      case 3:
        buf[2] = x[2];
      case 2:
        buf[1] = x[1];
      case 1:
        buf[0] = x[0];
    }
    return _mm_load_ps (buf);
    // cannot use AVX2 _mm_mask_set1_epi32
}

float fvec_norm_L2sqr (const float *  x,
                      size_t d) {
    __m128 mx;
    __m128 msum1 = _mm_setzero_ps();

    while (d >= 4) {
        mx = _mm_loadu_ps (x); x += 4;
        msum1 = _mm_add_ps (msum1, _mm_mul_ps (mx, mx));
        d -= 4;
    }

    mx = masked_read (d, x);
    msum1 = _mm_add_ps (msum1, _mm_mul_ps (mx, mx));

    msum1 = _mm_hadd_ps (msum1, msum1);
    msum1 = _mm_hadd_ps (msum1, msum1);
    return  _mm_cvtss_f32 (msum1);
}


#endif

#ifdef USE_AVX

// reads 0 <= d < 8 floats as __m256
static inline __m256 masked_read_8 (int d, const float *x) {
    assert (0 <= d && d < 8);
    if (d < 4) {
        __m256 res = _mm256_setzero_ps ();
        res = _mm256_insertf128_ps (res, masked_read (d, x), 0);
        return res;
    } else {
        __m256 res = _mm256_setzero_ps ();
        res = _mm256_insertf128_ps (res, _mm_loadu_ps (x), 0);
        res = _mm256_insertf128_ps (res, masked_read (d - 4, x + 4), 1);
        return res;
    }
}

float fvec_inner_product (const float * x,
                          const float * y,
                          size_t d) {
    __m256 msum1;
    msum1 = _mm256_setzero_ps();

    while (d >= 8) {
        __m256 mx = _mm256_loadu_ps (x); x += 8;
        __m256 my = _mm256_loadu_ps (y); y += 8;
        msum1 = _mm256_add_ps (msum1, _mm256_mul_ps (mx, my));
        d -= 8;
    }

    __m128 msum2;
    msum2 = _mm256_extractf128_ps(msum1, 1);
    msum2 +=       _mm256_extractf128_ps(msum1, 0);

    if (d >= 4) {
        __m128 mx = _mm_loadu_ps (x); x += 4;
        __m128 my = _mm_loadu_ps (y); y += 4;
        msum2 = _mm_add_ps (msum2, _mm_mul_ps (mx, my));
        d -= 4;
    }

    if (d > 0) {
        __m128 mx = masked_read (d, x);
        __m128 my = masked_read (d, y);
        msum2 = _mm_add_ps (msum2, _mm_mul_ps (mx, my));
    }

    msum2 = _mm_hadd_ps (msum2, msum2);
    msum2 = _mm_hadd_ps (msum2, msum2);
    return  _mm_cvtss_f32 (msum2);
}

float fvec_L2sqr (const float * x,
                 const float * y,
                 size_t d) {
    __m256 msum1;
    msum1 = _mm256_setzero_ps();

    while (d >= 8) {
        __m256 mx = _mm256_loadu_ps (x); x += 8;
        __m256 my = _mm256_loadu_ps (y); y += 8;
        const __m256 a_m_b1 = mx - my;
        msum1 += a_m_b1 * a_m_b1;
        d -= 8;
    }

    __m128 msum2;
    msum2 = _mm256_extractf128_ps(msum1, 1);
    msum2 +=       _mm256_extractf128_ps(msum1, 0);

    if (d >= 4) {
        __m128 mx = _mm_loadu_ps (x); x += 4;
        __m128 my = _mm_loadu_ps (y); y += 4;
        const __m128 a_m_b1 = mx - my;
        msum2 += a_m_b1 * a_m_b1;
        d -= 4;
    }

    if (d > 0) {
        __m128 mx = masked_read (d, x);
        __m128 my = masked_read (d, y);
        __m128 a_m_b1 = mx - my;
        msum2 += a_m_b1 * a_m_b1;
    }

    msum2 = _mm_hadd_ps (msum2, msum2);
    msum2 = _mm_hadd_ps (msum2, msum2);
    return  _mm_cvtss_f32 (msum2);
}

#elif defined(__SSE__)

//  SSE-implementation of L2 distance
float fvec_L2sqr (const float * x,
                 const float * y,
                 size_t d) {
    __m128 msum1;
    msum1 = _mm_setzero_ps();

    while (d >= 4) {
        __m128 mx, my, a_m_b1;
        mx = _mm_loadu_ps (x); x += 4;
        my = _mm_loadu_ps (y); y += 4;
        a_m_b1 = mx - my;
        msum1 += a_m_b1 * a_m_b1;
        d -= 4;
    }

    if (d > 0) {
        // add the last 1, 2 or 3 values
        __m128 mx, my, a_m_b1;
        mx = masked_read (d, x);
        my = masked_read (d, y);
        a_m_b1 = mx - my;
        msum1 += a_m_b1 * a_m_b1;
    }

    msum1 = _mm_hadd_ps (msum1, msum1);
    msum1 = _mm_hadd_ps (msum1, msum1);
    return  _mm_cvtss_f32 (msum1);
}


float fvec_inner_product (const float * x,
                         const float * y,
                         size_t d) {
    __m128 mx, my;
    __m128 msum1;
    __m128 prod;

    msum1 = _mm_setzero_ps();
    while (d >= 4) {
        mx = _mm_loadu_ps (x); x += 4;
        my = _mm_loadu_ps (y); y += 4;
        msum1 = _mm_add_ps (msum1, _mm_mul_ps (mx, my));
        d -= 4;
    }

    // add the last 1, 2, or 3 values
    mx = masked_read (d, x);
    my = masked_read (d, y);
    prod = _mm_mul_ps (mx, my);

    msum1 = _mm_add_ps (msum1, prod);

    msum1 = _mm_hadd_ps (msum1, msum1);
    msum1 = _mm_hadd_ps (msum1, msum1);
    return  _mm_cvtss_f32 (msum1);
}

#else
// scalar implementation

float fvec_L2sqr (const float * x,
                  const float * y,
                  size_t d) {
    return fvec_L2sqr_ref (x, y, d);
}

float fvec_inner_product (const float * x,
                             const float * y,
                             size_t d) {
    return fvec_inner_product_ref (x, y, d);
}

float fvec_norm_L2sqr (const float *x, size_t d) {
    return fvec_norm_L2sqr_ref (x, d);
}

void fvec_L2sqr_ny (float * dis, const float * x,
                        const float * y, size_t d, size_t ny) {
    fvec_L2sqr_ny_ref (dis, x, y, d, ny);
}

#endif
