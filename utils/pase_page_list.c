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

#include "utils/pase_page_list.h"

#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "pase.h"

// pase_page_list inner macros
#define PASEPL_PAGE_OFFSET(_l, _o) ((_o - 1) / (_l->tup_count))

#define PASEPL_TUP_OFFSET(_l, _o)  (((_o - 1) % (_l->tup_count)) + 1)

#define GetPasePageOpaque(_p)   \
  ((PasePageOpaque) PageGetSpecialPointer(_p))

#define PasePageGetMaxOffset(page)      \
  (GetPasePageOpaque(page)->max_count)

#define PalaemomPageGetFreeSpace(_l, _p)    \
  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData)  \
    - PasePageGetMaxOffset(_p) * _l->tup_size - MAXALIGN(_l->opaque_size))

#define PasePageGetTuple(_l, _p, _o)  \
  ((PaseTuple *)(PageGetContents(_p) + (_l->tup_size) * ((_o) - 1)))

// pase_page_list inner functions
static void InitPaseCachePage(PasePageList *list);
static bool PasePageAddItem(PasePageList *list, PaseTuple *tuple);
static int PaseFlushCachedPage(Relation index, PasePageList *list);

PasePageList *
InitPasePageListByNo(Relation index, int32 header,
    int tupSize, int opaqueSize, int64 totalTupCount) {
  PasePageList *list = palloc(sizeof(PasePageList));

  list->tup_size = tupSize;
  list->opaque_size = opaqueSize;
  list->rw_opts.index = index;
  list->header = header;
  list->tup_count = (BLCKSZ - MAXALIGN(SizeOfPageHeaderData)
    - MAXALIGN(list->opaque_size)) / list->tup_size;
  list->total_tup_count = totalTupCount;
  return list;
}

PasePageList *
InitPasePageList(Relation index, FormPaseTuple former,
    int num, int tupSize, int opaqueSize, void *arg) {
  int blkid = -1;
  int i;
  PasePageList *list = palloc(sizeof(PasePageList));
  PaseTuple *tup;

  list->tup_size = tupSize;
  list->opaque_size = opaqueSize;
  list->former = former;
  list->arg = arg; 
  list->rw_opts.index = index;

  InitPaseCachePage(list);
  for (i = 0; i < num; ++i) {
    tup = former(arg);
    if (PasePageAddItem(list, tup)) {
      list->buffer_count++;
      list->total_tup_count++;
    } else {
      blkid = PaseFlushCachedPage(index, list);
      list->max_writed_blkid = blkid;
      InitPaseCachePage(list);
      if (PasePageAddItem(list, tup)) {
        elog(ERROR, "could not add new tuple to empty page");
      }
      list->buffer_count++;
      list->total_tup_count++;
    }
    pfree(tup);
  }
  if (list->buffer_count > 0) {
    blkid = PaseFlushCachedPage(index, list);
  }
  list->header = blkid;
  list->tup_count = (BLCKSZ - MAXALIGN(SizeOfPageHeaderData)
    - MAXALIGN(list->opaque_size)) / list->tup_size;
  return list;
}

PaseTuple *
PasePlGet(PasePageList *list, int32 offset) {
  int32 pageno, off, maxoffset;

  if (list->header < 0) {
    return NULL;
  } 
  pageno = PASEPL_PAGE_OFFSET(list, offset) + list->header;
  off = PASEPL_TUP_OFFSET(list, offset);
  list->rw_opts.buffer = ReadBuffer(list->rw_opts.index, pageno);
  LockBuffer(list->rw_opts.buffer, BUFFER_LOCK_SHARE);
  list->rw_opts.page = BufferGetPage(list->rw_opts.buffer);
  maxoffset = PasePageGetMaxOffset(list->rw_opts.page);
  if (maxoffset < off) {
    UnlockReleaseBuffer(list->rw_opts.buffer);
    return NULL;
  }
  UnlockReleaseBuffer(list->rw_opts.buffer);
  return (PaseTuple *) PasePageGetTuple(list, list->rw_opts.page, off);
}

PaseTuple *
paseplfisrt(PasePageList *list, int32 start) {
  int32 offset, pageno, maxoffset;

  if (list->header < 0) {
    return NULL;
  } 
  pageno = PASEPL_PAGE_OFFSET(list, start) + list->header;
  offset = PASEPL_TUP_OFFSET(list, start);
  list->rw_opts.buffer = ReadBuffer(list->rw_opts.index, pageno);
  LockBuffer(list->rw_opts.buffer, BUFFER_LOCK_SHARE);
  list->rw_opts.page = BufferGetPage(list->rw_opts.buffer);
  maxoffset = PasePageGetMaxOffset(list->rw_opts.page);
  if (maxoffset < offset) {
    UnlockReleaseBuffer(list->rw_opts.buffer);
    return NULL;
  }
  list->cur_tup = start;
  list->cur_pageno = pageno;
  list->cur_offset = offset;
  list->rw_opts.maxoffset = maxoffset;
  return (PaseTuple *) PasePageGetTuple(list, list->rw_opts.page, offset);
}

PaseTuple *
paseplnext(PasePageList *list) {
  PaseTuple *tup;
  int32 pageno, offset;

  if (list->cur_tup >= list->total_tup_count) {
    if (ConditionalLockBuffer(list->rw_opts.buffer)) {
      UnlockReleaseBuffer(list->rw_opts.buffer);
    } else {
      UnlockReleaseBuffer(list->rw_opts.buffer);
    }
    return NULL;
  }
  list->cur_tup++;
  pageno = list->header - PASEPL_PAGE_OFFSET(list, list->cur_tup);
  offset = PASEPL_TUP_OFFSET(list, list->cur_tup);
  list->cur_pageno = pageno;
  list->cur_offset = offset;

  if (offset == 1) {
    if (ConditionalLockBuffer(list->rw_opts.buffer)) {
      UnlockReleaseBuffer(list->rw_opts.buffer);
    } else {
      UnlockReleaseBuffer(list->rw_opts.buffer);
    }
    list->rw_opts.buffer = ReadBuffer(list->rw_opts.index, pageno);
    LockBuffer(list->rw_opts.buffer, BUFFER_LOCK_SHARE);
    list->rw_opts.page = BufferGetPage(list->rw_opts.buffer);
    list->rw_opts.maxoffset =
      PasePageGetMaxOffset(list->rw_opts.page);
  } 
  if (list->rw_opts.maxoffset < offset) {
    list->cur_tup += list->tup_count - list->rw_opts.maxoffset - 1;
    list->total_tup_count += list->tup_count - list->rw_opts.maxoffset;
    return paseplnext(list);
  }
  tup = PasePageGetTuple(list, list->rw_opts.page, offset);
  if (tup == NULL) {
    UnlockReleaseBuffer(list->rw_opts.buffer);
  }
  return tup;
}

static void
InitPaseCachePage(PasePageList *list) {
  PasePageOpaque opaque;
  memset(list->data.data, 0, BLCKSZ);
  PageInit(list->data.data, BLCKSZ, list->opaque_size);
  opaque = GetPasePageOpaque(list->data.data);
  memset(opaque, 0, list->opaque_size);
  opaque->next_blkid= list->max_writed_blkid;
  list->buffer_count = 0;
}

static bool
PasePageAddItem(PasePageList *list, PaseTuple *tuple) {
  PaseTuple *tup;
  Pointer ptr;
  PasePageOpaque opaque;
  Page page;
  page = list->data.data;

  if (PalaemomPageGetFreeSpace(list, page) < list->tup_size) {
    return false;
  }
  opaque = GetPasePageOpaque(page);
  tup = PasePageGetTuple(list, page, opaque->max_count + 1);
  memcpy((Pointer) tup, (Pointer) tuple, list->tup_size);
  opaque->max_count++;
  ptr = (Pointer) PasePageGetTuple(list, page, opaque->max_count + 1);
  ((PageHeader) page)->pd_lower = ptr - page;
  return true;
}

static int
PaseFlushCachedPage(Relation index, PasePageList *list) {
  Page page;
  Buffer buffer;
  GenericXLogState *state;
  int blkid;

  buffer = PaseNewBuffer(index);
  state = GenericXLogStart(index);
  page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
  memcpy(page, list->data.data, BLCKSZ);
  GenericXLogFinish(state);
  blkid = BufferGetBlockNumber(buffer);
  UnlockReleaseBuffer(buffer);
  CHECK_FOR_INTERRUPTS();
  return blkid;
}
