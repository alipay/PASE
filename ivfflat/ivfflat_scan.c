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

#include <pthread.h>
#include <omp.h>
#include "access/relscan.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/vector_util.h"
#include "ivfflat.h"

static const bool item_reverse = false;

static int
PairingHeapItemCompare(const pairingheap_node *a, const pairingheap_node *b,
    void *arg) {
  const InvertedListSearchItem *ia = (const InvertedListSearchItem *)a;
  const InvertedListSearchItem *ib = (const InvertedListSearchItem *)b;
  bool *reverse = (bool*) arg;
  if (ia->distance > ib->distance) {
    if (*reverse) {
      return 1;
    } else {
      return -1;
    }
  }
  else if (ia->distance < ib->distance) {
    if (*reverse) {
      return -1;
    } else {
      return 1;
    }
  } else {
    return 0;
  }
}

// Begin scan of ivfflat index.
IndexScanDesc
ivfflat_beginscan(Relation r, int nkeys, int norderbys) {
  IndexScanDesc     scan;
  IvfflatScanOpaque so;
  MemoryContext     scanCxt, oldCtx;

  scan = RelationGetIndexScan(r, nkeys, norderbys);
  scanCxt = AllocSetContextCreate(CurrentMemoryContext,
      "ivfflat scan context",
      ALLOCSET_DEFAULT_SIZES);
  oldCtx = MemoryContextSwitchTo(scanCxt);

  so = (IvfflatScanOpaque) palloc0(sizeof(IvfflatScanOpaqueData));
  InitIvfflatState(&so->state, scan->indexRelation);
  so->scan_pase = NULL;
  so->queue = NULL;
  so->first_call = true;
  so->scan_ctx = scanCxt;
  so->queue = pairingheap_allocate(PairingHeapItemCompare, (void*)&item_reverse);

  scan->opaque = so;

  if (scan->numberOfOrderBys > 0) {
    scan->xs_orderbyvals = palloc0(sizeof(Datum) * scan->numberOfOrderBys);
    scan->xs_orderbynulls = palloc(sizeof(bool) * scan->numberOfOrderBys);
    memset(scan->xs_orderbynulls, true, sizeof(bool) * scan->numberOfOrderBys);
  }

  MemoryContextSwitchTo(oldCtx);
  return scan;
}

// Rescan a ivfflat index.
void
ivfflat_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
    ScanKey orderbys, int norderbys) {
  MemoryContext          oldCtx;
  IvfflatScanOpaque      so;
  InvertedListSearchItem *item;

  so = (IvfflatScanOpaque) scan->opaque;
  oldCtx = MemoryContextSwitchTo(so->scan_ctx);

  if (so->queue != NULL) {
    if (!pairingheap_is_empty(so->queue)) {
      item = (InvertedListSearchItem *) pairingheap_remove_first(so->queue);
      pfree(item);
    }
    pairingheap_free(so->queue);
    so->queue = NULL;
  }
  so->scan_pase = NULL;
  so->queue = pairingheap_allocate(PairingHeapItemCompare, (void*)&item_reverse);
  so->first_call = true;

  if (scankey && scan->numberOfKeys > 0) {
    memmove(scan->keyData, scankey,
        scan->numberOfKeys * sizeof(ScanKeyData));
  }
  if (orderbys && scan->numberOfOrderBys > 0) {
    memmove(scan->orderByData, orderbys,
        scan->numberOfOrderBys * sizeof(ScanKeyData));
  }
  MemoryContextSwitchTo(oldCtx);
}

// End scan of ivfflat index.
void
ivfflat_endscan(IndexScanDesc scan) {
  IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
  MemoryContextDelete(so->scan_ctx);
}

static void
ScanInvertedListAndCalDistance(Relation index, IvfflatMetaPageData *meta,
    IvfflatState *state, BlockNumber headBlkno,
    float4 *queryVec, pairingheap *queue, pthread_mutex_t *mutex) {
  BlockNumber            blkno;
  Buffer                 buffer;
  Page                   page;
  IvfflatPageOpaque      opaque;
  InvertedListTuple      *itup;
  int                    i;
  float                  dis;
  InvertedListSearchItem *item;

  blkno = headBlkno;
  for (;;) {
    // to the end of inverted list
    if (blkno == 0) {
      break;
    }

    buffer = ReadBuffer(index, blkno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);
    opaque = IvfflatPageGetOpaque(page);

    for (i = 0; i < opaque->maxoff; ++i) {
      itup = InvertedListPageGetTuple(state, page, i + 1); 
      dis = fvec_L2sqr(queryVec, itup->vector, meta->opts.dimension); 
      if (mutex) {
        pthread_mutex_lock(mutex);
      }
      item = (InvertedListSearchItem *) palloc0(
          sizeof(InvertedListSearchItem));
      item->heap_ptr = itup->heap_ptr;
      item->distance = dis;
      pairingheap_add(queue, &item->ph_node);
      if (mutex) {
        pthread_mutex_unlock(mutex);
      }
    }
    UnlockReleaseBuffer(buffer); 
    blkno = opaque->next;
  }
}

// ivfflat_gettuple() -- Get the next tuple in the scan
bool
ivfflat_gettuple(IndexScanDesc scan, ScanDirection dir) {
  IvfflatScanOpaque      so;
  MemoryContext          oldCtx;
  bool                   reverse;
  IvfflatMetaPageData    *meta;
  Buffer		         metaBuffer;
  uint32                 scanRatio;
  uint32                 scanCentroidNum;
  InvertedListSearchItem *item;
  CentroidSearchItem     *citems;
  int                    i;

  if (dir != ForwardScanDirection) {
    elog(WARNING, "ivfflat only supports forward scan direction");
    return false;
  }

  reverse = false;
  so = (IvfflatScanOpaque) scan->opaque;
  if (!scan->orderByData) {
    elog(WARNING, "orderByData is invalid");
    return false;
  }
  if (!scan->orderByData->sk_argument) {
    elog(WARNING, "orderBy value is invalid");
    return false;
  }
  oldCtx = MemoryContextSwitchTo(so->scan_ctx);

  if (so->first_call) {
    so->scan_pase = DatumGetPASE(scan->orderByData->sk_argument);

    // get info from meta
    metaBuffer = ReadBuffer(scan->indexRelation, IVFFLAT_METAPAGE_BLKNO);
    LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
    meta = IvfflatPageGetMeta(BufferGetPage(metaBuffer));

    if (meta->centroid_num == 0) {
      elog(WARNING, "centroid count is 0");
      MemoryContextSwitchTo(oldCtx);
      UnlockReleaseBuffer(metaBuffer);
      return false;
    }

    if (PASE_DIM(so->scan_pase) != meta->opts.dimension) {
      elog(WARNING, "query dimension(%u) not equal to data dimension(%u)",
          PASE_DIM(so->scan_pase), meta->opts.dimension);
      MemoryContextSwitchTo(oldCtx);
      UnlockReleaseBuffer(metaBuffer);
      return false;
    }

    scanRatio = PASE_EXTRA(so->scan_pase);
    if (scanRatio > MAX_SCAN_RATION) {
      elog(WARNING, "scanRatio[%u] is illegal, should in (0, 1000]", scanRatio);
      MemoryContextSwitchTo(oldCtx);
      UnlockReleaseBuffer(metaBuffer);
      return false;
    }
    if (scanRatio == 0) {
      scanRatio = DEFAULT_SCAN_RATIO;
    }
    scanCentroidNum = (scanRatio * meta->centroid_num) / MAX_SCAN_RATION;
    // scan one inverted list at least
    if (scanCentroidNum == 0) {
      scanCentroidNum = 1;
    }
    citems = (CentroidSearchItem*) palloc0(sizeof(CentroidSearchItem) * scanCentroidNum);

    scan->xs_recheck = false;
    scan->xs_recheckorderby = false;
    SearchKNNInvertedListFromCentroidPages(scan->indexRelation,
        &so->state, meta, so->scan_pase->x, scanCentroidNum,
        reverse, citems, true);
    if (meta->opts.open_omp) {
      pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
      omp_set_num_threads(meta->opts.omp_thread_num);
#pragma omp for 
      for (i = 0; i < scanCentroidNum; ++i) {
        if (citems[i].cblkno == 0) {
          continue;
        }
        // inverted list is empty
        if (citems[i].head_ivl_blkno == 0) {
          continue;
        }
        ScanInvertedListAndCalDistance(scan->indexRelation, meta,
            &so->state, citems[i].head_ivl_blkno,
            so->scan_pase->x, so->queue, &mutex);
      }
      pthread_mutex_destroy(&mutex);
    } else {
      for (i = 0; i < scanCentroidNum; ++i) {
        if (citems[i].cblkno == 0) {
          continue;
        }
        // inverted list is empty
        if (citems[i].head_ivl_blkno == 0) {
          continue;
        }
        ScanInvertedListAndCalDistance(scan->indexRelation, meta,
            &so->state, citems[i].head_ivl_blkno,
            so->scan_pase->x, so->queue, (pthread_mutex_t *)NULL);
      }
    }
    if (!pairingheap_is_empty(so->queue)) {
      item = (InvertedListSearchItem*) pairingheap_remove_first(
          so->queue);
      scan->xs_ctup.t_self = item->heap_ptr;
      if (scan->numberOfOrderBys > 0) {
        scan->xs_orderbyvals[0] = Float4GetDatum(item->distance);
        scan->xs_orderbynulls[0] = false;
      }
      pfree(item);
    }
    so->first_call = false;
    pfree(citems);
    UnlockReleaseBuffer(metaBuffer);
  }
  else {
    if (!pairingheap_is_empty(so->queue)) {
      item = (InvertedListSearchItem*) pairingheap_remove_first(
          so->queue);
      scan->xs_ctup.t_self = item->heap_ptr;
      if (scan->numberOfOrderBys > 0) {
        scan->xs_orderbyvals[0] = Float4GetDatum(item->distance);
        scan->xs_orderbynulls[0] = false;
      }
      pfree(item);
    }
    else {
      elog(WARNING, "not enough data to pop for queue"); 
      MemoryContextSwitchTo(oldCtx);
      return false;
    }
  }
  MemoryContextSwitchTo(oldCtx);
  return true;
}

// ivfflat_gettuple() -- Get the next tuple in the scan
int64
ivfflat_getbitmap(IndexScanDesc scan, TIDBitmap *tbm) {
  elog(NOTICE, "ivfflat_getbitmap begin");
  return 0;
}
