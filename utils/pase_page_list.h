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
// PasePageList is a wraper of page list
//
// NOTICE: 
// 1. it is not safely called from multiple threads at the same time.
// 2. if you use InitPasePageListByNo your PageList must be
//    Continuous memory

#ifndef PASE_UTILS_PASE_PAGE_LIST_H_
#define PASE_UTILS_PASE_PAGE_LIST_H_

#include "postgres.h"

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "lib/pairingheap.h"
#include "nodes/relation.h"

typedef struct PaseTuple {
  char *ppl_t;  // PasePageList tag
} PaseTuple; 

typedef struct PasePageOpaqueData {
  int32 max_count;
  int32 next_blkid;
} PasePageOpaqueData;

typedef PasePageOpaqueData *PasePageOpaque;

typedef PaseTuple* (*FormPaseTuple) (void *arg);

typedef struct PasePageListRWOpts {
  int32 maxoffset;
  Relation index;
  Buffer buffer;
  Page page;
} PasePageListRWOpts;

typedef struct PasePageList {
  int32 header;
  int32 tup_size;
  int32 opaque_size;
  int32 buffer_count;
  int32 tup_count;
  int32 max_writed_blkid;
  void *arg;
  FormPaseTuple former;
  PasePageOpaqueData opaque;
  int64 cur_tup;
  int64 cur_pageno;
  int64 cur_offset;
  int64 total_tup_count;
  PasePageListRWOpts rw_opts;
  PGAlignedBlock data;
} PasePageList;

#define pase_page_foreach(_t, _l) \
  for ((_t) = paseplfisrt(_l, 1); (_t) != NULL; (_t) = paseplnext(_l))
#define pase_page_foreach2(_t, _l, _s) \
  for ((_t) = paseplfisrt(_l, _s); (_t) != NULL; (_t) = paseplnext(_l))
extern PaseTuple *paseplfisrt(PasePageList *list, int32 start);
extern PaseTuple *paseplnext(PasePageList *list);

extern PasePageList *InitPasePageList(Relation index,
    FormPaseTuple former, int num, int tupSize,
    int opaqueSize, void *arg);
extern PasePageList *InitPasePageListByNo(Relation index, int32 header,
    int tupSize, int opaqueSize, int64 totalTupCount);
extern PaseTuple *PasePlGet(PasePageList *list, int32 offset);

#endif  // PASE_UTILS_PASE_PAGE_LIST_H_
