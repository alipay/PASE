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

#include "access/relscan.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pase.h"
#include "utils/priority_queue.h"

static void HNSWSearch(Relation index, HNSWScanOpaque so, HNSWMetaPage meta,
  uint16 topk, float4 *queryVec); 

static void HNSWDoSearch(Relation index, HNSWOptions *opts, int ef,
  float *queryVec, HNSWGlobalId nearest, float dNearest, int level,
  PriorityQueue *result, HNSWVtable *vtable);

IndexScanDesc
hnsw_beginscan(Relation r, int nkeys, int norderbys) {
  IndexScanDesc scan;
  HNSWScanOpaque so;
  MemoryContext scanCtx, oldCtx;

  scan = RelationGetIndexScan(r, nkeys, norderbys);
  scanCtx = AllocSetContextCreate(CurrentMemoryContext,
    "hnsw scan context", ALLOCSET_DEFAULT_SIZES);
  so = (HNSWScanOpaque) palloc(sizeof(HNSWScanOpaqueData));
  oldCtx = MemoryContextSwitchTo(scanCtx);
  so->scan_ctx = scanCtx;
  so->scan_pase = NULL;
  so->queue = NULL;
  so->first_call = true;

  if (scan->numberOfOrderBys > 0) {
    scan->xs_orderbyvals = palloc(sizeof(Datum) * scan->numberOfOrderBys);
    scan->xs_orderbynulls = palloc(sizeof(bool) * scan->numberOfOrderBys);
    memset(scan->xs_orderbynulls, true, sizeof(bool) * scan->numberOfOrderBys);
  }
  scan->opaque = so;

  MemoryContextSwitchTo(oldCtx);
  return scan;
}

void
hnsw_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
  ScanKey orderbys, int norderbys) {
  MemoryContext oldCtx;
  HNSWScanOpaque so;
  bool type;

  so = (HNSWScanOpaque) scan->opaque;
  oldCtx = MemoryContextSwitchTo(so->scan_ctx);
  if (NULL != so->queue) {
    PriorityQueueFree(so->queue); 
    so->queue = NULL;
  }

  type = false; // nearest --> farthest
  so->scan_pase = NULL;
  so->queue = PriorityQueueAllocate(HNSWPriorityQueueCmp, &type);
  so->first_call = true;

  if (scankey && scan->numberOfKeys > 0) {
    memmove(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));
  }
  if (orderbys && scan->numberOfOrderBys > 0) {
    memmove(scan->orderByData, orderbys,
      scan->numberOfOrderBys * sizeof(ScanKeyData));
  }

  MemoryContextSwitchTo(oldCtx);
  elog(DEBUG1, "hnsw_rescan");
}

bool
hnsw_gettuple(IndexScanDesc scan, ScanDirection dir) {
  HNSWScanOpaque so;
  HNSWMetaPageData *meta;
  ItemPointerData itData;
  Buffer metaBuffer;
  HNSWPriorityQueueNode* node;
  int extra;

  if (dir != ForwardScanDirection) {
    elog(ERROR, "hnsw only support forward scan direction");
  }
  if (!scan->orderByData) {
    elog(WARNING, "orderByData is invalid");
    return false;
  }
  if (!scan->orderByData->sk_argument) {
    elog(WARNING, "orderBy value is invalid");
    return false;
  }
  so = (HNSWScanOpaque)scan->opaque;
  scan->xs_recheck = false;
  scan->xs_recheckorderby = false;
  if (so->first_call) {
    so->scan_pase = DatumGetPASE(scan->orderByData->sk_argument);
    metaBuffer = ReadBuffer(scan->indexRelation, HNSW_METAPAGE_BLKNO);
    LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
    meta = HNSWPageGetMeta(BufferGetPage(metaBuffer));
    if (PASE_DIM(so->scan_pase) != meta->opts.dim) {
      elog(ERROR, "query dimemsion(%u) not equal to data dimemsion(%u)",
              PASE_DIM(so->scan_pase), meta->opts.dim);
    }
    so->data_tup_size = meta->opts.data_tup_size;
    extra = PASE_EXTRA(so->scan_pase);
    if (extra > 0) {
      meta->opts.ef_search = extra;
    }
    // try get all neighbors from index and return one result
    HNSWSearch(scan->indexRelation, so, meta, meta->opts.ef_search, so->scan_pase->x);
    if (!PriorityQueueIsEmpty(so->queue)) {
      node = (HNSWPriorityQueueNode *)PriorityQueuePop(so->queue);
      FindItDataByOffset(scan->indexRelation, so->data_tup_size,
        node->gid, &itData);
      scan->xs_ctup.t_self = itData; 
      if (scan->numberOfOrderBys > 0) {
        scan->xs_orderbyvals[0] = Float4GetDatum(node->distance);
        scan->xs_orderbynulls[0] = false;
      }
      pfree(node);
    }
    so->first_call = false; 
    UnlockReleaseBuffer(metaBuffer);
  } else {
    // return one reslut from existed results
    if (!PriorityQueueIsEmpty(so->queue)) {
      node = (HNSWPriorityQueueNode *)PriorityQueuePop(so->queue);
      FindItDataByOffset(scan->indexRelation, so->data_tup_size,
        node->gid, &itData);
      scan->xs_ctup.t_self = itData;
      if (scan->numberOfOrderBys >0) {
        scan->xs_orderbyvals[0] = Float4GetDatum(node->distance);
        scan->xs_orderbynulls[0] = false;
      }
      pfree(node);
    } else {
      elog(WARNING, "no more data to pop");
      return false;
    }
  }
  return true;
}

void
hnsw_endscan(IndexScanDesc scan) {
  HNSWScanOpaque so = (HNSWScanOpaque) scan->opaque;

  if (NULL != so) {
    PriorityQueueFree(so->queue);
    if (so->scan_ctx) {
      MemoryContextDelete(so->scan_ctx);
    }
    pfree(so);
  }
}

static void
HNSWSearch(Relation index, HNSWScanOpaque so, HNSWMetaPage meta, uint16 topk,
  float4 *queryVec) {
  HNSWGlobalId nearest;
  float dNearest;
  int ef;
  const int level = 1;
  bool type;
  PriorityQueue *result;
  HNSWOptions *opts = &meta->opts;
  HNSWVtable vtable;
  HNSWPriorityQueueNode* node;

  if (HNSW_CHECK_GID(meta->entry_gid)) {
    return;
  }
  nearest = meta->entry_gid;
  dNearest = Distance(index, opts, queryVec, nearest);
  GreedyUpdateNearest(index, opts, opts->real_max_level, level, &nearest, &dNearest, queryVec);

  ef = opts->ef_search > topk ? opts->ef_search : topk;
  HVTInit(index->rd_indexcxt, &vtable);
  type = true; // farthest --> nearest
  result = PriorityQueueAllocate(HNSWPriorityQueueCmp, &type);

  // do search on level 0
  HNSWDoSearch(index, opts, ef, queryVec, nearest, dNearest, 0, result, &vtable);
  // remove resident node
  while (PriorityQueueSize(result) > topk) {
    node = (HNSWPriorityQueueNode *)PriorityQueuePop(result);
    pfree(node);
  }

  // MaxHeap to MinHeap
  while (PriorityQueueSize(result) > 0) {
    node = (HNSWPriorityQueueNode *)PriorityQueuePop(result); 
    PriorityQueueAdd(so->queue, (PriorityQueueNode *)node);
  }
  PriorityQueueFree(result);
  HVTFree(&vtable);
}

static void
HNSWDoSearch(Relation index, HNSWOptions *opts, int ef, float *queryVec,
  HNSWGlobalId nearest, float dNearest, int level, PriorityQueue *result,
  HNSWVtable *vtable) {
  int32 beginOffset, endOffset;
  float dis;
  HNSWNeighborTuple* tup;
  PaseTuple* tup1;
  PasePageList* pageList;
  HNSWPriorityQueueNode *node, *node1, *node2;
  HNSWPriorityQueueNode *top;
  bool type = false; // nearest --> farthest 
  PriorityQueue* candidates = PriorityQueueAllocate(HNSWPriorityQueueCmp, &type);

  ADD_HNSW_PQ_NODE(candidates, nearest, dNearest);
  ADD_HNSW_PQ_NODE(result, nearest, dNearest);
  HVTSet(vtable, nearest);
  while(!PriorityQueueIsEmpty(candidates)) {
    node1 = (HNSWPriorityQueueNode*)PriorityQueuePop(candidates);
    node2 = (HNSWPriorityQueueNode*)PriorityQueueFirst(result);
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
    pageList = InitPasePageListByNo(index,
      node1->gid.nblkid, sizeof(HNSWNeighborTuple),
      sizeof(HNSWNeighborPageOpaqueData), endOffset);
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
      dis = Distance(index, opts, queryVec, tup->gid);
      top = (HNSWPriorityQueueNode*)PriorityQueueFirst(result);
      if (top->distance > dis || PriorityQueueSize(result) < ef) {
        ADD_HNSW_PQ_NODE(candidates, tup->gid, dis); 
        ADD_HNSW_PQ_NODE(result, tup->gid, dis);

        if (PriorityQueueSize(result) > ef) {
            node = (HNSWPriorityQueueNode*)PriorityQueuePop(result);
            pfree(node);
        }
      }
    }
    pfree(pageList);
  }
  PriorityQueueFree(candidates);
}
