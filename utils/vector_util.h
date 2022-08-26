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
//
// vector utilities
//

#ifndef PASE_UTILS_VECTOR_UTIL_H_
#define PASE_UTILS_VECTOR_UTIL_H_

#ifdef __SSE__
#include <immintrin.h>
#endif

float fvec_L2sqr_ref(const float * x, const float * y, size_t d);
float fvec_inner_product_ref(const float * x, const float * y, size_t d);
float fvec_norm_L2sqr_ref(const float *x, size_t d);
void fvec_L2sqr_ny_ref(float * dis, const float * x, const float * y, size_t d, size_t ny);
float fvec_norm_L2sqr(const float *  x, size_t d);
float fvec_inner_product(const float * x, const float * y, size_t d);
float fvec_L2sqr (const float * x, const float * y, size_t d);

#endif  // PASE_UTILS_VECTOR_UTIL_H_
