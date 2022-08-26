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

#include "utils/pase_hash_table.h"

static int
PaseHashInt(PaseHashTable *t, uint64_t key) {
  key = (key) ^ (key >> 16);
  return key % t->table_size;
}

static int
PaseHashCode(PaseHashTable *t, uint64_t key){
  int curr;
  int i;

  if (t->size >= (t->table_size / 2)) {
    return MAP_FULL;
  }

  curr = PaseHashInt(t, key);

  for (i = 0; i< MAX_CHAIN_LENGTH; ++i) {
    if (t->data[curr].in_use == 0) {
      return curr;
    }

    if (t->data[curr].in_use == 1 && t->data[curr].key == key) {
      return curr;
    }

    curr = (curr + 1) % t->table_size;
  }

  return MAP_FULL;
}

PaseHashTable *
PaseHashCreateTable(MemoryContext ctx) {
  int i;
  PaseHashTable *t;

  t = (PaseHashTable *) MemoryContextAlloc(ctx, sizeof(PaseHashTable));
  t->data = (PaseHashNode*) MemoryContextAlloc(ctx, INITIAL_SIZE * sizeof(PaseHashNode));
  t->table_size = INITIAL_SIZE;
  for (i = 0; i < t->table_size; ++i) {  
    t->data[i].in_use = 0;
  }
  t->size = 0;
  t->ctx = ctx;

  return t;
}

static int
PaseHashReAlloc(PaseHashTable *t) {
  int i;
  int old_size;
  PaseHashNode* curr;

  PaseHashNode *temp = (PaseHashNode *)
    MemoryContextAlloc(t->ctx, 2 * t->table_size * sizeof(PaseHashNode)); 
  for (i = 0; i < 2 * t->table_size; ++i) {  
    temp[i].in_use = 0;
  }

  curr = t->data;
  t->data = temp;

  old_size = t->table_size;
  t->table_size = 2 * t->table_size;
  t->size = 0;

  for (i = 0; i < old_size; i++) {
    int status;
    if (curr[i].in_use == 0) {
      continue;
    }
            
    status = PaseHashInsert(t, curr[i].key, curr[i].val);
    if (status != MAP_OK) {
      return status;
    }
  }

  pfree(curr);
  return MAP_OK;
}

int
PaseHashInsert(PaseHashTable *t, uint64_t key, bool val) {
  int index;

  index = PaseHashCode(t, key);
  while (index == MAP_FULL) {
    PaseHashReAlloc(t);
    index = PaseHashCode(t, key);
  }

  t->data[index].val = val;
  t->data[index].key = key;
  t->data[index].in_use = 1;
  t->size++; 

  return MAP_OK;
}

int
PaseHashLookUp(PaseHashTable *t, uint64_t key, bool* val) {
  int curr;
  int i;

  curr = PaseHashInt(t, key);

  for (i = 0; i < MAX_CHAIN_LENGTH; ++i) {
    int in_use = t->data[curr].in_use;
    if (in_use == 1) {
      if (t->data[curr].key == key) {
        *val = (t->data[curr].val);
        return MAP_OK;
      }
    }

    curr = (curr + 1) % t->table_size;
  }

  return MAP_MISSING;
}

void
PaseHashTableReset(PaseHashTable *t) {
  int i;
  if (t->table_size > 20000) {
    MemoryContext ctx = t->ctx;
    PaseHashTableFree(t);
    t = PaseHashCreateTable(ctx);
    return;
  }
  for (i = 0; i < t->table_size; ++i) {  
    t->data[i].in_use = 0;
  }
  t->size = 0;
}

void
PaseHashTableFree(PaseHashTable *t) {
  pfree(t->data);
  pfree(t);
}
