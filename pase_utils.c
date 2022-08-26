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

#include <float.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>

#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "common/base64.h"
#include "storage/indexfsm.h"
#include "access/stratnum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "pase.h"
#include "type/pase_data.h"
#include "utils/vector_util.h"

PG_FUNCTION_INFO_V1(g_pase_distance);

Datum
g_pase_distance(PG_FUNCTION_ARGS) {
  ArrayType *b = PG_GETARG_ARRAYTYPE_P(0);
  PASE *a = PG_GETARG_PASE(1);
  float4 distance = 0.0;
  float4 *data;
  int i;
  int dim1 = PASE_DIM(a);
  int dim2 = PASE_ARRNELEMS(b);

  if (dim1 != dim2) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
      errmsg("leftarg[%d] and rightarg[%d] dim is not equal.", dim1, dim2)));
  }

  data = PASE_ARRPTR(b);
  if (PASE_DS(a) == PaseKNNDistanceIP) {
    for (i = 0; i < dim1; ++i) {
      distance += a->x[i] * data[i];
    }
  } else {
    distance = fvec_L2sqr(a->x, data, dim1);
  }
  PG_RETURN_FLOAT4(distance);
}

PG_FUNCTION_INFO_V1(g_pase_distance_3);

Datum
g_pase_distance_3(PG_FUNCTION_ARGS) {
  PASE *a = PG_GETARG_PASE(1);
  text *b = PG_GETARG_TEXT_P(0);
  double distance = 0.0;
  int i;
  int dim1 = PASE_DIM(a);
  char dest[1024 * 1024];
  char *rawData = VARDATA_ANY(b);
  int len = VARSIZE_ANY_EXHDR(b);
  int dim2 = pg_b64_decode(rawData, len, dest) / sizeof(float4);

  if (dim1 != dim2) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
      errmsg("leftarg[%d] and rightarg[%d] dim is not equal.", dim1, dim2)));
  }

  if (PASE_DS(a) == PaseKNNDistanceIP) {
    for (i = 0; i < dim1; ++i) {
      distance += a->x[i] * ((float4 *)dest)[i];
    }
  } else {
      distance = fvec_L2sqr(a->x, (float4 *)dest, dim1);
  }
  PG_RETURN_FLOAT4(distance);
}

Buffer
PaseNewBuffer(Relation index) {
  Buffer buffer;
  bool needLock;

  for (;;) {
    BlockNumber blkno = GetFreeIndexPage(index);
    if (blkno == InvalidBlockNumber) {
      break;
    }
    buffer = ReadBuffer(index, blkno);
    if (ConditionalLockBuffer(buffer)) {
      Page page = BufferGetPage(buffer);
      if (PageIsNew(page)) {
        return buffer;  // OK to use, if never initialized
      }
      LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    }
    ReleaseBuffer(buffer);
  }
  // Must extend the file
  needLock = !RELATION_IS_LOCAL(index);
  if (needLock) {
    LockRelationForExtension(index, ExclusiveLock);
  }
  buffer = ReadBuffer(index, P_NEW);
  LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
  if (needLock) {
    UnlockRelationForExtension(index, ExclusiveLock);
  }
  return buffer;
}

void
PaseFlushBuff(Relation index, Buffer buffer) {
  // NOTICE: it must be lock by caller
  GenericXLogState *state;

  LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
  LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
  state = GenericXLogStart(index);
  GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
  GenericXLogFinish(state);
  // NOTICE: it must be unlock and release by caller
}

double
elapsed () {
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return  tv.tv_sec + tv.tv_usec * 1e-6;
}
