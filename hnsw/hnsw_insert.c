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

static void HNSWBuildLink4Insert(Relation index, HNSWBuildState *state,
  HNSWDataTuple *tup, int32 dataPageBlkid,
  int32 offsetInDataPage);

bool
hnsw_insert(Relation index, Datum *values, bool *isnull,
  ItemPointer htCtid, Relation heapRel,
  IndexUniqueCheck checkUnique,
  IndexInfo *indexInfo) {
  HNSWBuildState state;
  Buffer metaBuffer;
  HNSWMetaPageData *meta;
  HNSWDataTuple *tup;
  Buffer dataBuffer;
  Page page;
  int32 dataPageBlkid = -1;
  int32 offsetInDataPage;

  metaBuffer = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
  LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
  meta = HNSWPageGetMeta(BufferGetPage(metaBuffer));
  InitHNSWBuildState4Insert(meta, &state);
  UnlockReleaseBuffer(metaBuffer);


  // add data
  tup = HNSWFormDataTuple(&(state.opts), htCtid, values, isnull);
  if (!tup) {
    return false;
  }
  if (state.data_entry_blkid == -1) {
    elog(ERROR, "No exist index found. Build index first.");
    return false;
  } else {
    dataBuffer = ReadBuffer(index, state.data_entry_blkid);
    LockBuffer(dataBuffer, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(dataBuffer);

    // add to exist page
    if (HNSWDataPageAddItem(&state, page, tup)) {
      PaseFlushBuff(index, dataBuffer);
      UnlockReleaseBuffer(dataBuffer);
      offsetInDataPage = HNSWDataPageGetMaxOffset(page);
    } else {
      UnlockReleaseBuffer(dataBuffer);   

      // add to new page
      state.pre_data_blkid = state.data_entry_blkid;
      HNSW_INIT_CACHE_PAGE(HNSWDataPageOpaque, &state);
      if (!HNSWDataPageAddItem(&state, state.data.data, tup)) {
        elog(ERROR, "could not add new tuple to empty data page");
      }
      dataPageBlkid = HNSWFlushCachedPage(index, &state);
      state.data_entry_blkid = dataPageBlkid;
      offsetInDataPage = HNSWDataPageGetMaxOffset(state.data.data);
    }
  }

  // add neighbor
  HNSWBuildLink4Insert(index, &state, tup, state.data_entry_blkid,
      offsetInDataPage);
  HNSWFillMetaPage(index, &state);

  return true;
}

void
HNSWBuildLink4Insert(Relation index, HNSWBuildState *state,
  HNSWDataTuple *tup, int32 dataPageBlkid, int32 offsetInDataPage) {
  HNSWOptions *opts = &state->opts;
  int nbNum, level;
  HNSWGlobalId gid;
  HNSWGlobalId nearest;
  int32 nbBlkid;
  float dNearest;
  PasePageList *list;
  HNSWVtable vtable;
  
  HVTInit(index->rd_indexcxt, &vtable);
  nbNum = opts->cum_nn_per_level[tup->level + 1];
  list = InitPasePageList(index, HNSWNeighborTupleFormer, nbNum,
    state->opts.nb_tup_size, sizeof(HNSWNeighborPageOpaqueData),
    &(state->opts.nb_tup_size));
  nbBlkid = list->header;
  HNSW_GID(gid, nbBlkid, dataPageBlkid, offsetInDataPage);

  // first node
  if (HNSW_CHECK_GID(state->entry_gid)) {
    state->entry_gid = gid;
    state->cur_max_level = tup->level;
    pfree(list);
    HVTFree(&vtable);
    return;
  }

  nearest = state->entry_gid;
  dNearest = Distance(index, opts, tup->vector, nearest);

  level = GreedyUpdateNearest(index, opts, state->cur_max_level, tup->level,
    &nearest, &dNearest, tup->vector);

  AddLinkFromHighToLow(index, state, nearest, dNearest, level, tup,
    gid, &vtable);
  if (tup->level > state->cur_max_level) {
    state->entry_gid = gid;
    state->cur_max_level = tup->level;
  }
  pfree(list);
  HVTFree(&vtable);
}
