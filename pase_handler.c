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

#include <omp.h>
#include "access/reloptions.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/pase_hash_table.h"

#include "pase.h"

PG_MODULE_MAGIC;

relopt_kind hnsw_relopt_kind;
relopt_kind ivfflat_relopt_kind;

void
_PG_init(void) {
    int totalCoreNum;
    totalCoreNum = omp_get_num_procs();

    // hnsw options
    hnsw_relopt_kind = add_reloption_kind();
    add_int_reloption(hnsw_relopt_kind, "dim",
                      "vector dimension",
                      256, 8, 512);
    add_int_reloption(hnsw_relopt_kind, "base_nb_num",
                      "hnsw base_nb_num",
                      16, 5, 64);
    add_int_reloption(hnsw_relopt_kind, "ef_build",
                      "build queue",
                      40, 10, 320);
    add_int_reloption(hnsw_relopt_kind, "ef_search",
                      "vector dimension",
                      50, 10, 400);
    add_int_reloption(hnsw_relopt_kind, "base64_encoded",
                      "whether data base64 encoded",
                      0, 0, 1);
    // ivfflat options
    ivfflat_relopt_kind = add_reloption_kind();
    add_int_reloption(ivfflat_relopt_kind, "clustering_type",
                      "clustering type: 0 centroid_file, 1 inner clustering",
                      0, 0, 1);
    add_int_reloption(ivfflat_relopt_kind, "distance_type",
                      "distance metric type:0 l2, 1 inner proudct, 2 cosine",
                      0, 0, 2);
    add_int_reloption(ivfflat_relopt_kind, "dimension",
                      "vector dimension",
                      1, 1, 1024);
    add_int_reloption(ivfflat_relopt_kind, "open_omp",
                      "whether open omp",
                      0, 0, 1);
    add_int_reloption(ivfflat_relopt_kind, "omp_thread_num",
                      "omp thread number",
                      0, 1, totalCoreNum);
    add_int_reloption(ivfflat_relopt_kind, "base64_encoded",
                      "data whether base64 encoded",
                      0, 0, 1);
    add_string_reloption(ivfflat_relopt_kind, "clustering_params",
                      "clustering parameters", "", NULL);

}

bytea *
hnsw_options(Datum reloptions, bool validate) {
  HNSWOptions *opts;
  relopt_value *options;
  int numOptions;

  opts = MakeDefaultHNSWOptions();
  static const relopt_parse_elt hnsw_relopt_tab[] = {
      {"dim", RELOPT_TYPE_INT, offsetof(HNSWOptions, dim)},
      {"base_nb_num", RELOPT_TYPE_INT, offsetof(HNSWOptions, base_nb_num)},
      {"ef_build", RELOPT_TYPE_INT, offsetof(HNSWOptions, ef_build)},
      {"ef_search", RELOPT_TYPE_INT, offsetof(HNSWOptions, ef_search)},
      {"base64_encoded", RELOPT_TYPE_INT, offsetof(HNSWOptions, base64_encoded)}
  };
  options = parseRelOptions(reloptions, validate, hnsw_relopt_kind, &numOptions);
  if (numOptions < 5) {
    elog(ERROR, "options format error");
  }
  opts = allocateReloptStruct(sizeof(HNSWOptions), options, numOptions);
  fillRelOptions((void *) opts, sizeof(HNSWOptions), options, numOptions,
      validate, hnsw_relopt_tab, lengthof(hnsw_relopt_tab));
  pfree(options);
  return (bytea *)opts;
}

PG_FUNCTION_INFO_V1(pase_ivfflat);

Datum
pase_ivfflat(PG_FUNCTION_ARGS) {
  IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

  amroutine->amstrategies = 1;
  amroutine->amsupport = 1;
  amroutine->amcanorder = false;
  amroutine->amcanorderbyop = true;
  amroutine->amcanbackward = false;
  amroutine->amcanunique = false;
  amroutine->amcanmulticol = true;
  amroutine->amoptionalkey = true;
  amroutine->amsearcharray = false;
  amroutine->amsearchnulls = false;
  amroutine->amstorage = false;
  amroutine->amclusterable = false;
  amroutine->ampredlocks = false;
  amroutine->amcanparallel = false;
  amroutine->amkeytype = InvalidOid;

  amroutine->ambuild = ivfflat_build;
  amroutine->ambuildempty = ivfflat_buildempty;
  amroutine->aminsert = ivfflat_insert;
  amroutine->ambulkdelete = ivfflat_bulkdelete;
  amroutine->amvacuumcleanup = ivfflat_vacuumcleanup;
  amroutine->amcanreturn = NULL;
  amroutine->amcostestimate = ivfflat_costestimate;
  amroutine->amoptions = ivfflat_options;
  amroutine->amproperty = NULL;
  amroutine->ambeginscan = ivfflat_beginscan;
  amroutine->amrescan = ivfflat_rescan;
  amroutine->amgettuple = ivfflat_gettuple;
  amroutine->amgetbitmap = ivfflat_getbitmap;
  amroutine->amendscan = ivfflat_endscan;
  amroutine->ammarkpos = NULL;
  amroutine->amrestrpos = NULL;
  amroutine->amestimateparallelscan = NULL;
  amroutine->aminitparallelscan = NULL;
  amroutine->amparallelrescan = NULL;

  PG_RETURN_POINTER(amroutine);
}

PG_FUNCTION_INFO_V1(pase_hnsw);

Datum
pase_hnsw(PG_FUNCTION_ARGS) {
  IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

  amroutine->amstrategies = 1;
  amroutine->amsupport = 1;
  amroutine->amcanorder = false;
  amroutine->amcanorderbyop = true;
  amroutine->amcanbackward = false;
  amroutine->amcanunique = false;
  amroutine->amcanmulticol = true;
  amroutine->amoptionalkey = true;
  amroutine->amsearcharray = false;
  amroutine->amsearchnulls = false;
  amroutine->amstorage = false;
  amroutine->amclusterable = false;
  amroutine->ampredlocks = false;
  amroutine->amcanparallel = false;
  amroutine->amkeytype = InvalidOid;

  amroutine->ambuild = hnsw_build;
  amroutine->ambuildempty = hnsw_buildempty;
  amroutine->aminsert = hnsw_insert;
  amroutine->ambulkdelete = NULL;
  amroutine->amvacuumcleanup = hnsw_vacuumcleanup;
  amroutine->amcanreturn = NULL;
  amroutine->amcostestimate = hnsw_costestimate;
  amroutine->amoptions = hnsw_options;
  amroutine->amproperty = NULL;
  amroutine->ambeginscan = hnsw_beginscan;
  amroutine->amrescan = hnsw_rescan;
  amroutine->amgettuple = hnsw_gettuple;
  amroutine->amgetbitmap = NULL;
  amroutine->amendscan = hnsw_endscan;
  amroutine->ammarkpos = NULL;
  amroutine->amrestrpos = NULL;
  amroutine->amestimateparallelscan = NULL;
  amroutine->aminitparallelscan = NULL;
  amroutine->amparallelrescan = NULL;

  PG_RETURN_POINTER(amroutine);
}
