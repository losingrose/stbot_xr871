#
# Copyright (2017) Baidu Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)
##
# Build for coap
#

include $(CLEAR_VARS)

MODULE_PATH := $(LOCAL_PATH)

LOCAL_MODULE := coap

ifeq ($(strip $(DUER_HIDDEN_SYMBOLS)),true)
  LOCAL_CFLAGS += -fvisibility=hidden
endif

MY_SRC_FILES := \
    $(wildcard $(LOCAL_PATH)/*.c)

LOCAL_SRC_FILES := $(MY_SRC_FILES:$(LOCAL_PATH)/%=%)

LOCAL_STATIC_LIBRARIES := framework nsdl device_status

LOCAL_EXPORT_C_INCLUDES := $(MODULE_PATH)

include $(BUILD_STATIC_LIBRARY)
