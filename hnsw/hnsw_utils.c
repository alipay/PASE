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
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "common/base64.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/pase_hash_table.h"

#include "pase.h"
#include "hnsw/hnsw.h"

static int scount = 0;
// static function
static int RandomLevel(HNSWOptions *opts);
static void SetDefaultProbas(HNSWOptions *opts);
static float Distance2(Relation index, HNSWOptions *opts,
  HNSWGlobalId blkid1, HNSWGlobalId blkid2);

HNSWOptions *
MakeDefaultHNSWOptions() {
  int optsSize = sizeof(HNSWOptions);
  HNSWOptions *opts;

  opts = (HNSWOptions *) palloc(optsSize);
  opts->dim = 256;
  opts->base_nb_num = 16;
  opts->ef_build = 40;
  opts->ef_search = 50;
  opts->base64_encoded = 0;
  return opts;
}

int
HNSWFlushCachedPage(Relation index, HNSWBuildState *buildState) {
  Page page;
  GenericXLogState *state;
  int blkid;
  Buffer buffer = PaseNewBuffer(index);

  state = GenericXLogStart(index);
  page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
  memcpy(page, buildState->data.data, BLCKSZ);
  GenericXLogFinish(state);
  blkid = BufferGetBlockNumber(buffer);
  UnlockReleaseBuffer(buffer);
  return blkid;
}

// it must be called by build at first time
void
HNSWInitMetapage(Relation index) {
  Buffer metaBuffer;
  Page metaPage;
  GenericXLogState *state;
  HNSWOptions *opts;
  HNSWMetaPageData *metadata;
  HNSWGlobalId gid;

  metaBuffer = PaseNewBuffer(index);
  Assert(BufferGetBlockNumber(metaBuffer) == HNSW_METAPAGE_BLKNO);
  state = GenericXLogStart(index);
  metaPage = GenericXLogRegisterBuffer(state, metaBuffer,
    GENERIC_XLOG_FULL_IMAGE);

  opts = (HNSWOptions *) index->rd_options;
  if (!opts) {
    opts = MakeDefaultHNSWOptions();
  }
  SetDefaultProbas(opts);
  PageInit(metaPage, BLCKSZ, HNSW_METAPAGE_BLKNO);
  metadata = HNSWPageGetMeta(metaPage);
  memset(metadata, 0, sizeof(HNSWMetaPageData));
  metadata->magick_num = HNSW_MAGICK_NUMBER;
  HNSW_GID(gid, -1, -1, -1);
  metadata->entry_gid = gid;
  metadata->last_data_blkid = -1;
  metadata->opts = *opts;
  ((PageHeader) metaPage)->pd_lower += sizeof(HNSWMetaPageData);
  Assert(((PageHeader) metaPage)->pd_lower <= ((PageHeader) metaPage)->pd_upper);

  GenericXLogFinish(state);
  UnlockReleaseBuffer(metaBuffer);
  CHECK_FOR_INTERRUPTS();
}

void
HNSWFillMetaPage(Relation index, HNSWBuildState *state) {
  Buffer buffer;
  HNSWMetaPageData *meta;

  buffer = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
  LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
  meta = (HNSWMetaPageData *) PageGetContents(BufferGetPage(buffer));
  if (meta->magick_num != HNSW_MAGICK_NUMBER) {
    elog(ERROR, "Relation is not a hnsw index");
  }
  meta->entry_gid = state->entry_gid;
  meta->last_data_blkid = state->data_entry_blkid;
  meta->opts.data_tup_size = state->opts.data_tup_size;
  meta->opts.nb_tup_size = state->opts.nb_tup_size;
  meta->opts.real_max_level = state->cur_max_level;
  state = GenericXLogStart(index);
  GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
  GenericXLogFinish(state);
  UnlockReleaseBuffer(buffer);
  CHECK_FOR_INTERRUPTS();
}

void
InitHNSWBuildState(HNSWBuildState *state, Relation index) {
  Buffer buffer;
  HNSWMetaPageData *meta;
  HNSWOptions *opts;

  memset(state, 0, sizeof(HNSWBuildState));
  if (!index->rd_amcache) {
    opts = MemoryContextAlloc(index->rd_indexcxt, sizeof(HNSWOptions));
    buffer = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    meta = HNSWPageGetMeta(BufferGetPage(buffer));
    if (meta->magick_num != HNSW_MAGICK_NUMBER) {
      elog(ERROR, "Relation is not a hnsw index");
    }
    *opts = meta->opts;
    opts->nb_tup_size = sizeof(HNSWNeighborTuple);
    opts->data_tup_size = HNSWDATATUPLEHDRSZ + sizeof(float4) * opts->dim;
    index->rd_amcache = (void *)opts;
    state->entry_gid = meta->entry_gid;
    UnlockReleaseBuffer(buffer);
  }

  memcpy(&state->opts, index->rd_amcache, sizeof(state->opts));
  state->pre_data_blkid = -1;
}

void
InitHNSWBuildState4Insert(HNSWMetaPageData *meta, HNSWBuildState *state) {
  memset(state, 0, sizeof(HNSWBuildState));
  state->opts = meta->opts;
  state->data_entry_blkid = meta->last_data_blkid;
  state->cur_max_level = state->opts.real_max_level;
  state->entry_gid = meta->entry_gid;
}

PaseTuple *
HNSWNeighborTupleFormer(void *arg) {
  uint16 *tupSize = (uint16 *) arg;
  PaseTuple *res = (PaseTuple *) palloc(*tupSize);
  HNSWNeighborTuple *tup = (HNSWNeighborTuple *) res;
  HNSW_GID(tup->gid, 0, 0, 0);

  return res;
}

void
HNSWBuildLink(Relation index, HNSWBuildState *state) {
  double t0;
  float dNearest;
  PasePageList *list;
  PaseTuple *tup1;
  HNSWDataTuple *tup;
  int nbNum, level; 
  int count = 0;
  HNSWGlobalId gid;
  HNSWGlobalId nearest;
  int32 nbBlkid;
  int32 offset, dataBlkid;
  MemoryContext oldctx;
  HNSWVtable vtable;
  PasePageList *dtlist;

  t0 = elapsed();
  HVTInit(index->rd_indexcxt, &vtable);

  dtlist = InitPasePageListByNo(index,
    state->data_entry_blkid, state->opts.data_tup_size,
    sizeof(HNSWDataPageOpaqueData), state->indtuples);

  pase_page_foreach(tup1, dtlist) {
    oldctx = MemoryContextSwitchTo(state->tmpctx);
    offset = dtlist->cur_offset;
    dataBlkid = dtlist->cur_pageno;
    tup = (HNSWDataTuple *) tup1;
    nbNum = (state->opts).cum_nn_per_level[tup->level + 1];
    list = InitPasePageList(index, HNSWNeighborTupleFormer, nbNum,
      state->opts.nb_tup_size, sizeof(HNSWNeighborPageOpaqueData),
      &(state->opts.nb_tup_size));
    nbBlkid = list->header;
    HNSW_GID(gid, nbBlkid, dataBlkid, offset);

    count++;
    if (HNSW_CHECK_GID(state->entry_gid)) {
      state->entry_gid = gid;
      state->cur_max_level = tup->level;
      pfree(list);
      MemoryContextSwitchTo(oldctx);
      MemoryContextReset(state->tmpctx);
      continue;
    }
    nearest = state->entry_gid;
    dNearest = Distance(index, &(state->opts), tup->vector,
      nearest);
    level = GreedyUpdateNearest(index, &state->opts, state->cur_max_level,
      tup->level, &nearest, &dNearest, tup->vector);
    AddLinkFromHighToLow(index, state, nearest, dNearest, level,
      tup, gid, &vtable);
    if (tup->level > state->cur_max_level) {
      state->entry_gid = gid;
      state->cur_max_level = tup->level;
    }
    pfree(list);
    if (count % 1000 == 0) {
      elog(NOTICE, "build count: %d, total use time: [%f], distance fun calls %d",
        count, elapsed() - t0, scount);
    }
    MemoryContextSwitchTo(oldctx);
    MemoryContextReset(state->tmpctx);
  }
  pfree(dtlist);
  HVTFree(&vtable);
  elog(NOTICE, "build count: %d, total use time: [%f], distance fun calls %d",
    count, elapsed() - t0, scount);
}

HNSWDataTuple *
HNSWFormDataTuple(HNSWOptions *opts,
  ItemPointer iptr, Datum *values, bool *isnull) {
  ArrayType *arr;
  text *rawText;
  int dim, i, len;
  float4 *data;
  char *rawData;
  char dest[1024 * 1024];
  HNSWDataTuple *res = (HNSWDataTuple *) palloc(opts->data_tup_size);

  res->heap_ptr = *iptr;
  if (isnull[0]) {
    pfree(res);
    return NULL;
  }
  
  if (opts->base64_encoded) {
    rawText = DatumGetTextPP(values[0]);
    rawData = VARDATA_ANY(rawText);
    len = VARSIZE_ANY_EXHDR(rawText);

    memset(dest, 0, sizeof(dest));
    dim = pg_b64_decode(rawData, len, dest) / sizeof(float4);
    if (dim != opts->dim) {
      elog(WARNING, "data dimension[%d] not equal to configure dimension[%d]",
        dim, opts->dim);
      pfree(res);
      return NULL;
    }
    for (i = 0; i < dim; ++i) {
      res->vector[i] = ((float4*)dest)[i];
    }
  } else {
    arr = DatumGetArrayTypeP(values[0]);
    dim = PASE_ARRNELEMS(arr);
    if (dim != opts->dim) {
      elog(WARNING, "data dimension[%d] not equal to configure dimension[%d]",
        dim, opts->dim);
      pfree(res);
      return NULL;
    }
    data = PASE_ARRPTR(arr);
    for (i = 0; i < opts->dim; ++i) {
      res->vector[i] = data[i];
    }
  }
  res->level = RandomLevel(opts);
  return res;
}

bool
HNSWDataPageAddItem(HNSWBuildState *state, Page page, HNSWDataTuple *tuple) {
  HNSWDataTuple *tup;
  HNSWDataPageOpaque opaque;
  Pointer ptr;
  uint16 dataTupSize;

  dataTupSize = state->opts.data_tup_size;
  if (HNSWDataPageGetFreeSpace(state, page) < dataTupSize) {
    return false;
  }
  opaque = GetHNSWDataPageOpaque(page);
  tup = HNSWDataPageGetTuple(dataTupSize, page, opaque->maxoff + 1);
  memcpy((Pointer) tup, (Pointer) tuple, dataTupSize);
  opaque->maxoff++;
  ptr = (Pointer) HNSWDataPageGetTuple(dataTupSize, page, opaque->maxoff + 1);
  ((PageHeader) page)->pd_lower = ptr - page;
  Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);
  return true;
}

int
GreedyUpdateNearest(Relation index, HNSWOptions *opts, int32 maxLevel, int level,
  HNSWGlobalId *nearest, float *dNearest, const float4 *vector) {
  float dis;
  int cur;
  HNSWGlobalId prevNearest;
  int32 nbPage, beginOffset, endOffset;
  PaseTuple *tup1;
  HNSWNeighborTuple *tup;
  PasePageList *pageList;

  for (cur = maxLevel; cur > level; --cur) {
    for(;;) {
      prevNearest = *nearest;
      nbPage = nearest->nblkid;
      beginOffset = opts->cum_nn_per_level[cur] + 1;
      endOffset = opts->cum_nn_per_level[cur + 1];
      pageList = InitPasePageListByNo(index, nbPage, sizeof(HNSWNeighborTuple),
          sizeof(HNSWNeighborPageOpaqueData), endOffset);
      pase_page_foreach2(tup1, pageList, beginOffset) {
        tup = (HNSWNeighborTuple *) tup1;
        if (HNSW_CHECK_GID(tup->gid)) {
          UnlockReleaseBuffer(pageList->rw_opts.buffer);
          break;
        }
        dis = Distance(index, opts, vector, tup->gid);
        if (dis < *dNearest) {
          *nearest = tup->gid;
          *dNearest = dis;
        }
      }
      pfree(pageList);
      if (HNSW_CHECK_GID_EQ(*nearest, prevNearest)) {
        break;
      }
    }
  }
  return cur;
}

void
SearchNbToAdd(Relation index, HNSWOptions *opts, int level, HNSWGlobalId nearest,
  float dNearest, PriorityQueue *results, HNSWVtable *vtable,
  HNSWDataTuple *dtTup) {
  // top is nearest candidate
  bool type = false; // nearest --> farthest
  PriorityQueue *candidates = PriorityQueueAllocate(HNSWPriorityQueueCmp, &type);
  int32 beginOffset, endOffset;
  float dis;
  PaseTuple *tup1;
  PasePageList *pageList;
  HNSWPriorityQueueNode *node, *node1, *node2;
  HNSWNeighborTuple *tup;

  ADD_HNSW_PQ_NODE(candidates, nearest, dNearest);
  ADD_HNSW_PQ_NODE(results, nearest, dNearest);
  HVTSet(vtable, nearest);

  while (!PriorityQueueIsEmpty(candidates)) {
    // get nearest
    node1 = (HNSWPriorityQueueNode *)PriorityQueuePop(candidates);
    node2 = (HNSWPriorityQueueNode *)PriorityQueueFirst(results);
    if (node2 == NULL) {
      PriorityQueueFree(candidates); 
      return;
    }
    // candidates nearest is greater than result farthest, so break
    if (node1->distance > node2->distance) {
      pfree(node1);
      break;
    }

    beginOffset = opts->cum_nn_per_level[level] + 1;
    endOffset = opts->cum_nn_per_level[level + 1];
    pageList = InitPasePageListByNo(index, node1->gid.nblkid,
      sizeof(HNSWNeighborTuple), sizeof(HNSWNeighborPageOpaqueData), endOffset);
    pase_page_foreach2(tup1, pageList, beginOffset) {
      tup = (HNSWNeighborTuple *) tup1;
      if (HNSW_CHECK_GID(tup->gid)) {
        UnlockReleaseBuffer(pageList->rw_opts.buffer);
        break;
      }
      if (HVTGet(vtable, tup->gid)) {
        continue;
      }
      HVTSet(vtable, tup->gid);
      dis = Distance(index, opts, dtTup->vector, tup->gid);
      if (PriorityQueueSize(results) < opts->ef_build || node1->distance > dis) {
        ADD_HNSW_PQ_NODE(candidates, tup->gid, dis);
        ADD_HNSW_PQ_NODE(results, tup->gid, dis);
        if (PriorityQueueSize(results) > opts->ef_build) {
          node = (HNSWPriorityQueueNode *)PriorityQueuePop(results);
          pfree(node);
        }
      }
    }
    pfree(pageList);
  }
  PriorityQueueFree(candidates); 
}

void
ShrinkNbList(Relation index, HNSWBuildState *state, int level,
  PriorityQueue **linkTargets) {
  bool type = false;  // nearest --> farthest
  PriorityQueue *resultset;
  HNSWPriorityQueueNode *nodeArray, *node;
  float dist_v1_q, dist_v1_v2;
  bool good;
  int i;
  int count = 0;
  int maxNbSize = (state->opts).cum_nn_per_level[level + 1] -
    (state->opts).cum_nn_per_level[level];

  if (PriorityQueueSize(*linkTargets) < maxNbSize) {
    return;
  }
  nodeArray = palloc(MAX_HNSW_LEVEL * 2 * sizeof(HNSWPriorityQueueNode));
  resultset = PriorityQueueAllocate(HNSWPriorityQueueCmp, &type);
  while (!PriorityQueueIsEmpty(*linkTargets)) {
    node = (HNSWPriorityQueueNode *)PriorityQueuePop(*linkTargets);
    ADD_HNSW_PQ_NODE(resultset, node->gid, node->distance);
    pfree(node);
  }

  while (!PriorityQueueIsEmpty(resultset)) {
    node = (HNSWPriorityQueueNode *)PriorityQueuePop(resultset);
    dist_v1_q = node->distance;
    good = true;

    for (i = 0; i < count; ++i) {
      dist_v1_v2 = Distance2(index, &(state->opts),
        node->gid, nodeArray[i].gid);
      if (dist_v1_v2 < dist_v1_q) {
        good = false;
        break;
      }
    }
    if (good) {
      nodeArray[count++] = *node;
      if (count >= maxNbSize) {
        pfree(node);
        break;
      }
    }
    pfree(node);
  }
  
  PriorityQueueFree(resultset); 
  for (i = 0; i < count; ++i) {
    ADD_HNSW_PQ_NODE(*linkTargets, nodeArray[i].gid,
      nodeArray[i].distance);
  }
  pfree(nodeArray);
  return;
}

void
AddLinkFromHighToLow(Relation index, HNSWBuildState *state, HNSWGlobalId nearest,
  float dNearest, int level, HNSWDataTuple *tup, HNSWGlobalId sourceid,
  HNSWVtable *vtable) {
  float qdis;
  int cur;
  bool type;
  PriorityQueue *linkTargets;
  HNSWPriorityQueueNode *node;

  for (cur = level; cur >= 0; --cur) {
    type = true;  // farthest --> nearest
    linkTargets = PriorityQueueAllocate(HNSWPriorityQueueCmp, &type);

    SearchNbToAdd(index, &(state->opts), cur, nearest, dNearest, linkTargets,
      vtable, tup);
    ShrinkNbList(index, state, cur, &linkTargets);

    while (!PriorityQueueIsEmpty(linkTargets)) {
      node = (HNSWPriorityQueueNode *)PriorityQueuePop(linkTargets);
      if (!HNSW_CHECK_GID(node->gid)) {
        qdis = -0.1;
        AddLink(index, state, node->gid, sourceid, cur, &qdis);
        AddLink(index, state, sourceid, node->gid, cur, &qdis);
      }
      pfree(node);
    }
    PriorityQueueFree(linkTargets); 
    HVTReset(vtable);
  }
}

void
AddLink(Relation index, HNSWBuildState *state, HNSWGlobalId src,
  HNSWGlobalId dest, int level, float *qdis) {
  int32 cur = src.nblkid;
  int32 beginOffset = (state->opts).cum_nn_per_level[level] + 1;
  int32 endOffset = (state->opts).cum_nn_per_level[level + 1];
  PasePageList *pageList = InitPasePageListByNo(index, cur,
    sizeof(HNSWNeighborTuple), sizeof(HNSWNeighborPageOpaqueData), endOffset);
  HNSWNeighborTuple *endTup =
    (HNSWNeighborTuple *) PasePlGet(pageList, endOffset);
  bool type = true;  // farthest --> nearest
  float dis;
  PriorityQueue *resultQueue;
  PaseTuple *tup;
  HNSWNeighborTuple *tup1;
  if (endTup == NULL) {
    elog(ERROR, "HNSWNeighborPage format is invalid!");
  }
  if (HNSW_CHECK_GID(endTup->gid)) {
    pase_page_foreach2(tup, pageList, beginOffset) {
      tup1 = (HNSWNeighborTuple *) tup;
      if (HNSW_CHECK_GID(tup1->gid)) {
        tup1->gid = dest;
        PaseFlushBuff(index, pageList->rw_opts.buffer);
        UnlockReleaseBuffer(pageList->rw_opts.buffer);
        CHECK_FOR_INTERRUPTS();
        pfree(pageList);
        return;
      }
    }
  }

  resultQueue = PriorityQueueAllocate(HNSWPriorityQueueCmp, &type);
  if (*qdis < 0.0) {
    *qdis = Distance2(index, &(state->opts), src, dest);
  }
  ADD_HNSW_PQ_NODE(resultQueue, dest, *qdis);
  pase_page_foreach2(tup, pageList, beginOffset) {
    tup1 = (HNSWNeighborTuple *) tup;
    if (HNSW_CHECK_GID(tup1->gid)) {
      tup1->gid = dest;
      PaseFlushBuff(index, pageList->rw_opts.buffer);
      UnlockReleaseBuffer(pageList->rw_opts.buffer);
      CHECK_FOR_INTERRUPTS();
      PriorityQueueFree(resultQueue); 
      pfree(pageList);
      return;
    }
    dis = Distance2(index, &(state->opts), src, tup1->gid);
    ADD_HNSW_PQ_NODE(resultQueue, tup1->gid, dis);
  }

  ShrinkNbList(index, state, level, &resultQueue);
  FillNeighborPages(index, pageList, resultQueue, beginOffset, endOffset);
  PriorityQueueFree(resultQueue); 
  pfree(pageList);
  return;
}

void
FillNeighborPages(Relation index, PasePageList *pageList,
  PriorityQueue *resultQueue, int32 begin, int32 end) {
  PaseTuple* tup;

  pase_page_foreach2(tup, pageList, begin) {
    HNSWNeighborTuple *tup1 = (HNSWNeighborTuple *) tup;
    HNSWPriorityQueueNode *node =
        (HNSWPriorityQueueNode *)PriorityQueuePop(resultQueue);

    if (node != NULL) {
      tup1->gid = node->gid;
      PaseFlushBuff(index, pageList->rw_opts.buffer);
      LockBuffer(pageList->rw_opts.buffer, BUFFER_LOCK_UNLOCK);
      LockBuffer(pageList->rw_opts.buffer, BUFFER_LOCK_SHARE);
      CHECK_FOR_INTERRUPTS();
      pfree(node);
    } else {
      HNSW_GID(tup1->gid, 0, 0, 0);
    }
  }
}

// if type = false: minHeap nearest --> farthest.
// if type = true: maxHeap farthest --> nearest.
int
HNSWPriorityQueueCmp(const PriorityQueueNode *a,
  const PriorityQueueNode *b, void *arg) {
  const HNSWPriorityQueueNode *node1 = (HNSWPriorityQueueNode *)a;
  const HNSWPriorityQueueNode *node2 = (HNSWPriorityQueueNode *)b;
  bool *type = (bool *)arg;

  if (node1->distance > node2->distance) {
    return *type ? 1 : -1;
  } else if (node1->distance < node2->distance) {
    return *type ? -1 : 1;
  }
  return 0;
}

static int
RandomLevel(HNSWOptions *opts) {
  float4 f = (float4)rand()/(float4)(RAND_MAX);
  int level;

  for (level = 0; level < opts->real_max_level; level++) {
    if (f < opts->assign_probas[level]) {
      return level;
    }
    f -= opts->assign_probas[level];
  }
  return opts->real_max_level == 0 ? 0 : opts->real_max_level - 1;
}

static void
SetDefaultProbas(HNSWOptions *opts) {
  int nbNum = 0;
  int level;
  float levelMult = 1.0 / log(opts->base_nb_num);
  float proba;

  opts->cum_nn_per_level[0] = 0;
  for (level = 0; ;level++) {
    proba = exp(-level / levelMult) * (1 - exp(-1 / levelMult));
    if (proba < 1e-9) {
      break;
    }
    nbNum += (level == 0) ? opts->base_nb_num * 2 : opts->base_nb_num;
    opts->cum_nn_per_level[level + 1] = nbNum;
    opts->assign_probas[level] = proba;
  }
  opts->real_max_level = level;
  if (opts->real_max_level > MAX_HNSW_LEVEL) {
    elog(ERROR, "real_max_level is greater than MAX_HNSW_LEVEL");
  }
}

float
Distance(Relation index, HNSWOptions *opts,
    const float4 *vector, HNSWGlobalId gid) {
  float distance = 0.0;
  Buffer buffer;
  Page page;
  int maxOffset, offset;
  HNSWDataTuple *tup; 

  scount++;
  buffer = ReadBuffer(index, gid.dblkid);
  LockBuffer(buffer, BUFFER_LOCK_SHARE);
  page = BufferGetPage(buffer);
  maxOffset = HNSWDataPageGetMaxOffset(page);
  offset = gid.doffset;
  if (offset > maxOffset) {
    elog(ERROR, "Distance:invalid entry offset  offset[%d] maxOffset[%d]",
      offset, maxOffset);
  }
  tup = HNSWDataPageGetTuple(opts->data_tup_size, page, offset);
  distance = fvec_L2sqr(vector, tup->vector, opts->dim);
  UnlockReleaseBuffer(buffer);
  return distance;
}

static float
Distance2(Relation index, HNSWOptions *opts, HNSWGlobalId gid1, HNSWGlobalId gid2) {
  float distance = 0.0;
  Buffer buffer;
  Page page;
  int maxOffset, offset;
  HNSWDataTuple *tup1, *tup2; 

  scount++;

  buffer = ReadBuffer(index, gid1.dblkid);
  LockBuffer(buffer, BUFFER_LOCK_SHARE);
  page = BufferGetPage(buffer);
  maxOffset = HNSWDataPageGetMaxOffset(page);
  offset = gid1.doffset;
  if (offset > maxOffset) {
    elog(ERROR, "Distance2:invalid entry offset 1 offset[%d] maxOffset[%d]",
      offset, maxOffset);
  }
  UnlockReleaseBuffer(buffer);
  tup1 = HNSWDataPageGetTuple(opts->data_tup_size, page, offset);

  buffer = ReadBuffer(index, gid2.dblkid);
  LockBuffer(buffer, BUFFER_LOCK_SHARE);
  page = BufferGetPage(buffer);
  maxOffset = HNSWDataPageGetMaxOffset(page);
  offset = gid2.doffset;
  if (offset > maxOffset) {
    elog(ERROR, "Distance2:invalid entry offset 2 offset[%d] maxOffset[%d]",
      offset, maxOffset);
  }
  tup2 = HNSWDataPageGetTuple(opts->data_tup_size, page, offset);
  distance = fvec_L2sqr(tup1->vector, tup2->vector, opts->dim);
  UnlockReleaseBuffer(buffer);
  return distance;
}

void
FindItDataByOffset(Relation index, uint16 dataTupSize, HNSWGlobalId gid,
  ItemPointerData *it_data) {
  Buffer buffer;
  Page page;
  int maxOffset;
  int offset;
  HNSWDataTuple *tup;

  buffer = ReadBuffer(index, gid.dblkid);
  LockBuffer(buffer, BUFFER_LOCK_SHARE);
  page = BufferGetPage(buffer);
  maxOffset = HNSWDataPageGetMaxOffset(page);
  offset = gid.doffset;
  if (offset > maxOffset) {
    elog(ERROR, "invalid entry offset");
  }

  tup = HNSWDataPageGetTuple(dataTupSize, page, offset);
  if (NULL != it_data) {
    memcpy(it_data, &tup->heap_ptr, sizeof(ItemPointerData));
  }
  UnlockReleaseBuffer(buffer);
}


////////////////////////////////////////////////////////
void
HVTInit(MemoryContext ctx, HNSWVtable *vt) {
  vt->vt1 = PaseHashCreateTable(ctx);
  vt->vt2 = PaseHashCreateTable(ctx);
}

void
HVTReset(HNSWVtable *vt) {
  PaseHashTableReset(vt->vt1);
  PaseHashTableReset(vt->vt2);
}

void
HVTSet(HNSWVtable *vt, HNSWGlobalId gid) {
  uint64_t hkey = ((uint64_t)(gid.doffset) << 32) | ((uint64_t)(gid.dblkid));
  PaseHashInsert(vt->vt1, (uint64_t)gid.nblkid, 1);
  PaseHashInsert(vt->vt2, hkey, 1);
}

bool
HVTGet(HNSWVtable *vt, HNSWGlobalId gid) {
  bool val;
  uint64_t hkey = ((uint64_t)(gid.doffset) << 32) | ((uint64_t)(gid.dblkid));
  return ((PaseHashLookUp(vt->vt1, gid.nblkid, &val) == 0) &&
    (PaseHashLookUp(vt->vt2, hkey, &val) == 0));
}

void
HVTFree(HNSWVtable *vt) {
  PaseHashTableFree(vt->vt1);
  PaseHashTableFree(vt->vt2);
}
