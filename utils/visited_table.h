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
// Now it is base on mem. TODO(lyee.lit) : Base on disk Page.

#ifndef PASE_UTILS_VISITED_TABLE_H_
#define PASE_UTILS_VISITED_TABLE_H_

#include "postgres.h"

// 2^32
// #define VT_MAX_BIT_SIZE 4294967296
#define VT_MAX_BIT_SIZE 573741824
#define VT_I_SIZE 32

typedef struct VisitedTable {
  // for flexible array
  int32 vl_len_;
  int32 visited[FLEXIBLE_ARRAY_MEMBER];
} VisitedTable;

extern VisitedTable* VTAllocate();
extern void VTReset(VisitedTable *vt);
extern void VTSet(VisitedTable *vt, int32 no);
extern bool VTGet(VisitedTable *vt, int32 no);
extern void VTFree(VisitedTable *vt);

#define VT_SIZE (offsetof(VisitedTable, visited) + VT_MAX_BIT_SIZE)

#endif  // PASE_UTILS_VISITED_TABLE_H_
