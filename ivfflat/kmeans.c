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

#include "postgres.h"

#include <math.h>
#include "fmgr.h"
#include "miscadmin.h"
#include "windowapi.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/vector_util.h"
#include "kmeans.h"

#define SIZEOF_V(dim) (sizeof(float4) * dim)

#define KMEANS_CHECK_V(v, dim, isnull) do{ \
  if ((isnull) || \
      ARR_NDIM(v) != 1 || \
      ARR_DIMS(v)[0] != (dim) || \
      ARR_HASNULL(v)) \
  ereport(ERROR, \
      (errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
       errmsg("input vector not valid"), \
       errhint("input vectors must be 1d without NULL element, with the same length"))); \
} while(0)

typedef struct {
  bool	isdone;
  bool	isnull;
  int		result[1];
  // variable length
} kmeans_context;

// update classification (assignment) by calculated mean vectors.
  static void
update_r(myvector inputs, int dim, int N, int k, myvector mean, int *r) {
  int			i, klass;

  for (i = 0; i < N; i++) {
    float4		dist;
    float4		curr_dist;
    int			curr_klass;

    curr_dist = fvec_L2sqr(&inputs[i * dim], &mean[0], dim);
    curr_klass = 0;
    // Search the nearst mean point.
    for (klass = 1; klass < k; klass++) {
      dist = fvec_L2sqr(&inputs[i * dim], &mean[klass * dim], dim);
      if (dist < curr_dist)
      {
        curr_dist = dist;
        curr_klass = klass;
      }
    }
#ifdef KMEANS_DEBUG
    elog(LOG, "r[%d] = %d -> %d", i, r[i], curr_klass);
#endif
    r[i] = curr_klass;
  }
}

// update mean vectors by all vectors classified in each class.
static void
update_mean(myvector inputs, int dim, int N, int k, myvector mean, int *r) {
  myvector	mean_sum = (myvector) palloc0(SIZEOF_V(dim) * k);
  int		   *mean_count = (int *) palloc0(sizeof(int) * k);
  int			i, a, klass;

  for (i = 0; i < N; i++) {
    klass = r[i];

    for (a = 0; a < dim; a++) {
      mean_sum[klass * dim + a] += inputs[i * dim + a];
    }
    mean_count[klass]++;
  }

  for (klass = 0; klass < k; klass++) {
    for (a = 0; a < dim; a++) {
      if (mean_count[klass] > 0)
        mean[klass * dim + a] = mean_sum[klass * dim + a] / mean_count[klass];
    }
  }
  pfree(mean_sum);
  pfree(mean_count);
}

// Evaluation function. kmeans tries to minimize value of this function.
static float4
J(myvector inputs, int dim, int N, int k, myvector mean, int *r) {
  int		i;
  float4	sum = 0.0;

  for (i = 0; i < N; i++)
  {
    sum += fvec_L2sqr(&inputs[i * dim], &mean[r[i] * dim], dim);
  }
  return sum;
}

// initialize_mean
// determine initial mean vectors (centroids) when they aren't specified.
// The way to initialize is hard; it decides the result quality.
static void
initialize_mean(myvector inputs, int dim, int N, int k, myvector mean, int *r) {
  myvector	minvec = (myvector) palloc0(SIZEOF_V(dim));
  myvector	maxvec = (myvector) palloc0(SIZEOF_V(dim));
  myvector	midvec = (myvector) palloc0(SIZEOF_V(dim));
  int			i, j, a, klass, sidx;
  int		   *seen;

  // First, scan all input vectors and find min/max values
  // for each dimension (i.e. take largest space)
  for (i = 0; i < N; i++) {
    if (i == 0) {
      for (a = 0; a < dim; a++) {
        minvec[a] = maxvec[a] = inputs[i * dim + a];
      }
    } else {
      for (a = 0; a < dim; a++) {
        if (minvec[a] > inputs[i * dim + a]) {
          minvec[a] = inputs[i * dim + a];
        } if (maxvec[a] < inputs[i * dim + a]) {
          maxvec[a] = inputs[i * dim + a];
        }
      }
    }
  }

  // array to store vectors which were chosen until
  // the iteration. We try to avoid duplicated assignment.
  seen = (int *) palloc0(sizeof(int) * N);
  sidx = 0;
  for (klass = 0; klass < k; klass++) {
    float4	curr_dist, dist;
    int		curr_idx = 0;

    // split vector space linearly. Then find nearest point
    // and take it as a centroid.
    for (a = 0; a < dim; a++) {
      midvec[a] = (maxvec[a] - minvec[a]) *
        ((float4) (klass + 1) / (float4) (k + 1)) + minvec[a];
    }
    for (i = 0; i < N; i++) {
      bool	found = false;

      // If this element is taken by another centroid,
      // then take another for this loop as far as possible.
      for (j = 0; j < sidx; j++) {
        if (seen[j] == i)
          found = true;
      }
      if (found)
        continue;
      dist = fvec_L2sqr(midvec, &inputs[i * dim], dim);
      if (curr_idx == 0 || dist < curr_dist) {
        // the input vector seems nearer
        curr_dist = dist;
        curr_idx = i;
      }
    }
    memcpy(&mean[klass * dim], &inputs[curr_idx * dim], SIZEOF_V(dim));
    seen[sidx++] = curr_idx;
  }
  pfree(minvec);
  pfree(maxvec);
  pfree(midvec);
}

#ifdef KMEANS_DEBUG
static void
kmeans_debug(myvector mean, int dim, int k) {
  StringInfoData	buf;
  int			klass, a;

  for (klass = 0; klass < k; klass++) {
    initStringInfo(&buf);
    for (a = 0; a < dim; a++) {
      appendStringInfo(&buf, "%lf", mean[klass * dim + a]);
      if (a != dim - 1)
        appendStringInfoString(&buf, ", ");
    }
    elog(LOG, "%d: %s", klass, buf.data);
  }
}
#else
#define kmeans_debug(mean, dim, k)
#endif // KMEANS_DEBUG

// calc_kmeans
// The main body of kmean calculation.
// inputs	: all of elements to be clustered by kmeans
// dim		: dimension of element, namely myvector has dim floats in each.
// N		: the number of input vectors
// k		: the number to cluster
// mean		: initial mean vectors (centroids)
// r		: (out) an array to put answer cluster ids
static int *
calc_kmeans(myvector inputs, int dim, int N, int k, myvector mean, int *r) {
  float4	target, new_target;

  
  // initialize purpose value. At this time, r doesn't mean anything
  // but it's ok; just fill target by some value.
  target = J(inputs, dim, N, k, mean, r);
  for (;;) {

    // it's good to check here, for avoid infinite loop
    CHECK_FOR_INTERRUPTS();
    update_r(inputs, dim, N, k, mean, r);
    update_mean(inputs, dim, N, k, mean, r);
    new_target = J(inputs, dim, N, k, mean, r);
    kmeans_debug(mean, dim, k);
    // if all the classification stay, diff must be 0.0,
    // which means we can go out!
    if (target == new_target)
      break;
    target = new_target;
  }

  return  r;
}

myvector
kmeans_impl(int dim, int k, int N, myvector inputs,
    bool initial_mean_supplied, myvector initial_mean, int *r) {
  myvector	mean;

   // initial mean vectors. need improve how to define them.
  mean = (myvector) palloc(SIZEOF_V(dim) * k);
  if (initial_mean_supplied) {
    memcpy(mean, initial_mean, SIZEOF_V(dim) * k);
  } else {
    initialize_mean(inputs, dim, N, k, mean, r);
    kmeans_debug(mean, dim, k);
  }
  // run it!
  calc_kmeans(inputs, dim, N, k, mean, r);
  return mean;
}

