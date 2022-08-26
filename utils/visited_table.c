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

#include "utils/visited_table.h"

VisitedTable *
VTAllocate() {
  VisitedTable *vt = palloc0(VT_SIZE);
  SET_VARSIZE(vt, VT_SIZE);
  return vt;
}

void
VTReset(VisitedTable *vt) {
  memset(vt->visited, 0, VT_MAX_BIT_SIZE);
}

void
VTSet(VisitedTable *vt, int32 no) {
  vt->visited[no / VT_I_SIZE] |= (1 << (no % VT_I_SIZE));
}

bool
VTGet(VisitedTable *vt, int32 no) {
  return (vt->visited[no / VT_I_SIZE] & (1 << (no % VT_I_SIZE))) > 0;
}

void
VTFree(VisitedTable *vt) {
  pfree(vt);
}
