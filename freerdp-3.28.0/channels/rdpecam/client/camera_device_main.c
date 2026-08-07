/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, Device Channels
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
#include <winpr/cast.h>
#include <winpr/interlocked.h>
#include <winpr/sysinfo.h>

#include "camera.h"
#include "rdpecam-utils.h"

#define TAG CHANNELS_TAG("rdpecam-device.client")

/**
 * @brief Get supported format list based on available codecs
 *
 * Returns supported formats in preference order:
 * H264, MJPG (if decoder available), I420, other YUV based, RGB based
 *
 * @param pCount Output parameter for number of formats
 * @return Pointer to format array
 */
static const CAM_MEDIA_FORMAT_INFO* getSupportedFormats(size_t* pCount)
{
	WINPR_ASSERT(pCount);

	/* Format priority: MJPG > H264 > YUY2 > NV12.
	 * Passthrough pairs (src==dst) always work — the camera either supports
	 * the format natively or it doesn't. Additionally, NV12 -> H264 is offered
	 * so that backends capturing raw NV12 (AVFoundation/macOS) can transmit
	 * software-encoded H.264 instead of uncompressed NV12. */
	const CAM_MEDIA_FORMAT baseAvailable[] = { CAM_MEDIA_FORMAT_MJPG,
		                                        CAM_MEDIA_FORMAT_H264,
		                                        CAM_MEDIA_FORMAT_YUY2,
		                                        CAM_MEDIA_FORMAT_NV12 };
	static CAM_MEDIA_FORMAT_INFO formats[ARRAYSIZE(baseAvailable) * ARRAYSIZE(baseAvailable)];
	static size_t count = 0;
	static BOOL initialized = FALSE;

	if (!initialized)
	{
		for (size_t i = 0; i < ARRAYSIZE(baseAvailable); i++)
		{
			for (size_t j = 0; j < ARRAYSIZE(baseAvailable); j++)
			{
				const BOOL passthrough = (baseAvailable[j] == baseAvailable[i]);
				const BOOL nv12ToH264 = (baseAvailable[j] == CAM_MEDIA_FORMAT_NV12 &&
				                         baseAvailable[i] == CAM_MEDIA_FORMAT_H264);
				if ((passthrough || nv12ToH264) &&
				    freerdp_video_conversion_supported(ecamToVideoFormat(baseAvailable[j]),
				                                       ecamToVideoFormat(baseAvailable[i])))
					formats[count++] = (CAM_MEDIA_FORMAT_INFO){ baseAvailable[j], baseAvailable[i] };
			}
		}
		initialized = TRUE;
	}

	*pCount = count;
	return formats;
}

static void ecam_dev_write_media_type(wStream* s, CAM_MEDIA_TYPE_DESCRIPTION* mediaType)
{
	WINPR_ASSERT(mediaType);

	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, mediaType->Format));
	Stream_Write_UINT32(s, mediaType->Width);
	Stream_Write_UINT32(s, mediaType->Height);
	Stream_Write_UINT32(s, mediaType->FrameRateNumerator);
	Stream_Write_UINT32(s, mediaType->FrameRateDenominator);
	Stream_Write_UINT32(s, mediaType->PixelAspectRatioNumerator);
	Stream_Write_UINT32(s, mediaType->PixelAspectRatioDenominator);
	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, mediaType->Flags));
}

static BOOL ecam_dev_read_media_type(wStream* s, CAM_MEDIA_TYPE_DESCRIPTION* mediaType)
{
	WINPR_ASSERT(mediaType);

	const uint8_t format = Stream_Get_UINT8(s);
	if (!rdpecam_valid_CamMediaFormat(format))
		return FALSE;

	mediaType->Format = WINPR_ASSERTING_INT_CAST(CAM_MEDIA_FORMAT, format);
	Stream_Read_UINT32(s, mediaType->Width);
	Stream_Read_UINT32(s, mediaType->Height);
	Stream_Read_UINT32(s, mediaType->FrameRateNumerator);
	Stream_Read_UINT32(s, mediaType->FrameRateDenominator);
	Stream_Read_UINT32(s, mediaType->PixelAspectRatioNumerator);
	Stream_Read_UINT32(s, mediaType->PixelAspectRatioDenominator);

	if (mediaType->FrameRateNumerator == 0)
		return FALSE;
	if (mediaType->FrameRateDenominator == 0)
		return FALSE;
	if (mediaType->PixelAspectRatioNumerator == 0)
		return FALSE;
	if (mediaType->PixelAspectRatioDenominator == 0)
		return FALSE;

	const uint8_t flags = Stream_Get_UINT8(s);
	if (!rdpecam_valid_MediaTypeDescriptionFlags(flags))
		return FALSE;
	mediaType->Flags = WINPR_ASSERTING_INT_CAST(CAM_MEDIA_TYPE_DESCRIPTION_FLAGS, flags);
	return TRUE;
}

static void ecam_dev_print_media_type(CAM_MEDIA_TYPE_DESCRIPTION* mediaType)
{
	WINPR_ASSERT(mediaType);

	WLog_DBG(TAG, "Format: %u, width: %u, height: %u, fps: %u, flags: %u", mediaType->Format,
	         mediaType->Width, mediaType->Height, mediaType->FrameRateNumerator, mediaType->Flags);
}

/**
 * @brief Prepare a sample response stream with header fields
 *
 * @return wStream with header written, or nullptr on failure
 */
WINPR_ATTR_NODISCARD
static wStream* ecam_dev_prepare_sample_response(CameraDevice* dev, size_t streamIndex)
{
	WINPR_ASSERT(dev);

	CameraDeviceStream* stream = &dev->streams[streamIndex];
	CAM_MSG_ID msg = CAM_MSG_ID_SampleResponse;

	Stream_ResetPosition(stream->sampleRespBuffer);
	if (!Stream_EnsureRemainingCapacity(stream->sampleRespBuffer, 3))
		return nullptr;

	Stream_Write_UINT8(stream->sampleRespBuffer,
	                   WINPR_ASSERTING_INT_CAST(uint8_t, dev->ecam->version));
	Stream_Write_UINT8(stream->sampleRespBuffer, WINPR_ASSERTING_INT_CAST(uint8_t, msg));
	Stream_Write_UINT8(stream->sampleRespBuffer, WINPR_ASSERTING_INT_CAST(uint8_t, streamIndex));

	return stream->sampleRespBuffer;
}

static UINT ecam_dev_send_pending(CameraDevice* dev, size_t streamIndex, CameraDeviceStream* stream)
{
	WINPR_ASSERT(dev);
	WINPR_ASSERT(stream);

	if (stream->samplesRequested <= 0)
	{
		WLog_VRB(TAG, "Frame delayed: No sample requested");
		return CHANNEL_RC_OK;
	}

	if (!stream->haveSample)
	{
		WLog_VRB(TAG, "Frame response delayed: No sample available");
		return CHANNEL_RC_OK;
	}

	wStream* output = ecam_dev_prepare_sample_response(dev, streamIndex);
	if (!output)
		return CHANNEL_RC_OK;

	/* The pendingSample already holds the newest pre-encoded frame (encoded on
	 * the capture thread in ecam_dev_sample_captured_callback), so responding
	 * here is just a copy — no encoder work, no blocking. This keeps the
	 * end-to-end latency and the server's request pacing independent of the
	 * encoder speed. */
	const BYTE* encodedSample = Stream_Buffer(stream->pendingSample);
	const size_t encodedSize = Stream_Length(stream->pendingSample);

	if (!Stream_EnsureRemainingCapacity(output, encodedSize))
		return CHANNEL_RC_OK;
	Stream_Write(output, encodedSample, encodedSize);

	if (!stream->streaming)
	{
		WLog_DBG(TAG, "Frame delayed/dropped: stream stopped");
		return CHANNEL_RC_OK;
	}

	stream->samplesRequested--;
	stream->haveSample = FALSE;

	/* channel write is protected by critical section in dvcman_write_channel */
	return ecam_channel_write(dev->ecam, stream->hSampleReqChannel, CAM_MSG_ID_SampleResponse,
	                          output, FALSE /* don't free stream */);
}

static UINT ecam_dev_sample_captured_callback(CameraDevice* dev, size_t streamIndex,
                                              const BYTE* sample, size_t size)
{
	WINPR_ASSERT(dev);

	if (streamIndex >= ECAM_DEVICE_MAX_STREAMS)
		return ERROR_INVALID_INDEX;

	CameraDeviceStream* stream = &dev->streams[streamIndex];

	if (!stream->streaming)
	{
		WLog_DBG(TAG, "Frame drop: stream not running");
		return CHANNEL_RC_OK;
	}

	EnterCriticalSection(&stream->lock);
	UINT ret = CHANNEL_RC_NO_MEMORY;

	/* Pre-encode the freshest captured frame right here on the capture thread,
	 * instead of deferring the encoder to the SampleRequest path. The encoder
	 * produces exactly one output per captured frame (one-frame-delayed
	 * pipeline for VideoToolbox), so a pending request can be answered with
	 * the most recent encoded frame almost immediately. If the encoder is not
	 * ready yet (warm-up/underrun, rc=-2) this sample is dropped and the next
	 * captured frame simply replaces it. */
	Stream_ResetPosition(stream->pendingSample);
	if (!ecam_encoder_compress(stream, sample, size, stream->pendingSample))
	{
		WLog_DBG(TAG, "Frame dropped: error in ecam_encoder_compress");
		goto out;
	}
	Stream_SealLength(stream->pendingSample);
	stream->haveSample = TRUE;

	ret = ecam_dev_send_pending(dev, streamIndex, stream);

out:
	LeaveCriticalSection(&stream->lock);
	return ret;
}

static void ecam_dev_stop_stream(CameraDevice* dev, size_t streamIndex)
{
	WINPR_ASSERT(dev);

	if (streamIndex >= ECAM_DEVICE_MAX_STREAMS)
		return;

	CameraDeviceStream* stream = &dev->streams[streamIndex];

	/* The stream critical section is device-lifetime; it is NOT deleted here
	 * (see ecam_dev_create) so the idle-check timer can always lock safely. */
	if (stream->streaming)
	{
		stream->streaming = FALSE;
		dev->ihal->StopStream(dev->ihal, dev->deviceId, 0);
	}
	stream->autoPaused = FALSE;
	stream->lastSampleRequestTime = 0;

	Stream_Free(stream->sampleRespBuffer, TRUE);
	stream->sampleRespBuffer = nullptr;

	Stream_Free(stream->pendingSample, TRUE);
	stream->pendingSample = nullptr;

	ecam_encoder_context_free(stream);
}

/**
 * Pause a stream after an idle timeout (no SampleRequest for a while).
 * Releases the camera hardware so e.g. the camera activity LED goes off, even
 * if the server never sends StopStreamsRequest / DeactivateDeviceRequest.
 * The stream structure (lock, response buffers) is kept alive so a later
 * SampleRequest can resume the stream transparently.
 *
 * Caller must NOT hold the stream lock.
 *
 * @return TRUE if the stream was actually paused
 */
BOOL ecam_dev_pause_stream_idle(CameraDevice* dev, size_t streamIndex, UINT64 now)
{
	WINPR_ASSERT(dev);

	if (streamIndex >= ECAM_DEVICE_MAX_STREAMS)
		return FALSE;

	CameraDeviceStream* stream = &dev->streams[streamIndex];

	EnterCriticalSection(&stream->lock);

	if (!stream->streaming || stream->autoPaused)
	{
		LeaveCriticalSection(&stream->lock);
		return FALSE;
	}

	const UINT64 idleMs = now - stream->lastSampleRequestTime;
	if (idleMs < ECAM_STREAM_IDLE_TIMEOUT_MS)
	{
		LeaveCriticalSection(&stream->lock);
		return FALSE;
	}

	stream->streaming = FALSE;
	stream->autoPaused = TRUE;
	stream->samplesRequested = 0;
	stream->haveSample = FALSE;

	dev->ihal->StopStream(dev->ihal, dev->deviceId, 0);
	ecam_encoder_context_free(stream);

	LeaveCriticalSection(&stream->lock);

	WLog_WARN(TAG, "Stream %s/%zu idle for %" PRIu64 " ms, auto-paused (camera released)",
	          dev->deviceId, (size_t)streamIndex, idleMs);

	return TRUE;
}

/**
 * Resume a stream that was auto-paused by the idle timeout.
 * Re-creates the encoder and restarts the capture using the saved media type.
 *
 * Caller must hold the stream lock.
 *
 * @return CHANNEL_RC_OK on success, a Win32 error code otherwise
 */
static UINT ecam_dev_resume_stream(CameraDevice* dev, size_t streamIndex)
{
	WINPR_ASSERT(dev);

	CameraDeviceStream* stream = &dev->streams[streamIndex];

	CAM_MEDIA_TYPE_DESCRIPTION mediaType = stream->currMediaType;
	mediaType.Format = streamInputFormat(stream);
	if (mediaType.Format == CAM_MEDIA_FORMAT_INVALID)
		return ERROR_INVALID_DATA;

	if (!ecam_encoder_context_init(stream))
	{
		WLog_ERR(TAG, "Unable to re-initialize encoder for %s/%zu", dev->deviceId, streamIndex);
		return ERROR_MOD_NOT_FOUND;
	}

	if (!stream->sampleRespBuffer)
		stream->sampleRespBuffer = Stream_New(nullptr, ECAM_SAMPLE_RESPONSE_BUFFER_SIZE);
	if (!stream->pendingSample)
		stream->pendingSample = Stream_New(nullptr, 4ull * mediaType.Width * mediaType.Height);
	if (!stream->sampleRespBuffer || !stream->pendingSample)
	{
		ecam_encoder_context_free(stream);
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	const CAM_ERROR_CODE error = dev->ihal->StartStream(dev->ihal, dev, streamIndex, &mediaType,
	                                                    ecam_dev_sample_captured_callback);
	if (error)
	{
		WLog_ERR(TAG, "Unable to resume stream %s/%zu, StartStream error=%" PRIu32, dev->deviceId,
		         streamIndex, error);
		ecam_encoder_context_free(stream);
		return ERROR_GEN_FAILURE;
	}

	stream->streaming = TRUE;
	stream->autoPaused = FALSE;
	stream->samplesRequested = 0;
	stream->haveSample = FALSE;

	WLog_WARN(TAG, "Stream %s/%zu resumed after SampleRequest", dev->deviceId, streamIndex);
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_stop_streams_request(CameraDevice* dev,
                                                  GENERIC_CHANNEL_CALLBACK* hchannel, wStream* s)
{
	WINPR_ASSERT(dev);
	WINPR_UNUSED(s);

	for (size_t i = 0; i < ECAM_DEVICE_MAX_STREAMS; i++)
		ecam_dev_stop_stream(dev, i);

	return ecam_channel_send_generic_msg(dev->ecam, hchannel, CAM_MSG_ID_SuccessResponse);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_start_streams_request(CameraDevice* dev,
                                                   GENERIC_CHANNEL_CALLBACK* hchannel, wStream* s)
{
	BYTE streamIndex = 0;
	CAM_MEDIA_TYPE_DESCRIPTION mediaType = WINPR_C_ARRAY_INIT;

	WINPR_ASSERT(dev);

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 1 + 26))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT8(s, streamIndex);

	if (streamIndex >= ECAM_DEVICE_MAX_STREAMS)
	{
		WLog_ERR(TAG, "Incorrect streamIndex %" PRIu8, streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_InvalidStreamNumber);
		return ERROR_INVALID_INDEX;
	}

	if (!ecam_dev_read_media_type(s, &mediaType))
	{
		WLog_ERR(TAG, "Unable to read MEDIA_TYPE_DESCRIPTION");
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_InvalidMessage);
		return ERROR_INVALID_DATA;
	}

	ecam_dev_print_media_type(&mediaType);

	CameraDeviceStream* stream = &dev->streams[streamIndex];

	if (stream->streaming)
	{
		WLog_ERR(TAG, "Streaming already in progress, device %s, streamIndex %u", dev->deviceId,
		         streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_UnexpectedError);
		return ERROR_INVALID_DATA;
	}

	if (stream->autoPaused)
	{
		/* A previous idle auto-pause stopped the capture but kept the
		 * (device-lifetime) critical section and response buffers alive;
		 * release the buffers so this fresh start works as usual. */
		stream->autoPaused = FALSE;
		stream->lastSampleRequestTime = 0;
		Stream_Free(stream->sampleRespBuffer, TRUE);
		stream->sampleRespBuffer = nullptr;
		Stream_Free(stream->pendingSample, TRUE);
		stream->pendingSample = nullptr;
		ecam_encoder_context_free(stream);
	}

	/* saving  media type description for CurrentMediaTypeRequest,
	 * to be done before calling ecam_encoder_context_init
	 */
	stream->currMediaType = mediaType;

	if (!ecam_encoder_context_init(stream))
	{
		WLog_ERR(TAG, "stream_ecam_encoder_init failed");
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_UnexpectedError);
		return ERROR_INVALID_DATA;
	}

	stream->sampleRespBuffer = Stream_New(nullptr, ECAM_SAMPLE_RESPONSE_BUFFER_SIZE);
	if (!stream->sampleRespBuffer)
	{
		WLog_ERR(TAG, "Stream_New failed");
		ecam_dev_stop_stream(dev, streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_OutOfMemory);
		return ERROR_INVALID_DATA;
	}

	/* replacing outputFormat with inputFormat in mediaType before starting stream */
	mediaType.Format = streamInputFormat(stream);

	stream->samplesRequested = 0;
	stream->haveSample = FALSE;

	stream->pendingSample = Stream_New(nullptr, 4ull * mediaType.Width * mediaType.Height);
	if (!stream->pendingSample)
	{
		WLog_ERR(TAG, "pending stream failed");
		ecam_dev_stop_stream(dev, streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_OutOfMemory);
		return ERROR_INVALID_DATA;
	}

	const CAM_ERROR_CODE error = dev->ihal->StartStream(dev->ihal, dev, streamIndex, &mediaType,
	                                                    ecam_dev_sample_captured_callback);
	if (error)
	{
		WLog_ERR(TAG, "StartStream failure");
		ecam_dev_stop_stream(dev, streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, error);
		return ERROR_INVALID_DATA;
	}

	stream->streaming = TRUE;
	stream->lastSampleRequestTime = GetTickCount64();
	return ecam_channel_send_generic_msg(dev->ecam, hchannel, CAM_MSG_ID_SuccessResponse);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_property_list_request(CameraDevice* dev,
                                                   GENERIC_CHANNEL_CALLBACK* hchannel,
                                                   WINPR_ATTR_UNUSED wStream* s)
{
	WINPR_ASSERT(dev);
	// TODO: supported properties implementation

	return ecam_channel_send_generic_msg(dev->ecam, hchannel, CAM_MSG_ID_PropertyListResponse);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_send_current_media_type_response(CameraDevice* dev,
                                                      GENERIC_CHANNEL_CALLBACK* hchannel,
                                                      CAM_MEDIA_TYPE_DESCRIPTION* mediaType)
{
	CAM_MSG_ID msg = CAM_MSG_ID_CurrentMediaTypeResponse;

	WINPR_ASSERT(dev);

	wStream* s = Stream_New(nullptr, CAM_HEADER_SIZE + sizeof(CAM_MEDIA_TYPE_DESCRIPTION));
	if (!s)
	{
		WLog_ERR(TAG, "Stream_New failed");
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, dev->ecam->version));
	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, msg));

	ecam_dev_write_media_type(s, mediaType);

	return ecam_channel_write(dev->ecam, hchannel, msg, s, TRUE);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_sample_request(CameraDevice* dev, GENERIC_CHANNEL_CALLBACK* hchannel,
                                            wStream* s)
{
	BYTE streamIndex = 0;

	WINPR_ASSERT(dev);

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 1))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT8(s, streamIndex);

	if (streamIndex >= ECAM_DEVICE_MAX_STREAMS)
	{
		WLog_ERR(TAG, "Incorrect streamIndex %u", streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_InvalidStreamNumber);
		return ERROR_INVALID_INDEX;
	}

	CameraDeviceStream* stream = &dev->streams[streamIndex];

	EnterCriticalSection(&stream->lock);

	/* A stream auto-paused by the idle timeout is transparently resumed when
	 * the server starts requesting samples again. */
	if (stream->autoPaused)
	{
		const UINT resumeError = ecam_dev_resume_stream(dev, streamIndex);
		if (resumeError != CHANNEL_RC_OK)
		{
			WLog_WARN(TAG, "Failed to resume auto-paused stream %s/%zu: 0x%08" PRIX32,
			          dev->deviceId, (size_t)streamIndex, resumeError);
			LeaveCriticalSection(&stream->lock);
			return resumeError;
		}
	}

	stream->lastSampleRequestTime = GetTickCount64();

	/* need to save channel because responses are asynchronous and coming from capture thread */
	if (stream->hSampleReqChannel != hchannel)
		stream->hSampleReqChannel = hchannel;

	stream->samplesRequested++;
	const UINT ret = ecam_dev_send_pending(dev, streamIndex, stream);

	LeaveCriticalSection(&stream->lock);
	return ret;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_current_media_type_request(CameraDevice* dev,
                                                        GENERIC_CHANNEL_CALLBACK* hchannel,
                                                        wStream* s)
{
	BYTE streamIndex = 0;

	WINPR_ASSERT(dev);

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 1))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT8(s, streamIndex);

	if (streamIndex >= ECAM_DEVICE_MAX_STREAMS)
	{
		WLog_ERR(TAG, "Incorrect streamIndex %u", streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_InvalidStreamNumber);
		return ERROR_INVALID_INDEX;
	}

	CameraDeviceStream* stream = &dev->streams[streamIndex];

	if (stream->currMediaType.Format == 0)
	{
		WLog_ERR(TAG, "Current media type unknown for streamIndex %u", streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_NotInitialized);
		return ERROR_DEVICE_REINITIALIZATION_NEEDED;
	}

	return ecam_dev_send_current_media_type_response(dev, hchannel, &stream->currMediaType);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_send_media_type_list_response(CameraDevice* dev,
                                                   GENERIC_CHANNEL_CALLBACK* hchannel,
                                                   CAM_MEDIA_TYPE_DESCRIPTION* mediaTypes,
                                                   size_t nMediaTypes)
{
	CAM_MSG_ID msg = CAM_MSG_ID_MediaTypeListResponse;

	WINPR_ASSERT(dev);

	wStream* s = Stream_New(nullptr, CAM_HEADER_SIZE + ECAM_MAX_MEDIA_TYPE_DESCRIPTORS *
	                                                       sizeof(CAM_MEDIA_TYPE_DESCRIPTION));
	if (!s)
	{
		WLog_ERR(TAG, "Stream_New failed");
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, dev->ecam->version));
	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, msg));

	for (size_t i = 0; i < nMediaTypes; i++, mediaTypes++)
	{
		ecam_dev_write_media_type(s, mediaTypes);
	}

	return ecam_channel_write(dev->ecam, hchannel, msg, s, TRUE);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_media_type_list_request(CameraDevice* dev,
                                                     GENERIC_CHANNEL_CALLBACK* hchannel, wStream* s)
{
	UINT error = CHANNEL_RC_OK;
	BYTE streamIndex = 0;
	CAM_MEDIA_TYPE_DESCRIPTION* mediaTypes = nullptr;
	size_t nMediaTypes = ECAM_MAX_MEDIA_TYPE_DESCRIPTORS;

	WINPR_ASSERT(dev);

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 1))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT8(s, streamIndex);

	if (streamIndex >= ECAM_DEVICE_MAX_STREAMS)
	{
		WLog_ERR(TAG, "Incorrect streamIndex %u", streamIndex);
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_InvalidStreamNumber);
		return ERROR_INVALID_INDEX;
	}
	CameraDeviceStream* stream = &dev->streams[streamIndex];

	mediaTypes =
	    (CAM_MEDIA_TYPE_DESCRIPTION*)calloc(nMediaTypes, sizeof(CAM_MEDIA_TYPE_DESCRIPTION));
	if (!mediaTypes)
	{
		WLog_ERR(TAG, "calloc failed");
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_OutOfMemory);
		return CHANNEL_RC_NO_MEMORY;
	}

	size_t nSupportedFormats = 0;
	const CAM_MEDIA_FORMAT_INFO* supportedFormats = getSupportedFormats(&nSupportedFormats);

	INT16 formatIndex =
	    dev->ihal->GetMediaTypeDescriptions(dev->ihal, dev->deviceId, streamIndex, supportedFormats,
	                                        nSupportedFormats, mediaTypes, &nMediaTypes);
	if (formatIndex == -1 || nMediaTypes == 0)
	{
		WLog_ERR(TAG, "Camera doesn't support any compatible video formats");
		ecam_channel_send_error_response(dev->ecam, hchannel, CAM_ERROR_CODE_ItemNotFound);
		error = ERROR_DEVICE_FEATURE_NOT_SUPPORTED;
		goto error;
	}

	stream->formats = supportedFormats[formatIndex];

	/* replacing inputFormat with outputFormat in mediaTypes before sending response */
	for (size_t i = 0; i < nMediaTypes; i++)
	{
		mediaTypes[i].Format = streamOutputFormat(stream);
		mediaTypes[i].Flags = CAM_MEDIA_TYPE_DESCRIPTION_FLAG_DecodingRequired;
	}

	if (stream->currMediaType.Format == 0)
	{
		/* saving 1st media type description for CurrentMediaTypeRequest */
		stream->currMediaType = mediaTypes[0];
	}

	error = ecam_dev_send_media_type_list_response(dev, hchannel, mediaTypes, nMediaTypes);

error:
	free(mediaTypes);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_send_stream_list_response(CameraDevice* dev,
                                               GENERIC_CHANNEL_CALLBACK* hchannel)
{
	CAM_MSG_ID msg = CAM_MSG_ID_StreamListResponse;

	WINPR_ASSERT(dev);

	wStream* s = Stream_New(nullptr, CAM_HEADER_SIZE + sizeof(CAM_STREAM_DESCRIPTION));
	if (!s)
	{
		WLog_ERR(TAG, "Stream_New failed");
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, dev->ecam->version));
	Stream_Write_UINT8(s, WINPR_ASSERTING_INT_CAST(uint8_t, msg));

	/* single stream description */
	Stream_Write_UINT16(s, CAM_STREAM_FRAME_SOURCE_TYPE_Color);
	Stream_Write_UINT8(s, CAM_STREAM_CATEGORY_Capture);
	Stream_Write_UINT8(s, TRUE /* Selected */);
	Stream_Write_UINT8(s, FALSE /* CanBeShared */);

	return ecam_channel_write(dev->ecam, hchannel, msg, s, TRUE);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_stream_list_request(CameraDevice* dev,
                                                 GENERIC_CHANNEL_CALLBACK* hchannel,
                                                 WINPR_ATTR_UNUSED wStream* s)
{
	return ecam_dev_send_stream_list_response(dev, hchannel);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_activate_device_request(CameraDevice* dev,
                                                     GENERIC_CHANNEL_CALLBACK* hchannel,
                                                     WINPR_ATTR_UNUSED wStream* s)
{
	WINPR_ASSERT(dev);
	CAM_ERROR_CODE errorCode = CAM_ERROR_CODE_None;

	if (dev->ihal->Activate(dev->ihal, dev->deviceId, &errorCode))
		return ecam_channel_send_generic_msg(dev->ecam, hchannel, CAM_MSG_ID_SuccessResponse);

	return ecam_channel_send_error_response(dev->ecam, hchannel, errorCode);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_process_deactivate_device_request(CameraDevice* dev,
                                                       GENERIC_CHANNEL_CALLBACK* hchannel,
                                                       wStream* s)
{
	WINPR_ASSERT(dev);
	WINPR_UNUSED(s);

	for (size_t i = 0; i < ECAM_DEVICE_MAX_STREAMS; i++)
		ecam_dev_stop_stream(dev, i);

	CAM_ERROR_CODE errorCode = CAM_ERROR_CODE_None;
	if (dev->ihal->Deactivate(dev->ihal, dev->deviceId, &errorCode))
		return ecam_channel_send_generic_msg(dev->ecam, hchannel, CAM_MSG_ID_SuccessResponse);

	return ecam_channel_send_error_response(dev->ecam, hchannel, errorCode);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)
{
	UINT error = CHANNEL_RC_OK;
	BYTE version = 0;
	BYTE messageId = 0;
	GENERIC_CHANNEL_CALLBACK* hchannel = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;

	if (!hchannel || !data)
		return ERROR_INVALID_PARAMETER;

	CameraDevice* dev = (CameraDevice*)hchannel->plugin;

	if (!dev)
		return ERROR_INTERNAL_ERROR;

	if (!Stream_CheckAndLogRequiredLength(TAG, data, CAM_HEADER_SIZE))
		return ERROR_NO_DATA;

	Stream_Read_UINT8(data, version);
	Stream_Read_UINT8(data, messageId);
	WLog_DBG(TAG, "ChannelId=%" PRIu32 ", MessageId=0x%02" PRIx8 ", Version=%d",
	         hchannel->channel_mgr->GetChannelId(hchannel->channel), messageId, version);

	switch (messageId)
	{
		case CAM_MSG_ID_ActivateDeviceRequest:
			error = ecam_dev_process_activate_device_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_DeactivateDeviceRequest:
			error = ecam_dev_process_deactivate_device_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_StreamListRequest:
			error = ecam_dev_process_stream_list_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_MediaTypeListRequest:
			error = ecam_dev_process_media_type_list_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_CurrentMediaTypeRequest:
			error = ecam_dev_process_current_media_type_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_PropertyListRequest:
			error = ecam_dev_process_property_list_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_StartStreamsRequest:
			error = ecam_dev_process_start_streams_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_StopStreamsRequest:
			error = ecam_dev_process_stop_streams_request(dev, hchannel, data);
			break;

		case CAM_MSG_ID_SampleRequest:
			error = ecam_dev_process_sample_request(dev, hchannel, data);
			break;

		default:
			WLog_WARN(TAG, "unknown MessageId=0x%02" PRIx8 "", messageId);
			error = ERROR_INVALID_DATA;
			ecam_channel_send_error_response(dev->ecam, hchannel,
			                                 CAM_ERROR_CODE_OperationNotSupported);
			break;
	}

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_on_open(WINPR_ATTR_UNUSED IWTSVirtualChannelCallback* pChannelCallback)
{
	WLog_DBG(TAG, "entered");
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_on_close(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* hchannel = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	WINPR_ASSERT(hchannel);

	CameraDevice* dev = (CameraDevice*)hchannel->plugin;
	WINPR_ASSERT(dev);

	WLog_DBG(TAG, "entered");

	for (size_t i = 0; i < ECAM_DEVICE_MAX_STREAMS; i++)
		ecam_dev_stop_stream(dev, i);

	/* make sure this channel is not used for sample responses */
	for (size_t i = 0; i < ECAM_DEVICE_MAX_STREAMS; i++)
		if (dev->streams[i].hSampleReqChannel == hchannel)
			dev->streams[i].hSampleReqChannel = nullptr;

	free(hchannel);
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT ecam_dev_on_new_channel_connection(IWTSListenerCallback* pListenerCallback,
                                               IWTSVirtualChannel* pChannel,
                                               WINPR_ATTR_UNUSED BYTE* Data,
                                               WINPR_ATTR_UNUSED BOOL* pbAccept,
                                               IWTSVirtualChannelCallback** ppCallback)
{
	GENERIC_LISTENER_CALLBACK* hlistener = (GENERIC_LISTENER_CALLBACK*)pListenerCallback;

	if (!hlistener || !hlistener->plugin)
		return ERROR_INTERNAL_ERROR;

	WLog_DBG(TAG, "entered");
	GENERIC_CHANNEL_CALLBACK* hchannel =
	    (GENERIC_CHANNEL_CALLBACK*)calloc(1, sizeof(GENERIC_CHANNEL_CALLBACK));

	if (!hchannel)
	{
		WLog_ERR(TAG, "calloc failed");
		return CHANNEL_RC_NO_MEMORY;
	}

	hchannel->iface.OnDataReceived = ecam_dev_on_data_received;
	hchannel->iface.OnOpen = ecam_dev_on_open;
	hchannel->iface.OnClose = ecam_dev_on_close;
	hchannel->plugin = hlistener->plugin;
	hchannel->channel_mgr = hlistener->channel_mgr;
	hchannel->channel = pChannel;
	*ppCallback = (IWTSVirtualChannelCallback*)hchannel;
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return CameraDevice pointer or nullptr in case of error
 */
CameraDevice* ecam_dev_create(CameraPlugin* ecam, const char* deviceId,
                              WINPR_ATTR_UNUSED const char* deviceName,
                              const char* channelName)
{
	WINPR_ASSERT(ecam);
	WINPR_ASSERT(ecam->hlistener);

	IWTSVirtualChannelManager* pChannelMgr = ecam->hlistener->channel_mgr;
	WINPR_ASSERT(pChannelMgr);

	WLog_DBG(TAG, "entered for %s", deviceId);

	CameraDevice* dev = (CameraDevice*)calloc(1, sizeof(CameraDevice));

	if (!dev)
	{
		WLog_ERR(TAG, "calloc failed");
		return nullptr;
	}

	dev->ecam = ecam;
	dev->ihal = ecam->ihal;
	strncpy(dev->deviceId, deviceId, sizeof(dev->deviceId) - 1);
	if (channelName)
		strncpy(dev->channelName, channelName, sizeof(dev->channelName) - 1);
	dev->hlistener = (GENERIC_LISTENER_CALLBACK*)calloc(1, sizeof(GENERIC_LISTENER_CALLBACK));

	if (!dev->hlistener)
	{
		free(dev);
		WLog_ERR(TAG, "calloc failed");
		return nullptr;
	}

	dev->hlistener->iface.OnNewChannelConnection = ecam_dev_on_new_channel_connection;
	dev->hlistener->plugin = (IWTSPlugin*)dev;
	dev->hlistener->channel_mgr = pChannelMgr;

	const char* listenerName = channelName ? channelName : deviceId;
	if (CHANNEL_RC_OK != pChannelMgr->CreateListener(pChannelMgr, listenerName, 0,
	                                                 &dev->hlistener->iface, &dev->listener))
	{
		free(dev->hlistener);
		free(dev);
		WLog_ERR(TAG, "CreateListener failed for %s", listenerName);
		return nullptr;
	}

	/* The stream critical sections live for the whole device lifetime.
	 * They are intentionally NOT created/destroyed on each stream start/stop,
	 * so the idle-check timer can safely lock any stream at any time. */
	for (size_t i = 0; i < ECAM_DEVICE_MAX_STREAMS; i++)
	{
		if (!InitializeCriticalSectionEx(&dev->streams[i].lock, 0, 0))
		{
			for (size_t j = 0; j < i; j++)
				DeleteCriticalSection(&dev->streams[j].lock);
			IFCALL(pChannelMgr->DestroyListener, pChannelMgr, dev->listener);
			free(dev->hlistener);
			free(dev);
			WLog_ERR(TAG, "InitializeCriticalSectionEx failed for %s", listenerName);
			return nullptr;
		}
	}

	return dev;
}

/**
 * Function description
 *
 * OBJECT_FREE_FN for devices hash table value
 *
 */
void ecam_dev_destroy(CameraDevice* dev)
{
	if (!dev)
		return;

	WLog_DBG(TAG, "entered for %s", dev->deviceId);

	if (dev->hlistener)
	{
		IWTSVirtualChannelManager* mgr = dev->hlistener->channel_mgr;
		if (mgr)
			IFCALL(mgr->DestroyListener, mgr, dev->listener);
	}

	free(dev->hlistener);

	for (size_t i = 0; i < ECAM_DEVICE_MAX_STREAMS; i++)
		ecam_dev_stop_stream(dev, i);
	for (size_t i = 0; i < ECAM_DEVICE_MAX_STREAMS; i++)
		DeleteCriticalSection(&dev->streams[i].lock);

	free(dev);
}
