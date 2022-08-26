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
// This impaseentation is copy from 'src/include/lib/pairingheap.h'.
// pairingheap is simple implement of priorityqueue.
// Add 'PriorityQueueSize' to get the heap size.
//
// Priorityqueue also can simple implement by list.

#ifndef PASE_UTILS_PRIORITY_QUEUE_H_
#define PASE_UTILS_PRIORITY_QUEUE_H_

#include <stdio.h>

#include "postgres.h"

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "fmgr.h"
#include "lib/pairingheap.h"
#include "nodes/relation.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"

typedef struct PriorityQueueNode {
  struct PriorityQueueNode *first_child;
  struct PriorityQueueNode *next_sibling;
  struct PriorityQueueNode *prev_or_parent;
} PriorityQueueNode;

typedef int (*PriorityQueueCmp) (const PriorityQueueNode *a,
                                 const PriorityQueueNode *b,
                                 void *arg);

typedef struct PriorityQueue {
  PriorityQueueCmp compare;
  void *arg;
  int size;
  PriorityQueueNode *root;
} PriorityQueue;

extern PriorityQueue *PriorityQueueAllocate(PriorityQueueCmp compare, void *arg);
extern void PriorityQueueAdd(PriorityQueue *queue, PriorityQueueNode *node);
extern void PriorityQueueFree(PriorityQueue *queue);
extern int PriorityQueueSize(PriorityQueue *queue);
extern PriorityQueueNode *PriorityQueueFirst(PriorityQueue *queue);
extern PriorityQueueNode *PriorityQueuePop(PriorityQueue *queue);

#define PriorityQueueReset(queue)       ((queue)->root = NULL)
#define PriorityQueueIsEmpty(queue)    ((queue)->root == NULL)
#define PriorityQueueIsSingular(queue) \
  ((queue)->root && (queue)->root->first_child == NULL)

#endif  // PASE_UTILS_PRIORITY_QUEUE_H_
