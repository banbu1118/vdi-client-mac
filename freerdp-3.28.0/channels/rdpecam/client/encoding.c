/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, Video Encoding
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

#include <winpr/assert.h>
#include <winpr/winpr.h>

#include "camera.h"

#define TAG CHANNELS_TAG("rdpecam-video.client")

UINT32 h264_get_max_bitrate(UINT32 height)
{
	static const struct { UINT32 height; UINT32 bitrate; }
	bitrates[] = {
		{ 1080, 2700 }, { 720, 1250 }, { 480, 700 },
		{ 360, 400 },   { 240, 170 },  { 180, 140 }, { 0, 100 },
	};
	for (size_t i = 0; i < ARRAYSIZE(bitrates); i++)
		if (height >= bitrates[i].height)
			return bitrates[i].bitrate * 1000;
	return 100000;
}

FREERDP_VIDEO_FORMAT ecamToVideoFormat(CAM_MEDIA_FORMAT ecamFormat)
{
	switch (ecamFormat)
	{
		case CAM_MEDIA_FORMAT_YUY2:  return FREERDP_VIDEO_FORMAT_YUYV422;
		case CAM_MEDIA_FORMAT_NV12:  return FREERDP_VIDEO_FORMAT_NV12;
		case CAM_MEDIA_FORMAT_I420:  return FREERDP_VIDEO_FORMAT_YUV420P;
		case CAM_MEDIA_FORMAT_RGB24: return FREERDP_VIDEO_FORMAT_RGB24;
		case CAM_MEDIA_FORMAT_RGB32: return FREERDP_VIDEO_FORMAT_RGB32;
		case CAM_MEDIA_FORMAT_H264:  return FREERDP_VIDEO_FORMAT_H264;
		case CAM_MEDIA_FORMAT_MJPG:  return FREERDP_VIDEO_FORMAT_MJPEG;
		default: return FREERDP_VIDEO_FORMAT_NONE;
	}
}

static BOOL ecam_init_video_context(CameraDeviceStream* stream)
{
	WINPR_ASSERT(stream);
	if (stream->video) return TRUE;
	stream->video = freerdp_video_context_new(stream->currMediaType.Width,
	                                          stream->currMediaType.Height);
	if (!stream->video) { WLog_ERR(TAG, "freerdp_video_context_new failed"); return FALSE; }
	return TRUE;
}

BOOL ecam_encoder_context_init(CameraDeviceStream* stream)
{
	if (!ecam_init_video_context(stream))
		return FALSE;
	const UINT32 fr = stream->currMediaType.FrameRateNumerator /
	                  stream->currMediaType.FrameRateDenominator;
	if (!freerdp_video_context_reconfigure(stream->video, stream->currMediaType.Width,
	                                       stream->currMediaType.Height, fr, 0,
	                                       H264_CAMERA_VIDEO_REAL_TIME))
		WLog_WARN(TAG, "Video context reconfigure failed, continuing");
	return TRUE;
}

BOOL ecam_encoder_context_free(CameraDeviceStream* stream)
{
	if (!stream) return FALSE;
	if (stream->video) { freerdp_video_context_free(stream->video); stream->video = nullptr; }
	return TRUE;
}

BOOL ecam_encoder_compress(CameraDeviceStream* stream, const BYTE* srcData, size_t srcSize,
                           wStream* output)
{
	if (!ecam_encoder_context_init(stream))
		return FALSE;
	return freerdp_video_sample_convert(stream->video,
	                                    ecamToVideoFormat(streamInputFormat(stream)),
	                                    srcData, srcSize,
	                                    ecamToVideoFormat(streamOutputFormat(stream)),
	                                    output);
}
