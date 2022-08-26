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

#include "common/base64.h"
#include "access/stratnum.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/varlena.h"

#include "pase.h"
#include "type/pase_data.h"

// io fuctions
PG_FUNCTION_INFO_V1(pase_in);
PG_FUNCTION_INFO_V1(pase_out);
PG_FUNCTION_INFO_V1(pase_send);
PG_FUNCTION_INFO_V1(pase_recv);

Datum
pase_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  char dest[1024*1024];
  const char *rawVector;
  List *paseList;
  List *vectors;
  ListCell *cell;
  int extra, ds, dim, size, i;
  int loop = 0;
  PASE *result;

  extra = 0;
  ds = 0;
  // ignore split erro
  if (!SplitGUCList(pstrdup(str), ':', &paseList)
    || paseList->length > 3) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
      errmsg("pase data is invalid")));
  }
  foreach(cell, paseList) {
    const char *str = (const char *) lfirst(cell);
    if (0 == loop) {
      rawVector = str;
      if (!SplitGUCList(pstrdup(str), ',', &vectors)
        || vectors->length > PASE_MAX_DIM) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
          errmsg("pase over limit dim. (or pase format is invalid).")));
      }
    } else if (1 == loop) {
      extra = atoi(str);  // check when it is use
    } else if (2 == loop) {
      ds = atoi(str);  // check when it is use
    }
    ++loop;
  }

  // base64 decode
  if (vectors->length <= 1) {
    memset(dest, 0, sizeof(dest));
    if (paseList->length > 1) {
        rawVector = NULL;
        dim = pg_b64_decode(rawVector, strlen(rawVector), dest) / sizeof(float4);
    } else {
        dim = pg_b64_decode(str, strlen(str), dest) / sizeof(float4);
    }
  } else {
    dim = vectors->length;
  }

  size = PASE_SIZE(dim);
  result = (PASE *)palloc0(size);
  SET_VARSIZE(result, size);
  SET_PASE(result, dim, extra, ds);

  if (vectors->length <= 1) {
    for (i = 0; i < dim; ++i) {
        result->x[i] = ((float4*)dest)[i];
    }
  } else {
      i = 0;
      foreach(cell, vectors) {
          const char *str = (const char *) lfirst(cell);
          result->x[i++] = strtof(str, NULL);
      }
  }
  list_free(paseList);
  list_free(vectors);

  PG_RETURN_POINTER(result);
}

Datum
pase_out(PG_FUNCTION_ARGS) {
  PASE *pase;
  StringInfoData buf;
  int dim, i;

  pase = PG_GETARG_PASE(0);
  dim = PASE_DIM(pase);

  initStringInfo(&buf);
  for (i = 0; i < dim; i++) {
    if (i > 0) {
      appendStringInfoString(&buf, ",");
    }
    appendStringInfoString(&buf, float8out_internal(pase->x[i]));
  }

  appendStringInfoString(&buf, ":");
  appendStringInfoString(&buf, ":");

  PG_FREE_IF_COPY(pase, 0);
  PG_RETURN_CSTRING(buf.data);
}

Datum
pase_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

  int i;
  int dim = pq_getmsgint(buf, 4);
  int extra = pq_getmsgint(buf, 4);
  int ds = pq_getmsgint(buf, 4);
  int size = PASE_SIZE(dim);

  PASE *result;
  result = (PASE *)palloc(size);
  SET_VARSIZE(result, size);
  SET_PASE(result, dim, extra, ds);

  for (i = 0; i < dim; ++i) {
    result->x[i] = pq_getmsgfloat4(buf);
  }
  PG_RETURN_POINTER(result);
}

Datum
pase_send(PG_FUNCTION_ARGS) {
  PASE *pase = (PASE *)PG_GETARG_POINTER(0);
  StringInfoData buf;
  int i;
  int dim = PASE_DIM(pase);
  pq_begintypsend(&buf);
  pq_sendint(&buf, dim, 4);
  pq_sendint(&buf, PASE_EXTRA(pase), 4);
  pq_sendint(&buf, PASE_DS(pase), 4);
  for (i = 0; i < dim; ++i) {
    pq_sendfloat4(&buf, pase->x[i]);
  }
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(pase_f4_i_i);

Datum
pase_f4_i_i(PG_FUNCTION_ARGS) {
  ArrayType *vector = PG_GETARG_ARRAYTYPE_P(0);
  int extra = PG_GETARG_INT32(1);
  int ds = PG_GETARG_INT32(2);
  int dim = PASE_ARRNELEMS(vector);
  int size;
  int i;
  PASE *result;
  float4* data;

  if (dim > PASE_MAX_DIM) {
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
      errmsg("pase have more than limit dimensions.")));
  }

  size = PASE_SIZE(dim);
  result = (PASE *) palloc0(size);
  SET_VARSIZE(result, size);
  SET_PASE(result, dim, extra, ds);
  data = PASE_ARRPTR(vector);
  for (i = 0; i < dim; ++i) {
    result->x[i] = data[i];
  }

  PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(pase_text_i_i);

Datum
pase_text_i_i(PG_FUNCTION_ARGS) {
  text *vector;
  char *rawData;
  PASE *result;
  int extra, ds, dim, len, size, i;
  char decodedData[1024*1024];

  vector = PG_GETARG_TEXT_P(0);
  extra = PG_GETARG_INT32(1);
  ds = PG_GETARG_INT32(2);

  len = VARSIZE_ANY_EXHDR(vector);
  rawData = VARDATA_ANY(vector);
  dim = pg_b64_decode(rawData, len, decodedData) / sizeof(float4);
  if (dim > PASE_MAX_DIM) {
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
      errmsg("pase have more than limit dimensions[%d].", PASE_MAX_DIM)));
  }

  size = PASE_SIZE(dim);
  result = (PASE *) palloc0(size);
  SET_VARSIZE(result, size);
  SET_PASE(result, dim, extra, ds);

  for (i = 0; i < dim; ++i) {
    result->x[i] = ((float4*)decodedData)[i];
  }

  PG_RETURN_POINTER(result);
}
