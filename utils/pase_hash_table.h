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
// ==============================================================================
//
// PaseHashTable is a simple implent of hashmap.

#ifndef PASE_UTILS_PASE_HASH_TABLE_H_
#define PASE_UTILS_PASE_HASH_TABLE_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "postgres.h"
#include "fmgr.h"

#define MAP_MISSING -2
#define MAP_FULL -1
#define MAP_OK 0

#define INITIAL_SIZE 1000
#define MAX_CHAIN_LENGTH 20

typedef struct PaseHashNode {
  uint64_t key;
  int in_use;
  bool val;
} PaseHashNode;

typedef struct PaseHashTable {
  int table_size;
  int size;
  PaseHashNode *data;
  MemoryContext ctx;
} PaseHashTable;

extern PaseHashTable *PaseHashCreateTable(MemoryContext ctx);
extern int PaseHashInsert(PaseHashTable *t, uint64_t key, bool val);
extern int PaseHashLookUp(PaseHashTable *t, uint64_t key, bool *val);
extern void PaseHashTableReset(PaseHashTable *t);
extern void PaseHashTableFree(PaseHashTable *t);

#endif  // PASE_UTILS_PASE_HASH_TABLE_H_
