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

#include <stdio.h>
#include <float.h>
#include <omp.h>
#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "catalog/index.h"
#include "storage/lmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/memutils.h"
#include "access/reloptions.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "lib/pairingheap.h"

#include "utils/string_util.h"
#include "utils/vector_util.h"
#include "ivfflat.h"

// Construct a default set of Bloom options.
static IvfflatOptions *
makeDefaultIvfflatOptions(void) {
  IvfflatOptions *opts;

  opts = (IvfflatOptions *) palloc0(sizeof(IvfflatOptions));
  opts->distance_type = 0;
  opts->dimension = 256;
  SET_VARSIZE(opts, sizeof(IvfflatOptions));
  return opts;
}

// Fill IvfflatState structure for particular index.
void
InitIvfflatState(IvfflatState *state, Relation index) {
  state->nColumns = index->rd_att->natts;

  // Initialize amcache if needed with options from metapage
  if (!index->rd_amcache)
  {
    Buffer		buffer;
    Page		page;
    IvfflatMetaPageData *meta;
    IvfflatOptions *opts;

    opts = MemoryContextAlloc(index->rd_indexcxt, sizeof(IvfflatOptions));

    buffer = ReadBuffer(index, IVFFLAT_METAPAGE_BLKNO);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);

    page = BufferGetPage(buffer);

    if (!IvfflatPageIsMeta(page))
      elog(ERROR, "Relation is not a pase ivfflat index");
    meta = IvfflatPageGetMeta(BufferGetPage(buffer));

    if (meta->magick_number != IVFFLAT_MAGICK_NUMBER)
      elog(ERROR, "Relation is not a pase ivfflat index");

    *opts = meta->opts;
    UnlockReleaseBuffer(buffer);
    index->rd_amcache = (void *) opts;
  }

  memcpy(&state->opts, index->rd_amcache, sizeof(state->opts));
  state->size_of_centroid_tuple = CENTROIDTUPLEHDRSZ +
    sizeof(float4) * state->opts.dimension;
  state->size_of_invertedlist_tuple = INVERTEDLISTTUPLEHDRSZ +
    sizeof(float4) * state->opts.dimension;
}

float
SearchNNFromCentroids(IvfflatState *state, InvertedListTuple *tuple,
    Centroids centroids, int *minPos) {
  // TODO(yangwen.yw): omp for
  float minDistance;
  CentroidTuple *ctup;
  int i;
  float dis;

  minDistance = FLT_MAX;
  *minPos = centroids->count;
  for (i = 0; i < centroids->count; ++i) {
    // TODO(yangwen.yw): support other metric type
    if (state->opts.distance_type == 0) {
      ctup = (CentroidTuple *)((char*)centroids->ctups + i * state->size_of_centroid_tuple);
      dis = fvec_L2sqr(tuple->vector, ctup->vector,
          centroids->dim);
      if (dis < minDistance) {
        minDistance = dis;
        *minPos = i;
      }
    }
  }
  return minDistance;
}

int
PairingHeapCentroidCompare(const pairingheap_node *a,
    const pairingheap_node *b, void *arg) {
  const CentroidSearchItem *ca = (const CentroidSearchItem *)a;
  const CentroidSearchItem *cb = (const CentroidSearchItem *)b;
  bool *reverse = (bool*) arg;
  if (ca->distance > cb->distance) {
    if (*reverse)
      return 1;
    else
      return -1;
  }
  else if (ca->distance < cb->distance) {
    if (*reverse)
      return -1;
    else
      return 1;
  }
  else
    return 0;
}

void
SearchKNNInvertedListFromCentroidPages(Relation index, IvfflatState *state,
    IvfflatMetaPageData *meta, float4 *tuple_vector,
    int count, bool reverse, CentroidSearchItem *items,
    bool isScan) {
  // TODO(yangwen.yw): omp for
  BlockNumber cblkno;
  Buffer cbuffer;
  Page cpage;
  CentroidTuple *ctup;
  BufferAccessStrategy bas;
  pairingheap *queue;
  CentroidSearchItem *item;
  int i;
  bas = GetAccessStrategy(BAS_BULKREAD);

  cblkno = meta->centroid_head_blkno;
  queue = pairingheap_allocate(PairingHeapCentroidCompare, &reverse);
  for (; cblkno < meta->centroid_head_blkno + meta->centroid_page_count; ++cblkno) {
    cbuffer = ReadBufferExtended(index, MAIN_FORKNUM, cblkno,
        RBM_NORMAL, bas);
    LockBuffer(cbuffer, BUFFER_LOCK_SHARE);
    cpage = BufferGetPage(cbuffer); 
    if (!PageIsNew(cpage) && !IvfflatPageIsDeleted(cpage)) {
      OffsetNumber offset,
                   maxOffset = IvfflatPageGetMaxOffset(cpage);
      for (offset = 1; offset <= maxOffset; ++offset) {
        ctup = CentroidPageGetTuple(state, cpage, offset);
        if (isScan && ctup->head_ivl_blkno == 0)
          continue;
        // TODO(yangwen.yw): support other metric type
        if (state->opts.distance_type == 0) {
          float dis = fvec_L2sqr(tuple_vector, ctup->vector,
              meta->opts.dimension);
          item = (CentroidSearchItem *) palloc0(
              sizeof(CentroidSearchItem));
          item->cblkno = cblkno;
          item->offset = offset;
          item->head_ivl_blkno = ctup->head_ivl_blkno;
          item->distance = dis;
          pairingheap_add(queue, &item->ph_node);
        }
      }
    }
    UnlockReleaseBuffer(cbuffer);
  }

  for (i = 0; i < count; ++i) {
    if (!pairingheap_is_empty(queue)) {
      item = (CentroidSearchItem*) pairingheap_remove_first(queue);
      items[i] = *item;
      pfree(item);
    }
  }
  for(;;) {
    if (!pairingheap_is_empty(queue)) {
      item = (CentroidSearchItem*) pairingheap_remove_first(queue);
      pfree(item);
    }
    else
      break;
  }
  pairingheap_free(queue);
  FreeAccessStrategy(bas);
}

void FlushBufferPage(Relation index, Buffer buffer, bool needUnLock) {
  GenericXLogState *state;

  if (!needUnLock)
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
  state = GenericXLogStart(index);
  GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
  GenericXLogFinish(state);
  UnlockReleaseBuffer(buffer);
}

// Allocate a new page (either by recycling, or by extending the index file)
// The returned buffer is already pinned and exclusive-locked when used in
// inserting, but not exclusive-locked in building
// Caller is responsible for initializing the page by calling IvfflatInitBuffer
Buffer
IvfflatNewBuffer(Relation index, bool needLock) {
  Buffer		buffer;
  bool		inNeedLock;

  /// First, try to get a page from FSM
  for (;;) {
    BlockNumber blkno = GetFreeIndexPage(index);

    if (blkno == InvalidBlockNumber)
      break;

    buffer = ReadBuffer(index, blkno);

    // We have to guard against the possibility that someone else already
    // recycled this page; the buffer may be locked if so.
    if (ConditionalLockBuffer(buffer)) {
      Page		page = BufferGetPage(buffer);

      if (PageIsNew(page))
        return buffer;	// OK to use, if never initialized

      if (IvfflatPageIsDeleted(page))
        return buffer;	// OK to use

      LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    }

    // Can't use it, so release buffer and try again
    ReleaseBuffer(buffer);
  }

  // Must extend the file
  inNeedLock = !RELATION_IS_LOCAL(index);
  if (inNeedLock)
    LockRelationForExtension(index, ExclusiveLock);

  buffer = ReadBuffer(index, P_NEW);
  if (needLock)
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

  if (inNeedLock)
    UnlockRelationForExtension(index, ExclusiveLock);

  return buffer;
}

// Initialize any page of a ivfflat index.
void
IvfflatInitPage(Page page, uint16 flags) {
  IvfflatPageOpaque opaque;
  PageInit(page, BLCKSZ, sizeof(IvfflatPageOpaqueData));
  opaque = IvfflatPageGetOpaque(page);
  memset(opaque, 0, sizeof(IvfflatPageOpaqueData));
  opaque->flags = flags;
}

// Fill in metapage for ivfflat index.
void
IvfflatFillMetapage(Relation index, Page metaPage) {
  IvfflatOptions *opts;
  IvfflatMetaPageData *metadata;

  // Choose the index's options.  If reloptions have been assigned, use
  // those, otherwise create default options.
  opts = (IvfflatOptions *) index->rd_options;
  if (!opts)
    opts = makeDefaultIvfflatOptions();

  // Initialize contents of meta page, including a copy of the options,
  // which are now frozen for the life of the index.
  IvfflatInitPage(metaPage, IVFFLAT_META);
  metadata = IvfflatPageGetMeta(metaPage);
  memset(metadata, 0, sizeof(IvfflatMetaPageData));
  metadata->magick_number = IVFFLAT_MAGICK_NUMBER;
  metadata->opts = *opts;
  ((PageHeader) metaPage)->pd_lower += sizeof(IvfflatMetaPageData);
  Assert(((PageHeader) metaPage)->pd_lower <= ((PageHeader) metaPage)->pd_upper);
}

// Initialize metapage for ivfflat index.
void
IvfflatInitMetapage(Relation index)
{
  Buffer      metaBuffer;
  Page        metaPage;
  GenericXLogState *state;

  // Make a new page; since it is first page it should be associated with
  // block number 0 (IVFFLAT_METAPAGE_BLKNO).
  metaBuffer = IvfflatNewBuffer(index, true);
  Assert(BufferGetBlockNumber(metaBuffer) == IVFFLAT_METAPAGE_BLKNO);

  // Initialize contents of meta page
  state = GenericXLogStart(index);
  metaPage = GenericXLogRegisterBuffer(state, metaBuffer,
      GENERIC_XLOG_FULL_IMAGE);
  IvfflatFillMetapage(index, metaPage);
  GenericXLogFinish(state);

  UnlockReleaseBuffer(metaBuffer);
}

// Parse reloptions for ivfflat index, producing a IvfflatOptions struct.
bytea *
ivfflat_options(Datum reloptions, bool validate) {
  relopt_value *options;
  int         numoptions;
  IvfflatOptions *rdopts;

  static const relopt_parse_elt ivfflat_relopt_tab[] = {
    {"clustering_type", RELOPT_TYPE_INT, offsetof(IvfflatOptions, clustering_type)},
    {"distance_type", RELOPT_TYPE_INT, offsetof(IvfflatOptions, distance_type)},
    {"dimension", RELOPT_TYPE_INT, offsetof(IvfflatOptions, dimension)},
    {"open_omp", RELOPT_TYPE_INT, offsetof(IvfflatOptions, open_omp)},
    {"omp_thread_num", RELOPT_TYPE_INT, offsetof(IvfflatOptions, omp_thread_num)},
    {"base64_encoded", RELOPT_TYPE_INT, offsetof(IvfflatOptions, base64_encoded)},
    {"clustering_params", RELOPT_TYPE_STRING, offsetof(IvfflatOptions, clustering_params_offset)}
  };

  // Parse the user-given reloptions
  options = parseRelOptions(reloptions, validate, ivfflat_relopt_kind, &numoptions);
  if (numoptions < 4) {
    elog(ERROR, "options format error");
  }
  rdopts = allocateReloptStruct(sizeof(IvfflatOptions), options, numoptions);
  fillRelOptions((void *) rdopts, sizeof(IvfflatOptions), options, numoptions,
      validate, ivfflat_relopt_tab, lengthof(ivfflat_relopt_tab));

  if (rdopts->open_omp) {
    elog(NOTICE, "using openmp to speed up");
    if (rdopts->omp_thread_num == 0) {
      elog(NOTICE, "not set omp thread number, set core number[%d] to it",
          omp_get_num_procs());
      omp_set_num_threads(omp_get_num_procs());
    } else {
      elog(NOTICE, "set omp thread number[%d]", rdopts->omp_thread_num);
      omp_set_num_threads(rdopts->omp_thread_num);
    }
  }

  pfree(options);
  return (bytea *) rdopts;
}
