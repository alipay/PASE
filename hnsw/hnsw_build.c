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
#include "access/generic_xlog.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pase.h"

static void
HNSWBuildCallback(Relation index, HeapTuple htup, Datum *values,
  bool *isnull, bool tupleIsAlive, void *state) {
  HNSWBuildState *buildState;
  HNSWDataTuple *tup;
  MemoryContext oldCtx;
  int blkid;

  buildState = (HNSWBuildState *) state;
  oldCtx = MemoryContextSwitchTo(buildState->tmpctx);
  tup = HNSWFormDataTuple(&(buildState->opts), &htup->t_self, values, isnull);
  if (!tup) {
    MemoryContextSwitchTo(oldCtx);
    MemoryContextReset(buildState->tmpctx);
    return;
  }
  if (HNSWDataPageAddItem(buildState, buildState->data.data, tup)) {
    buildState->count++;
  } else {
    blkid = HNSWFlushCachedPage(index, buildState);
    CHECK_FOR_INTERRUPTS();
    buildState->pre_data_blkid= blkid;
    buildState->data_entry_blkid = blkid;
    HNSW_INIT_CACHE_PAGE(HNSWDataPageOpaque, buildState);
    if (!HNSWDataPageAddItem(buildState, buildState->data.data, tup)) {
      MemoryContextSwitchTo(oldCtx);
      MemoryContextReset(buildState->tmpctx);
      elog(ERROR, "could not add new tuple to empty page");
    }
    buildState->count++;
  }
  buildState->indtuples += 1;
  MemoryContextSwitchTo(oldCtx);
  MemoryContextReset(buildState->tmpctx);
}

IndexBuildResult *
hnsw_build(Relation heap, Relation index, IndexInfo *indexInfo) {
  double reltuples;
  int blkid;
  HNSWBuildState buildState;
  HNSWBuildState *state;
  IndexBuildResult *result;

  srand((unsigned int)time(NULL));

  if (RelationGetNumberOfBlocks(index) != 0) {
    elog(ERROR, "index %s already contains data",
      RelationGetRelationName(index));
  }

  HNSWInitMetapage(index);
  memset(&buildState, 0, sizeof(buildState));
  InitHNSWBuildState(&buildState, index);
  buildState.tmpctx = AllocSetContextCreate(CurrentMemoryContext,
    "build temporary context", ALLOCSET_DEFAULT_SIZES);
  state = &buildState;
  HNSW_INIT_CACHE_PAGE(HNSWDataPageOpaque, state);
  reltuples = IndexBuildHeapScan(heap, index, indexInfo, true,
                   HNSWBuildCallback, (void *) &buildState, NULL);
  blkid = HNSWFlushCachedPage(index, &buildState);
  buildState.data_entry_blkid = blkid;
  elog(DEBUG1, "final build blkid is %d", blkid);

  elog(DEBUG1, "build options, dim=[%d], base_nb_num=[%d], ef_build=[%d], "
    "ef_search=[%d], base64_encoded=[%d]", buildState.opts.dim,
    buildState.opts.base_nb_num, buildState.opts.ef_build,
    buildState.opts.ef_search, buildState.opts.base64_encoded);
  if (buildState.indtuples < 0) {
    elog(ERROR, "no data to build!");
  } else if (buildState.indtuples > 0) {
    HNSWBuildLink(index, &buildState);
  }
  HNSWFillMetaPage(index, &buildState);
  MemoryContextDelete(buildState.tmpctx);
  result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
  result->heap_tuples = reltuples;
  result->index_tuples = buildState.indtuples;
  return result;
}

void
hnsw_buildempty(Relation index) {
  elog(NOTICE, "hnsw_buildempty");
}
