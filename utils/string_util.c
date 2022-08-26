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

#include "utils/string_util.h"

bool StringToFloat(const char *str, float *val, char **end) {
    if (NULL == str) {
        return false;
    }
    if (NULL == val) {
        return false;
    }
    *val = strtof(str, end);
    if (errno == ERANGE) {
        return false;
    }
    return true;
}


bool StringToLong(const char *str, long *val, char **end) {
    if (NULL == str) {
        return false;
    }
    if (NULL == val) {
        return false;
    }
    *val = strtol(str, end, 0);
    if (errno == ERANGE) {
        return false;
    }
    return true;
}
