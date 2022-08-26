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
// ==============================================================================

#ifndef PASE_TYPE_PASE_DATA_H_
#define PASE_TYPE_PASE_DATA_H_

typedef struct PASE {
   // for flexible array
   int32 vl_len_;
   // Header contains info about PASE.
   // Following information is stored:
   //  bits 0-9  : number of pase dimensions;
   //  bits 10-27 : ivfflat store nprobe num;
   //               hnsw store nn sort queue len;
   //  bits 28-31 : distance strategy num
   //               0 : l2 (default)
   //               1 : ip
  unsigned int header;

  float4 x[FLEXIBLE_ARRAY_MEMBER];
} PASE;

// dim limited
#define PASE_MAX_DIM (512)

// PASE access macros
#define PASE_DIM_MASK      0x3ff
#define PASE_EXTRA_MASK    0xffffc00
#define PASE_DS_MASK       0xf0000000

// dim
#define PASE_DIM(_x)           ( (_x)->header & PASE_DIM_MASK )
// extra data
#define PASE_EXTRA(_x)         ( ((_x)->header & PASE_EXTRA_MASK) >> 10 )
// distance strategy num
#define PASE_DS(_x)            ( ((_x)->header & PASE_DS_MASK) >> 28 )

#define SET_PASE(_p, _d, _e, _t)                                      \
  do {                                                               \
    (_p)->header = ((_p)->header & ~PASE_DIM_MASK) | (_d);            \
    (_p)->header = ((_p)->header & ~PASE_EXTRA_MASK) | ((_e) << 10);  \
    (_p)->header = ((_p)->header & ~PASE_DS_MASK) | ((_t) << 28);     \
  } while (0)

#define PaseKNNDistanceL2 0
#define PaseKNNDistanceIP 1
#define PaseKNNDistanceCosine 2

#define PASE_SIZE(_d)    (offsetof(PASE, x) + sizeof(float4)*(_d))

// fmgr interface macros
#define DatumGetPASE(_x)    ((PASE *) PG_DETOAST_DATUM(_x))
#define PG_GETARG_PASE(_x)  DatumGetPASE(PG_GETARG_DATUM(_x))

#endif  // PASE_TYPE_PASE_DATA_H_
