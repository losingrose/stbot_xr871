/**
 * Copyright (2017) Baidu Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * File: lightduer_data_cache.h
 * Auth: Su Hao (suhao@baidu.com)
 * Desc: Light duer stored the send data.
 */

#ifndef BAIDU_DUER_LIGHTDUER_COMMON_LIGHTDUER_DATA_CACHE_H
#define BAIDU_DUER_LIGHTDUER_COMMON_LIGHTDUER_DATA_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _duer_transmit_data_s {
    void *  data;
    size_t  size;
} duer_dcache_item;

void duer_dcache_initialize(void);

duer_status_t duer_dcache_push(const void *data, size_t size);

duer_dcache_item *duer_dcache_top(void);

void duer_dcache_pop(void);

size_t duer_dcache_length(void);

void duer_dcache_clear(void);

void duer_dcache_finalize(void);

#ifdef __cplusplus
}
#endif

#endif/*BAIDU_DUER_LIGHTDUER_COMMON_LIGHTDUER_DATA_CACHE_H*/