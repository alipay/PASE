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
// This impaseentation is heavily influenced by the NMSlib and Faiss.
// https://github.com/searchivarius/nmslib
// https://github.com/facebookresearch/faiss.git

#ifndef PASE_HNSW_HNSW_H_
#define PASE_HNSW_HNSW_H_

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "lib/pairingheap.h"
#include "nodes/relation.h"

#include "utils/pase_hash_table.h"
#include "utils/pase_page_list.h"
#include "utils/priority_queue.h"
#include "type/pase_data.h"

#define MAX_HNSW_LEVEL 100
#define HNSW_METAPAGE_BLKNO 0
#define HNSW_MAGICK_NUMBER (0xDBAC0EEE)

typedef struct HNSWGlobalId {
  int32 nblkid;
  int32 dblkid;
  int32 doffset;
} HNSWGlobalId;

typedef struct HNSWOptions {
  int32 vl_len_;
  int32 dim;
  int32 base_nb_num;
  int32 real_max_level;
  int32 ef_build;
  int32 ef_search;
  int32 base64_encoded;
  uint16 nb_tup_size;
  uint16 data_tup_size;
  // cumulative neighbor num on all lower level
  uint16 cum_nn_per_level[MAX_HNSW_LEVEL + 1];
  float4 assign_probas[MAX_HNSW_LEVEL];
} HNSWOptions;

typedef struct HNSWBuildState {
  HNSWOptions opts;
  MemoryContext tmpctx;
  uint16 count;
  int32 pre_data_blkid;
  int32 data_entry_blkid;
  int32 cur_max_level;
  HNSWGlobalId entry_gid;
  int64 indtuples;
  PGAlignedBlock data;
} HNSWBuildState;

typedef struct HNSWDataPageOpaqueData {
  int32 maxoff;
  int32 next_blkid;
} HNSWDataPageOpaqueData;
typedef HNSWDataPageOpaqueData *HNSWDataPageOpaque;

typedef struct HNSWDataTuple {
  PaseTuple tag;  // for PaseTupleList
  ItemPointerData heap_ptr;
  uint16 level;
  float4 vector[FLEXIBLE_ARRAY_MEMBER];
} HNSWDataTuple;

typedef struct HNSWPriorityQueueNode {
  PriorityQueueNode pr_node;  // for PriorityQueue
  HNSWGlobalId gid;
  double distance;
} HNSWPriorityQueueNode;

typedef struct HNSWNeighborPageOpaqueData {
  int32 maxoff;
  int32 next_blkid;
} HNSWNeighborPageOpaqueData;
typedef HNSWNeighborPageOpaqueData *HNSWNeighborPageOpaque;

typedef struct HNSWNeighborTuple {
  PaseTuple tag;  // for PaseTupleList
  HNSWGlobalId gid;
} HNSWNeighborTuple;

typedef struct HNSWMetaPageData {
  uint32 magick_num;
  HNSWGlobalId entry_gid;
  // real data page blkid
  int32 last_data_blkid;
  HNSWOptions opts;
} HNSWMetaPageData;
typedef HNSWMetaPageData* HNSWMetaPage;

typedef struct HNSWScanOpaqueData {
    PASE*  scan_pase;
    MemoryContext scan_ctx;
    PriorityQueue* queue;
    uint16 data_tup_size;
    bool first_call;
} HNSWScanOpaqueData;
typedef HNSWScanOpaqueData* HNSWScanOpaque;

// hnsw_build.c
extern IndexBuildResult *hnsw_build(Relation heap, Relation index,
    struct IndexInfo *indexInfo);
extern void hnsw_buildempty(Relation index);
extern void hnsw_costestimate(PlannerInfo *root, IndexPath *path,
     double loopCount, Cost *indexStartupCost,
     Cost *indexTotalCost, Selectivity *indexSelectivity,
     double *indexCorrelation, double *indexPages);

// hnsw_scan.c
extern IndexScanDesc hnsw_beginscan(Relation r, int nkeys, int norderbys);
extern void hnsw_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
     ScanKey orderbys, int norderbys);
extern bool hnsw_gettuple(IndexScanDesc scan, ScanDirection dir);
extern void hnsw_endscan(IndexScanDesc scan);

// hnsw_insert.c
extern bool hnsw_insert(Relation index, Datum *values, bool *isnull,
     ItemPointer htCtid, Relation heapRel,
     IndexUniqueCheck checkUnique,
     struct IndexInfo *indexInfo);
extern void
InitHNSWBuildState4Insert(HNSWMetaPageData *meta, HNSWBuildState *state);
extern void
HNSWFillMetaPage(Relation index, HNSWBuildState *state);

////////////////////////////////////////////////
// HNSWVtable gid wraper
typedef struct HNSWVtable {
  PaseHashTable *vt1;
  PaseHashTable *vt2;
} HNSWVtable;

extern void HVTInit(MemoryContext ctx, HNSWVtable *vt);
extern void HVTReset(HNSWVtable *vt);
extern void HVTSet(HNSWVtable *vt, HNSWGlobalId gid);
extern bool HVTGet(HNSWVtable *vt, HNSWGlobalId gid);
extern void HVTFree(HNSWVtable *vt);

////////////////////////////////////////////////

// data page macros
#define HNSWDATATUPLEHDRSZ offsetof(HNSWDataTuple, vector)
#define GetHNSWDataPageOpaque(_page)  \
  ((HNSWDataPageOpaque) PageGetSpecialPointer(_page))
#define HNSWDataPageGetMaxOffset(_page) (GetHNSWDataPageOpaque(_page)->maxoff)
#define HNSWDataPageGetFreeSpace(_state, _page)                      \
  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData)                           \
    - HNSWDataPageGetMaxOffset(_page) * (_state)->opts.data_tup_size \
    - MAXALIGN(sizeof(HNSWDataPageOpaqueData)))
#define HNSWDataPageGetTuple(_data_tup_size, _page, _offset)  \
  ((HNSWDataTuple *)(PageGetContents(_page)                   \
    + (_data_tup_size) * ((_offset) - 1)))

// neighbor page macros
#define HNSW_GID(_gid, _nbid, _dtid, _offset)          \
  do {                                                 \
    (_gid).nblkid = _nbid;                             \
    (_gid).dblkid = _dtid;                             \
    (_gid).doffset = _offset;                          \
  } while (0)                                          \

#define HNSW_CHECK_GID(_gid)  ((_gid).nblkid <= 0 && (_gid).dblkid <= 0 && (_gid).doffset <= 0)

#define HNSW_CHECK_GID_EQ(_gid1, _gid2)      \
  ((_gid1).nblkid == (_gid2).nblkid && (_gid1).dblkid == (_gid2).dblkid && (_gid1).doffset == (_gid2).doffset)

#define GetHNSWNeighborPageOpaque(_page) \
  ((HNSWNeighborPageOpaque) PageGetSpecialPointer(_page))
#define HNSWNeighborPageGetMaxOffset(_page) \
  (GetHNSWNeighborPageOpaque(_page)->maxoff)

#define HNSWNeighborPageGetTuple(_state, _page, _offset) \
  ((HNSWNeighborTuple *)(PageGetContents(_page)          \
    + (_state)->opts.nb_tup_size * ((_offset) - 1)))

#define HNSWNeighborPageGetFreeSpace(_state, _page)                    \
  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData)                             \
    - HNSWNeighborPageGetMaxOffset(_page) * (_state)->opts.nb_tup_size \
    - MAXALIGN(sizeof(HNSWNeighborPageOpaqueData)))

// init cache data or nb page
#define HNSW_INIT_CACHE_PAGE(_type, _state)                     \
  do {                                                          \
    _type opaque;                                               \
    memset((_state)->data.data, 0, BLCKSZ);                     \
    PageInit((_state)->data.data, BLCKSZ, sizeof(_type##Data)); \
    opaque = Get##_type((_state)->data.data);                   \
    memset(opaque, 0, sizeof(_type##Data));                     \
    opaque->next_blkid= (_state)->pre_data_blkid;               \
    (_state)->count = 0;                                        \
  } while (0)

#define ADD_HNSW_PQ_NODE(_queue, _gid, _distance)                            \
  do {                                                                       \
    HNSWPriorityQueueNode *node_xxx = palloc(sizeof(HNSWPriorityQueueNode)); \
    node_xxx->gid = (_gid);                                                  \
    node_xxx->distance = (_distance);                                        \
    PriorityQueueAdd(_queue, (PriorityQueueNode *)node_xxx);                 \
  } while (0)

// utils
int HNSWPriorityQueueCmp(const PriorityQueueNode *a,
    const PriorityQueueNode *b, void *arg);
// options
extern HNSWOptions *MakeDefaultHNSWOptions();
// flush cached page when it is full
extern int HNSWFlushCachedPage(Relation index, HNSWBuildState *buildState);
extern void HNSWInitMetapage(Relation index);
extern void InitHNSWBuildState(HNSWBuildState *state, Relation index);
// data page function
extern bool HNSWDataPageAddItem(HNSWBuildState *state, Page page,
    HNSWDataTuple *tuple);
extern HNSWDataTuple *HNSWFormDataTuple(HNSWOptions* opts,
    ItemPointer iptr, Datum *values, bool *isnull);
extern void FindItDataByOffset(Relation index, uint16 dataTupSize,
  HNSWGlobalId gid, ItemPointerData* itData); 
extern float Distance(Relation index, HNSWOptions *opts,
  const float4* vector, HNSWGlobalId gid);
// neighbor page function
extern PaseTuple* HNSWNeighborTupleFormer(void *arg); 
// hnsw build function
extern void HNSWBuildLink(Relation index, HNSWBuildState *state);
extern int GreedyUpdateNearest(Relation index, HNSWOptions *opts,
    int32 maxLevel, int level, HNSWGlobalId *nearest, float *dNearest, const float4* vector);
extern void AddLinkFromHighToLow(Relation index, HNSWBuildState *state,
    HNSWGlobalId nearest, float dNearest, int level, HNSWDataTuple* tup,
    HNSWGlobalId gid, HNSWVtable *vtable);
extern void ShrinkNbList(Relation index, HNSWBuildState *state, int level,
    PriorityQueue **linkTargets);
extern void SearchNbToAdd(Relation index, HNSWOptions* opts, int level,
    HNSWGlobalId nearest,float dNearest, PriorityQueue *results,
    HNSWVtable *vtable, HNSWDataTuple* dttup);
extern void FillNeighborPages(Relation index, PasePageList *pageList,
    PriorityQueue *resultQueue, int32 begin, int32 end);
extern void AddLink(Relation index, HNSWBuildState *state,
    HNSWGlobalId src, HNSWGlobalId dest,int level, float *qdis);

// vacuum
extern IndexBulkDeleteResult *
hnsw_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

#define HNSW_METAPAGE_BLKNO 0
#define HNSW_MAGICK_NUMBER (0xDBAC0EEE)

#define HNSWPageGetMeta(_page)  ((HNSWMetaPageData *) PageGetContents(_page))

#endif  // PASE_HNSW_HNSW_H_
