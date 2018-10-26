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
 * File: lightduer_random_impl.cpp
 * Auth: Zhang Leliang(zhangleliang@baidu.com)
 * Desc: Provide the random APIs.
 */

#include "lightduer_random_impl.h"


extern int xr871_rand_generate(void);

duer_s32_t duer_random_impl(void) {

	int res;
	res = xr871_rand_generate();
	return res;

}
