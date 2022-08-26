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

#include "fmgr.h"
#include "optimizer/cost.h"
#include "utils/selfuncs.h"

#include "ivfflat.h"

// Estimate cost of ivfflat index scan.
void
ivfflat_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
    Cost *indexStartupCost, Cost *indexTotalCost,
    Selectivity *indexSelectivity, double *indexCorrelation,
    double *indexPages) {
  IndexOptInfo *index = path->indexinfo;
  List	   *qinfos;
  GenericCosts costs;

  // Do preliminary analysis of indexquals
  qinfos = deconstruct_indexquals(path);

  MemSet(&costs, 0, sizeof(costs));

  // We have to visit all index tuples anyway
  costs.numIndexTuples = index->tuples;

  // Use generic estimate
  genericcostestimate(root, path, loop_count, qinfos, &costs);

  *indexStartupCost = costs.indexStartupCost;
  *indexTotalCost = costs.indexTotalCost;
  *indexSelectivity = costs.indexSelectivity;
  *indexCorrelation = costs.indexCorrelation;
  *indexPages = costs.numIndexPages;
}
