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

#include "priority_queue.h"

// helper function to merge two subheaps into one
static PriorityQueueNode *Merge(PriorityQueue *queue,
          PriorityQueueNode *a, PriorityQueueNode *b);
static PriorityQueueNode *MergeChildren(PriorityQueue *queue,
          PriorityQueueNode *children);

PriorityQueue *
PriorityQueueAllocate(PriorityQueueCmp compare, void *arg) {
  PriorityQueue *queue;
  queue = (PriorityQueue *) palloc(sizeof(PriorityQueue));

  queue->compare = compare;
  queue->arg = arg;

  queue->root = NULL;
  queue->size = 0;
  return queue;
}

void
PriorityQueueFree(PriorityQueue *queue) {
  while (!PriorityQueueIsEmpty(queue)) {
    pfree(PriorityQueuePop(queue));
  }
  pfree(queue);
}

// add the given node to the heap in O(1) time.
void
PriorityQueueAdd(PriorityQueue *queue, PriorityQueueNode *node) {
  node->first_child = NULL;
  queue->size++;

  queue->root = Merge(queue, queue->root, node);
  queue->root->prev_or_parent = NULL;
  queue->root->next_sibling = NULL;
}

// get size of heap in O(1) time.
int
PriorityQueueSize(PriorityQueue *queue) {
  return PriorityQueueIsEmpty(queue) ? 0 : queue->size;
}

// get first node of heap in O(1) time.
PriorityQueueNode *
PriorityQueueFirst(PriorityQueue *queue) {
  if (PriorityQueueIsEmpty(queue)) {
    return NULL;
  }
  return queue->root;
}

// pop first node of heap in O(log n) time.
PriorityQueueNode *
PriorityQueuePop(PriorityQueue *queue) {
  PriorityQueueNode *result;
  PriorityQueueNode *children;

  if (PriorityQueueIsEmpty(queue)) {
    return NULL;
  }

  result = queue->root;
  children = result->first_child;

  queue->root = MergeChildren(queue, children);
  if (queue->root) {
    queue->root->prev_or_parent = NULL;
    queue->root->next_sibling = NULL;
  }
  queue->size--;

  return result;
}

static PriorityQueueNode *
Merge(PriorityQueue *queue,
    PriorityQueueNode *a, PriorityQueueNode *b) {
  if (a == NULL) {
    return b;
  }
  if (b == NULL) {
    return a;
  }

  if (queue->compare(a, b, queue->arg) < 0) {
    PriorityQueueNode *tmp;
    tmp = a;
    a = b;
    b = tmp;
  }

  if (a->first_child) {
    a->first_child->prev_or_parent = b;
  }
  b->prev_or_parent = a;
  b->next_sibling = a->first_child;
  a->first_child = b;
  return a;
}

static PriorityQueueNode *
MergeChildren(PriorityQueue *queue,
    PriorityQueueNode *children) {
  PriorityQueueNode *curr, *next;
  PriorityQueueNode *pairs;
  PriorityQueueNode *newroot;

  if (children == NULL || children->next_sibling == NULL) {
    return children;
  }
  next = children;
  pairs = NULL;
  for (;;) {
    curr = next;
    if (curr == NULL) {
      break;
    }
    if (curr->next_sibling == NULL) {
      curr->next_sibling = pairs;
      pairs = curr;
      break;
    }
    next = curr->next_sibling->next_sibling;
    curr = Merge(queue, curr, curr->next_sibling);
    curr->next_sibling = pairs;
    pairs = curr;
  }

  newroot = pairs;
  next = pairs->next_sibling;
  while (next) {
    curr = next;
    next = curr->next_sibling;
    newroot = Merge(queue, newroot, curr);
  }

  return newroot;
}
