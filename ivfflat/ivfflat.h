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
// Impaseentation of ivfflat index
//
#ifndef PASE_IVFFLAT_IVFFLAT_H_
#define PASE_IVFFLAT_IVFFLAT_H_

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "access/reloptions.h"
#include "nodes/relation.h"
#include "catalog/index.h"
#include "lib/pairingheap.h"
#include "fmgr.h"
#include "type/pase_data.h"
#include "pase.h"

////////////////////////////////////////////////////////////////////////////////
// Opaque for centroid and inverted list pages
typedef struct IvfflatPageOpaqueData {
  OffsetNumber maxoff;		// number of index tuples on page
  uint16      flags;      // see bit definitions below
  BlockNumber next;       // refer to next centroid page block
} IvfflatPageOpaqueData;

typedef IvfflatPageOpaqueData *IvfflatPageOpaque;

// ivfflat index options
typedef struct IvfflatOptions {
  int32  vl_len_;                   // varlena header (do not touch directly!)
  int    clustering_type;           // clustering type:0 centroid_file, 1 inner clustering
  int	   distance_type;	            // distance metric type:0 l2, 1 inner proudct, 2 cosine
  int    dimension;                 // vector dimension
  int    open_omp;                  // whether open omp, 0:close, 1:open
  int    omp_thread_num;            // omp thread number
  int    base64_encoded;            // data whether base64 encoded
  int	   clustering_params_offset;  // clustering parameters offset
} IvfflatOptions;

// Metadata of ivfflat index
typedef struct IvfflatMetaPageData
{
  uint32		  magick_number;
  uint32      centroid_num;
  BlockNumber centroid_head_blkno;
  BlockNumber centroid_page_count;
  IvfflatOptions opts;
} IvfflatMetaPageData;

typedef struct IvfflatState {
  IvfflatOptions opts;			// copy of options on index's metapage
  int32          nColumns;
  Size           size_of_centroid_tuple;
  Size           size_of_invertedlist_tuple;
} IvfflatState;

// Tuple for centroid
typedef struct CentroidTuple {
  BlockNumber     head_ivl_blkno;
  uint32          inverted_list_size;
  float4          vector[FLEXIBLE_ARRAY_MEMBER];
} CentroidTuple;

// Tuple for inverted list
typedef struct InvertedListTuple {
  ItemPointerData heap_ptr;
  uint8           is_deleted;
  float4          vector[FLEXIBLE_ARRAY_MEMBER];
} InvertedListTuple;

// centroid data
typedef struct CentroidsData {
  int dim;
  int count;
  CentroidTuple *ctups;
} CentroidsData;

typedef CentroidsData *Centroids;

typedef struct CentroidSearchItem {
  pairingheap_node ph_node;
  BlockNumber cblkno;
  OffsetNumber offset;
  BlockNumber head_ivl_blkno;
  float distance;
} CentroidSearchItem;

typedef struct InvertedListSearchItem {
  pairingheap_node ph_node;
  ItemPointerData heap_ptr;
  float distance;
} InvertedListSearchItem;

// Opaque data structure for ivfflat index scan
typedef struct IvfflatScanOpaqueData {
  PASE *scan_pase;
  MemoryContext scan_ctx;
  pairingheap *queue;
  IvfflatState state; 
  bool first_call;
} IvfflatScanOpaqueData;

typedef IvfflatScanOpaqueData *IvfflatScanOpaque;

////////////////////////////////////////////////////////////////////////////////
// ivfflat page flags
#define IVFFLAT_META		(1<<0)
#define IVFFLAT_DELETED	    (2<<0)

// build initializing memory size
#define IVFFLAT_BUILD_INIT_MEM_SIZE 500 * 1024 * 1024
#define MAX_CLUSTERING_MEM          300 * 1024 * 1024
#define MAX_CLUSTERING_SAMPLE_COUNT 1000000
#define DEFAULT_SCAN_RATIO          20
#define MAX_SCAN_RATION             1000
#define MAX_CLUSTERING_SAMPLE_RATIO 1000

// Macros for accessing ivfflat page structures
#define IvfflatPageGetOpaque(_page) ((IvfflatPageOpaque) PageGetSpecialPointer(_page))
#define IvfflatPageGetMaxOffset(_page) (IvfflatPageGetOpaque(_page)->maxoff)
#define IvfflatPageIsMeta(_page) \
  ((IvfflatPageGetOpaque(_page)->flags & IVFFLAT_META) != 0)
#define IvfflatPageIsDeleted(_page) \
  ((IvfflatPageGetOpaque(_page)->flags & IVFFLAT_DELETED) != 0)
#define IvfflatPageSetDeleted(_page) \
  (IvfflatPageGetOpaque(_page)->flags |= IVFFLAT_DELETED)
#define IvfflatPageSetNonDeleted(_page) \
  (IvfflatPageGetOpaque(_page)->flags &= ~IVFFLAT_DELETED)
#define CentroidPageGetData(_page)		((CentroidTuple *)PageGetContents(_page))
#define CentroidPageGetTuple(_state, _page, _offset) \
  ((CentroidTuple *)(PageGetContents(_page) \
    + (_state)->size_of_centroid_tuple * ((_offset) - 1)))
#define CentoridPageGetNextTuple(_state, _tuple) \
  ((CentroidTuple *)((Pointer)(_tuple) + (_state)->size_of_centroid_tuple))
#define CentroidTuplesGetTuple(_buildState, _offset) \
  ((CentroidTuple *)((char*)_buildState->centroids.ctups + \
    _offset * (_buildState->ivf_state.size_of_centroid_tuple)))
#define InvertedListPageGetData(_page)	((InvertedListTuple *)PageGetContents(_page))
#define InvertedListPageGetTuple(_state, _page, _offset) \
  ((InvertedListTuple *)(PageGetContents(_page) \
    + (_state)->size_of_invertedlist_tuple * ((_offset) - 1)))
#define InvertedListPageGetNextTuple(_state, _tuple) \
  ((InvertedListTuple *)((Pointer)(_tuple) + (_state)->size_of_invertedlist_tuple))
#define IvfflatPageGetMeta(_page) ((IvfflatMetaPageData *) PageGetContents(_page))

// Preserved page numbers
#define IVFFLAT_METAPAGE_BLKNO	(0)
#define IVFFLAT_HEAD_BLKNO		(1) // first data page

// Default and maximum Ivfflat centroid file path length.
#define MAX_CENTROID_PATH_LEN   256
#define DEFAULT_DIMENSION       256

// Magic number to distinguish ivfflat pages among anothers
#define IVFFLAT_MAGICK_NUMBER (0xDBAC0DEE)

#define CentroidPageGetFreeSpace(_state, _page) \
  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
   - IvfflatPageGetMaxOffset(_page) * (_state)->size_of_centroid_tuple \
   - MAXALIGN(sizeof(IvfflatPageOpaqueData)))
#define InvertedListPageGetFreeSpace(_state, _page) \
  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) \
   - IvfflatPageGetMaxOffset(_page) * (_state)->size_of_invertedlist_tuple \
   - MAXALIGN(sizeof(IvfflatPageOpaqueData)))

#define CENTROIDTUPLEHDRSZ offsetof(CentroidTuple, vector)
#define INVERTEDLISTTUPLEHDRSZ offsetof(InvertedListTuple, vector)

////////////////////////////////////////////////////////////////////////////////
// ivfflat_utils.c
extern void _PG_init(void);
extern void InitIvfflatState(IvfflatState *state, Relation index);
extern void IvfflatFillMetapage(Relation index, Page metaPage);
extern void IvfflatInitMetapage(Relation index);
extern void IvfflatInitPage(Page page, uint16 flags);
extern Buffer IvfflatNewBuffer(Relation index, bool needLock);
extern bool IvfflatPageAddItem(IvfflatState *state, Page page,
    InvertedListTuple *tuple);
extern void FlushBufferPage(Relation index, Buffer buffer, bool needUnLock);
extern bytea *ivfflat_options(Datum reloptions, bool validate);
extern float SearchNNFromCentroids(IvfflatState *state, InvertedListTuple *tuple,
    Centroids centroids, int *minPos);
extern int PairingHeapCentroidCompare(const pairingheap_node *a,
    const pairingheap_node *b, void *arg);
extern void SearchKNNInvertedListFromCentroidPages(
    Relation index, IvfflatState *state,
    IvfflatMetaPageData *meta, float4 *tuple_vector,
    int count, bool reverse, CentroidSearchItem *items, bool isScan);

// ivfflat_build.c
extern IndexBuildResult *ivfflat_build(Relation heap, Relation index, IndexInfo *indexInfo);
extern bool ivfflat_insert(Relation index, Datum *values, bool *isnull,
    ItemPointer ht_ctid, Relation heapRel,
    IndexUniqueCheck checkUnique, IndexInfo *indexInfo);
extern void ivfflat_buildempty(Relation index);

// ivfflat_vacuum.c
extern IndexBulkDeleteResult *ivfflat_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
    IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *ivfflat_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

// ivfflat_scan.c
extern IndexScanDesc ivfflat_beginscan(Relation r, int nkeys, int norderbys);
extern void ivfflat_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
    ScanKey orderbys, int norderbys);
extern void ivfflat_endscan(IndexScanDesc scan);
extern bool ivfflat_gettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 ivfflat_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);

// ivfflat_cost.c
extern void
ivfflat_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
    Cost *indexStartupCost, Cost *indexTotalCost,
    Selectivity *indexSelectivity, double *indexCorrelation,
    double *indexPages);

// pase_handler.c
extern relopt_kind ivfflat_relopt_kind;

#endif  // PASE_IVFFLAT_IVFFLAT_H_
