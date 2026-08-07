/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, AVFoundation (macOS) Interface
 *
 * macOS 摄像头重定向后端。由于 FreeRDP 官方没有 macOS 摄像头后端
 * （仅 wmf/v4l/android），本文件用 AVFoundation 采集摄像头并以
 * NV12 连续帧上交给 rdpecam 通道，由通道内置编码器软编 H.264。
 *
 * Copyright 2026 vdi-client-mac project
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <winpr/synch.h>
#include <winpr/wtypes.h>

#include "camera.h"

#define TAG CHANNELS_TAG("rdpecam-avf.client")

#define CAM_AVF_DEFAULT_FPS 30
/* 软编 H.264（libopenh264）在采集线程同步执行，1080p/4K 会丢帧（卡）且
 * 超出默认 2Mbps 码率承载（糊）。限制上报分辨率到 720p，保证流畅与画质。 */
#define CAM_AVF_MAX_WIDTH 1280
#define CAM_AVF_MAX_HEIGHT 720

typedef struct s_CamAvfStream CamAvfStream;

/**
 * AVCaptureVideoDataOutput sample buffer delegate.
 * Calls ICamHalSampleCapturedCallback with a continuous NV12 frame.
 */
@interface CamAvfSampleDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) CamAvfStream* stream;
@end

struct s_CamAvfStream
{
	CRITICAL_SECTION lock;

	/* members used to call the callback */
	CameraDevice* dev;
	size_t streamIndex;
	WINPR_ATTR_NODISCARD ICamHalSampleCapturedCallback sampleCallback;

	BOOL streaming;
	UINT32 width;
	UINT32 height;

	AVCaptureSession* session;
	AVCaptureDeviceInput* input;
	AVCaptureVideoDataOutput* output;
	CamAvfSampleDelegate* delegate;
	dispatch_queue_t sessionQueue;
	dispatch_queue_t sampleQueue;

	/* reusable continuous NV12 buffer (Y plane + interleaved UV plane) */
	BYTE* sampleBuffer;
	size_t sampleBufferSize;
};

typedef struct
{
	ICamHal iHal;

	wHashTable* streams; /* Index: deviceId, Value: CamAvfStream */

} CamAvfHal;

@implementation CamAvfSampleDelegate

- (void)captureOutput:(AVCaptureOutput*)captureOutput
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
	(void)captureOutput;
	(void)connection;

	CamAvfStream* stream = self.stream;
	if (!stream || !stream->streaming)
		return;

	CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (!pixelBuffer)
		return;

	const OSType fmt = CVPixelBufferGetPixelFormatType(pixelBuffer);
	if (fmt != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
	    fmt != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
		return;

	CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

	const size_t width = CVPixelBufferGetWidth(pixelBuffer);
	const size_t height = CVPixelBufferGetHeight(pixelBuffer);
	const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
	const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
	const uint8_t* yBase = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
	const uint8_t* uvBase = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);

	/* NV12: Y plane (width*height) + interleaved CbCr plane (width*height/2) */
	const size_t needed = width * height + width * height / 2;
	if (needed > stream->sampleBufferSize)
	{
		BYTE* nb = (BYTE*)realloc(stream->sampleBuffer, needed);
		if (!nb)
		{
			CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
			return;
		}
		stream->sampleBuffer = nb;
		stream->sampleBufferSize = needed;
	}

	BYTE* dst = stream->sampleBuffer;
	if (yStride == width)
	{
		memcpy(dst, yBase, width * height);
		dst += width * height;
	}
	else
	{
		for (size_t r = 0; r < height; r++)
		{
			memcpy(dst, yBase + r * yStride, width);
			dst += width;
		}
	}

	const size_t uvRows = height / 2;
	if (uvStride == width)
	{
		memcpy(dst, uvBase, uvRows * width);
	}
	else
	{
		for (size_t r = 0; r < uvRows; r++)
		{
			memcpy(dst, uvBase + r * uvStride, width);
			dst += width;
		}
	}

	CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

	if (stream->sampleCallback)
	{
		const UINT error =
		    stream->sampleCallback(stream->dev, stream->streamIndex, stream->sampleBuffer, needed);
		if (error != CHANNEL_RC_OK)
			WLog_ERR(TAG, "Failure in sampleCallback: %" PRIu32, error);
	}
}

@end

/**
 * Function description
 *
 * @return TRUE if we are allowed to access the camera
 */
static BOOL cam_avf_ensure_authorization(void)
{
	/* Runs on the enumeration (DVC) thread which never drains an autorelease
	 * pool, so wrap all ObjC calls so autoreleased objects are freed here
	 * instead of lingering until the thread exits. */
	@autoreleasepool
	{
		AVAuthorizationStatus status =
		    [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
		if (status == AVAuthorizationStatusAuthorized)
			return TRUE;
		if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted)
			return FALSE;

		/* NotDetermined: ask for permission synchronously */
		__block BOOL granted = FALSE;
		dispatch_semaphore_t sem = dispatch_semaphore_create(0);
		[AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
		                         completionHandler:^(BOOL g) {
			                         granted = g;
			                         dispatch_semaphore_signal(sem);
		                         }];
		dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
		dispatch_release(sem);
		return granted;
	}
}

/**
 * Function description
 *
 * @return number of video capture devices
 */
static UINT cam_avf_enumerate(WINPR_ATTR_UNUSED ICamHal* ihal, ICamHalEnumCallback callback,
                              CameraPlugin* ecam, GENERIC_CHANNEL_CALLBACK* hchannel)
{
	CamAvfHal* hal = (CamAvfHal*)ihal;
	(void)hal;

	/* 无摄像头授权则直接返回 0（无设备，上层自动忽略） */
	if (!cam_avf_ensure_authorization())
		return 0;

	@autoreleasepool
	{
		/* macOS 上只有内建广角 + 外接设备可用（telephoto/ultra-wide 为 iOS only） */
		NSArray<AVCaptureDevice*>* devices =
		    [AVCaptureDeviceDiscoverySession
		        discoverySessionWithDeviceTypes:@[
			        AVCaptureDeviceTypeBuiltInWideAngleCamera,
			        AVCaptureDeviceTypeExternalUnknown,
		        ]
		                              mediaType:AVMediaTypeVideo
		                               position:AVCaptureDevicePositionUnspecified]
		        .devices;

		UINT count = 0;
		for (AVCaptureDevice* device in devices)
		{
			const char* uniqueId = device.uniqueID.UTF8String;
			if (!uniqueId)
				continue;

			const char* name = device.localizedName.UTF8String;
			if (!name)
				name = uniqueId;

			WLog_INFO(TAG, "found camera: %s (%s)", name, uniqueId);

			IFCALL(callback, ecam, hchannel, uniqueId, name);
			count++;
		}

		return count;
	}
}

/**
 * Function description
 *
 * @return 0 on success, otherwise -1
 */
static INT16 cam_avf_get_media_type_descriptions(
    WINPR_ATTR_UNUSED ICamHal* ihal, const char* deviceId, WINPR_ATTR_UNUSED size_t streamIndex,
    const CAM_MEDIA_FORMAT_INFO* supportedFormats, size_t nSupportedFormats,
    CAM_MEDIA_TYPE_DESCRIPTION* mediaTypes, size_t* nMediaTypes)
{
	INT16 formatIndex = -1;
	INT16 h264Index = -1;
	for (size_t i = 0; i < nSupportedFormats; i++)
	{
		if (supportedFormats[i].inputFormat != CAM_MEDIA_FORMAT_NV12)
			continue;
		if (formatIndex < 0)
			formatIndex = WINPR_ASSERTING_INT_CAST(INT16, i);
		if (supportedFormats[i].outputFormat == CAM_MEDIA_FORMAT_H264)
			h264Index = WINPR_ASSERTING_INT_CAST(INT16, i);
	}
	/* 优先选择 NV12 -> H264（软件编码传输），无编码器时退回 NV12 直传 */
	if (h264Index >= 0)
		formatIndex = h264Index;
	if (formatIndex < 0)
		return -1;

	size_t maxMediaTypes = *nMediaTypes;
	size_t nTypes = 0;

	@autoreleasepool
	{
		NSString* uniqueId = [NSString stringWithUTF8String:deviceId];
		AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:uniqueId];
		if (!device)
			return -1;

		BOOL anyCapped = FALSE;
		BOOL anyTypeAdded = FALSE;

		for (AVCaptureDeviceFormat* format in device.formats)
		{
			CMVideoFormatDescriptionRef desc = format.formatDescription;
			CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(desc);

			/* Cap resolution to keep the software encoder real-time */
			if ((UINT32)dims.width > CAM_AVF_MAX_WIDTH ||
			    (UINT32)dims.height > CAM_AVF_MAX_HEIGHT)
			{
				anyCapped = TRUE;
				continue;
			}

			/* pick the highest supported frame rate for this size */
			UINT32 fps = CAM_AVF_DEFAULT_FPS;
			for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
			{
				if ((UINT32)range.maxFrameRate >= CAM_AVF_DEFAULT_FPS)
				{
					fps = CAM_AVF_DEFAULT_FPS;
					break;
				}
				if ((UINT32)range.maxFrameRate > fps)
					fps = (UINT32)range.maxFrameRate;
			}

			mediaTypes->Format = CAM_MEDIA_FORMAT_NV12;
			mediaTypes->Width = (UINT32)dims.width;
			mediaTypes->Height = (UINT32)dims.height;
			mediaTypes->FrameRateNumerator = fps;
			mediaTypes->FrameRateDenominator = 1;
			mediaTypes->PixelAspectRatioNumerator = 1;
			mediaTypes->PixelAspectRatioDenominator = 1;
			mediaTypes->Flags = AM_MEDIA_TYPE_DESCRIPTION_FLAG_Invalid;

			WLog_DBG(TAG, "Camera media type: NV12, %ux%u, %u fps", (UINT32)dims.width,
			         (UINT32)dims.height, fps);

			mediaTypes++;
			nTypes++;
			anyTypeAdded = TRUE;
			if (nTypes >= maxMediaTypes)
				break;
		}

		/* 兜底：若所有格式都超过 720p 上限（罕见），回退到不过滤，避免无媒体类型 */
		if (!anyTypeAdded && anyCapped)
		{
			AVCaptureDeviceFormat* first = [device.formats firstObject];
			if (first)
			{
				CMVideoDimensions dims =
				    CMVideoFormatDescriptionGetDimensions(first.formatDescription);
				mediaTypes->Format = CAM_MEDIA_FORMAT_NV12;
				mediaTypes->Width = (UINT32)dims.width;
				mediaTypes->Height = (UINT32)dims.height;
				mediaTypes->FrameRateNumerator = CAM_AVF_DEFAULT_FPS;
				mediaTypes->FrameRateDenominator = 1;
				mediaTypes->PixelAspectRatioNumerator = 1;
				mediaTypes->PixelAspectRatioDenominator = 1;
				mediaTypes->Flags = AM_MEDIA_TYPE_DESCRIPTION_FLAG_Invalid;
				mediaTypes++;
				nTypes++;
			}
		}
	}

	*nMediaTypes = nTypes;
	return (nTypes > 0) ? formatIndex : -1;
}

/**
 * AVFoundation 没有设备独占/激活概念，设备随时可用。
 */
static BOOL cam_avf_activate(WINPR_ATTR_UNUSED ICamHal* ihal, WINPR_ATTR_UNUSED const char* deviceId,
                             CAM_ERROR_CODE* errorCode)
{
	if (errorCode)
		*errorCode = CAM_ERROR_CODE_None;
	return TRUE;
}

static BOOL cam_avf_deactivate(WINPR_ATTR_UNUSED ICamHal* ihal,
                               WINPR_ATTR_UNUSED const char* deviceId, CAM_ERROR_CODE* errorCode)
{
	if (errorCode)
		*errorCode = CAM_ERROR_CODE_None;
	return TRUE;
}

static CamAvfStream* cam_avf_stream_create(const char* deviceId, size_t streamIndex);
static void cam_avf_stream_free(void* obj);

/**
 * Function description
 *
 * @return CAM_ERROR_CODE_None on success
 */
static CAM_ERROR_CODE cam_avf_stream_start(ICamHal* ihal, CameraDevice* dev, size_t streamIndex,
                                           const CAM_MEDIA_TYPE_DESCRIPTION* mediaType,
                                           ICamHalSampleCapturedCallback callback)
{
	CamAvfHal* hal = (CamAvfHal*)ihal;
	WINPR_ASSERT(hal);

	if (mediaType->Format != CAM_MEDIA_FORMAT_NV12)
		return CAM_ERROR_CODE_InvalidMediaType;

	CamAvfStream* stream = (CamAvfStream*)HashTable_GetItemValue(hal->streams, dev->deviceId);
	if (!stream)
	{
		stream = cam_avf_stream_create(dev->deviceId, streamIndex);
		if (!stream)
			return CAM_ERROR_CODE_OutOfMemory;

		if (!HashTable_Insert(hal->streams, dev->deviceId, stream))
		{
			cam_avf_stream_free(stream);
			return CAM_ERROR_CODE_UnexpectedError;
		}
	}

	if (stream->streaming)
		return CAM_ERROR_CODE_UnexpectedError;

	stream->dev = dev;
	stream->streamIndex = streamIndex;
	stream->sampleCallback = callback;
	stream->width = mediaType->Width;
	stream->height = mediaType->Height;

	WLog_WARN(TAG, "Camera stream start: %ux%u @ %u fps",
	          (UINT32)mediaType->Width, (UINT32)mediaType->Height,
	          (UINT32)mediaType->FrameRateNumerator);

	__block CAM_ERROR_CODE result = CAM_ERROR_CODE_UnexpectedError;

	dispatch_sync(stream->sessionQueue, ^{
		@autoreleasepool
		{
			NSString* uniqueId = [NSString stringWithUTF8String:dev->deviceId];
			AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:uniqueId];
			if (!device)
			{
				result = CAM_ERROR_CODE_ItemNotFound;
				return;
			}

			NSError* error = nil;
			AVCaptureDeviceInput* input =
			    [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
			if (!input)
			{
				WLog_ERR(TAG, "deviceInputWithDevice failed for %s", dev->deviceId);
				result = CAM_ERROR_CODE_UnexpectedError;
				return;
			}

			AVCaptureSession* session = [[AVCaptureSession alloc] init];
			if (![session canAddInput:input])
			{
				/* input is autoreleased (+0) and not owned by us — do not
				 * release it; only release the session we allocated. */
				[session release];
				result = CAM_ERROR_CODE_UnexpectedError;
				return;
			}
			[session addInput:input];

			/* 尝试把设备 activeFormat 配成请求的分辨率 */
			for (AVCaptureDeviceFormat* fmt in device.formats)
			{
				CMVideoDimensions dims =
				    CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
				if (dims.width == (int32_t)stream->width &&
				    dims.height == (int32_t)stream->height)
				{
					if ([device lockForConfiguration:&error])
					{
						device.activeFormat = fmt;
						const int32_t fps =
						    (mediaType->FrameRateNumerator > 0)
						        ? (int32_t)mediaType->FrameRateNumerator
						        : CAM_AVF_DEFAULT_FPS;
						device.activeVideoMinFrameDuration = CMTimeMake(1, fps);
						device.activeVideoMaxFrameDuration = CMTimeMake(1, fps);
						[device unlockForConfiguration];
					}
					break;
				}
			}

			AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
			output.videoSettings = @{
				(id)kCVPixelBufferPixelFormatTypeKey :
				    @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
			};
			output.alwaysDiscardsLateVideoFrames = YES;

			CamAvfSampleDelegate* delegate = [[CamAvfSampleDelegate alloc] init];
			delegate.stream = stream;
			[output setSampleBufferDelegate:delegate queue:stream->sampleQueue];

			if (![session canAddOutput:output])
			{
				[delegate release];
				[output release];
				[session release];
				result = CAM_ERROR_CODE_UnexpectedError;
				return;
			}
			[session addOutput:output];

			[session startRunning];
			if (![session isRunning])
			{
				WLog_ERR(TAG, "AVCaptureSession failed to start");
				[delegate release];
				[output release];
				[session release];
				result = CAM_ERROR_CODE_UnexpectedError;
				return;
			}

			stream->input = input;
			stream->output = output;
			stream->session = session;
			stream->delegate = delegate;
			result = CAM_ERROR_CODE_None;
		}
	});

	if (result != CAM_ERROR_CODE_None)
		return result;

	stream->streaming = TRUE;

	WLog_INFO(TAG, "Camera stream started: %s %ux%u @ %u fps", dev->deviceId,
	          (unsigned int)mediaType->Width, (unsigned int)mediaType->Height,
	          mediaType->FrameRateNumerator);

	return CAM_ERROR_CODE_None;
}

static CAM_ERROR_CODE cam_avf_stream_stop(CamAvfStream* stream)
{
	if (!stream)
		return CAM_ERROR_CODE_NotInitialized;

	stream->streaming = FALSE;

	dispatch_sync(stream->sessionQueue, ^{
		@autoreleasepool
		{
			if (stream->session)
			{
				if ([stream->session isRunning])
					[stream->session stopRunning];

				/* Detach the AVFoundation objects and defer their release to
				 * the main queue. AVCaptureSession schedules main-thread
				 * deliveries (performSelectorOnMainThread) while stopping;
				 * releasing the session from this thread would deallocate it
				 * while the main run loop still holds a pending reference,
				 * causing a dangling objc_release crash on the main thread.
				 * Releasing on the main queue runs after those deliveries.
				 *
				 * NOTE: the AVCaptureDeviceInput is NOT released here — it is
				 * an autoreleased (+0) object we do not own; its only strong
				 * reference is the session's addInput retain, released when
				 * the session deallocates. Releasing it would over-release. */
				AVCaptureSession* session = stream->session;
				AVCaptureVideoDataOutput* output = stream->output;
				CamAvfSampleDelegate* delegate = stream->delegate;
				stream->session = nil;
				stream->input = nil;
				stream->output = nil;
				stream->delegate = nil;
				dispatch_async(dispatch_get_main_queue(), ^{
					@autoreleasepool
					{
						[session release];
						[output release];
						[delegate release];
					}
				});
			}
		}
	});

	return CAM_ERROR_CODE_None;
}

static CAM_ERROR_CODE cam_avf_stream_stop_by_device_id(ICamHal* ihal, const char* deviceId,
                                                       WINPR_ATTR_UNUSED size_t streamIndex)
{
	CamAvfHal* hal = (CamAvfHal*)ihal;

	CamAvfStream* stream = (CamAvfStream*)HashTable_GetItemValue(hal->streams, deviceId);

	if (!stream)
		return CAM_ERROR_CODE_NotInitialized;

	return cam_avf_stream_stop(stream);
}

/**
 * Function description
 *
 * OBJECT_FREE_FN for streams hash table value
 */
static void cam_avf_stream_free(void* obj)
{
	CamAvfStream* stream = (CamAvfStream*)obj;
	if (!stream)
		return;

	cam_avf_stream_stop(stream);

	free(stream->sampleBuffer);

	DeleteCriticalSection(&stream->lock);

	if (stream->sessionQueue)
		dispatch_release(stream->sessionQueue);
	if (stream->sampleQueue)
		dispatch_release(stream->sampleQueue);

	free(stream);
}

/**
 * Function description
 *
 * @return Null on failure, otherwise pointer to new CamAvfStream
 */
static CamAvfStream* cam_avf_stream_create(const char* deviceId, size_t streamIndex)
{
	(void)deviceId;

	CamAvfStream* stream = (CamAvfStream*)calloc(1, sizeof(CamAvfStream));
	if (!stream)
		return nullptr;

	stream->streamIndex = streamIndex;
	stream->streaming = FALSE;
	InitializeCriticalSection(&stream->lock);

	stream->sessionQueue = dispatch_queue_create("org.freerdp.rdpecam.avf.session", NULL);
	stream->sampleQueue = dispatch_queue_create("org.freerdp.rdpecam.avf.sample", NULL);
	if (!stream->sessionQueue || !stream->sampleQueue)
	{
		cam_avf_stream_free(stream);
		return nullptr;
	}

	return stream;
}

/**
 * Function description
 *
 * @return CAM_ERROR_CODE_None on success
 */
static CAM_ERROR_CODE cam_avf_free(ICamHal* ihal)
{
	CamAvfHal* hal = (CamAvfHal*)ihal;

	if (hal == nullptr)
		return CAM_ERROR_CODE_NotInitialized;

	HashTable_Free(hal->streams);

	free(hal);

	return CAM_ERROR_CODE_None;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
FREERDP_ENTRY_POINT(UINT VCAPITYPE avf_freerdp_rdpecam_client_subsystem_entry(
    PFREERDP_CAMERA_HAL_ENTRY_POINTS pEntryPoints))
{
	UINT ret = ERROR_INTERNAL_ERROR;
	WINPR_ASSERT(pEntryPoints);

	CamAvfHal* hal = (CamAvfHal*)calloc(1, sizeof(CamAvfHal));

	if (hal == nullptr)
		return CHANNEL_RC_NO_MEMORY;

	hal->iHal.Enumerate = cam_avf_enumerate;
	hal->iHal.GetMediaTypeDescriptions = cam_avf_get_media_type_descriptions;
	hal->iHal.Activate = cam_avf_activate;
	hal->iHal.Deactivate = cam_avf_deactivate;
	hal->iHal.StartStream = cam_avf_stream_start;
	hal->iHal.StopStream = cam_avf_stream_stop_by_device_id;
	hal->iHal.Free = cam_avf_free;

	hal->streams = HashTable_New(FALSE);
	if (!hal->streams)
		goto error;

	if (!HashTable_SetupForStringData(hal->streams, FALSE))
		goto error;

	wObject* obj = HashTable_ValueObject(hal->streams);
	WINPR_ASSERT(obj);
	obj->fnObjectFree = cam_avf_stream_free;

	ret = pEntryPoints->pRegisterCameraHal(pEntryPoints->plugin, &hal->iHal);
	if (ret != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "RegisterCameraHal failed with error %" PRIu32 "", ret);
		goto error;
	}

	return ret;

error:
	cam_avf_free(&hal->iHal);
	return ret;
}
