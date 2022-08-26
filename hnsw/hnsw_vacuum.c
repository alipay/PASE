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

#include "access/genam.h"
#include "catalog/storage.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"

IndexBulkDeleteResult *
hnsw_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

IndexBulkDeleteResult *
hnsw_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats) {
  if (info->analyze_only)
    return stats;
  if (stats == NULL)
    stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
  return stats;
}
