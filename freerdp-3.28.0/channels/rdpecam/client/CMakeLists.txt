# FreeRDP: A Remote Desktop Protocol Implementation
# FreeRDP cmake build script
#
# Copyright 2024 Oleg Turovski <oleg2104@hotmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

define_channel_client("rdpecam")

# camera redirect supported platforms:
#   Windows → Media Foundation (WMF)
#   macOS   → AVFoundation (AVF)
#   Android → Android Camera2
#   Linux   → Video4Linux (V4L)
if(WIN32)
  # Windows: use Media Foundation backend
  set(WITH_WMF ON)
  add_compile_definitions("WITH_WMF")
elseif(APPLE)
  # macOS: use AVFoundation backend (no libv4l available)
  set(WITH_AVF ON)
  add_compile_definitions("WITH_AVF")
elseif(NOT ANDROID)
  find_package(V4L)
  if(V4L_FOUND)
    set(WITH_V4L ON)
    add_compile_definitions("WITH_V4L")
  else()
    message(FATAL_ERROR "libv4l-dev required for CHANNEL_RDPECAM_CLIENT")
  endif()
endif()

set(${MODULE_PREFIX}_LIBS freerdp winpr)

if(WITH_WMF)
  list(APPEND ${MODULE_PREFIX}_LIBS mfplat mf mfuuid mfreadwrite)
endif()

set(${MODULE_PREFIX}_SRCS camera_device_enum_main.c camera_device_main.c encoding.c)

# TRUE = build as static addin (linked into libfreerdp-client), not dynamic plugin (.so)
add_channel_client_library(${MODULE_PREFIX} ${MODULE_NAME} ${CHANNEL_NAME} TRUE "DVCPluginEntry")

if(WITH_AVF)
  add_channel_client_subsystem(${MODULE_PREFIX} ${CHANNEL_NAME} "avf" "")
endif()

if(WITH_V4L)
  add_channel_client_subsystem(${MODULE_PREFIX} ${CHANNEL_NAME} "v4l" "")
endif()

if(WITH_WMF)
  add_channel_client_subsystem(${MODULE_PREFIX} ${CHANNEL_NAME} "wmf" "")
endif()

if(ANDROID)
  add_channel_client_subsystem(${MODULE_PREFIX} ${CHANNEL_NAME} "android" "")
endif()
