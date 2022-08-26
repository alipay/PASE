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
// Pase is ann search index extention of PostgreSQL.
// Now it contains HNSW and IVFFlat.

#ifndef PASE_PASE_H_
#define PASE_PASE_H_

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "nodes/relation.h"

#include "hnsw/hnsw.h"
#include "ivfflat/ivfflat.h"
#include "utils/vector_util.h"
#include "type/pase_data.h"

#define PASE_ARRPTR(_x)  ((float4 *)ARR_DATA_PTR(_x))
#define PASE_ARRNELEMS(_x)  ArrayGetNItems(ARR_NDIM(_x), ARR_DIMS(_x))

// pase_hander.c
extern void _PG_init(void);
extern bytea *hnsw_options(Datum reloptions, bool validate);

// pase_utils.c
extern Buffer PaseNewBuffer(Relation index);
extern void PaseFlushBuff(Relation index, Buffer buffer);
extern double elapsed ();

#endif  // PASE_PASE_H_
