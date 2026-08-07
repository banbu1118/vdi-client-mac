/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, WMF (Media Foundation) Interface
 *
 * Copyright 2024 Oleg Turovski <oleg2104@hotmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CAMERA_WMF_H
#define CAMERA_WMF_H

#include <winpr/synch.h>
#include <winpr/wtypes.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "../camera.h"

typedef struct
{
	CRITICAL_SECTION lock;

	/* members used to call the callback */
	CameraDevice* dev;
	size_t streamIndex;
	ICamHalSampleCapturedCallback sampleCallback;

	BOOL streaming;

	IMFMediaSource* mediaSource;
	IMFSourceReader* sourceReader;
	IMFMediaType* nativeMediaType; /* Saved native type for SetCurrentMediaType */
	HANDLE captureThread;

} CamWmfStream;

#endif /* CAMERA_WMF_H */
