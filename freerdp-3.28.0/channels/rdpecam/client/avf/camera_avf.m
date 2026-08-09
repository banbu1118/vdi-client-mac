/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * MS-RDPECAM Implementation, AVFoundation (macOS) Interface
 *
 * macOS 摄像头重定向后端。由于 FreeRDP 官方没有 macOS 摄像头后端
 * （仅 wmf/v4l/android），本文件用 AVFoundation 采集摄像头，并直接以
 * VideoToolbox 硬件编码器把 CVPixelBuffer 编码为 H.264（Annex B），
 * 通过 sampleCallback 交给 rdpecam 通道（H264 passthrough）传输。
 *
 * 为什么采集侧直连 VT 编码：
 *  - 不同 macOS 版本/设备摄像头交付的 NV12 数据范围不同（420f full range
 *    vs 420v video range，FourCC 标签甚至可能不可靠）。若先拷成裸 NV12
 *    字节再交给通用编码链路，CVPixelBuffer 自带的色彩元数据
 *    （kCVImageBufferYCbCrMatrixKey 等）会丢失，编码器只能猜测，导致
 *    服务器端解码紫绿偏色 + 横纹（macOS 15 实测复现）。
 *  - VTCompressionSessionEncodeFrame 直接消费 CVPixelBuffer，VideoToolbox
 *    读取 buffer 自带的色彩信息输出正确的 SPS VUI，绕开所有 YUV 猜测。
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
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

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

	/* VideoToolbox hardware H.264 encoder (directly consumes CVPixelBuffer so
	 * the buffer's color metadata is preserved and the SPS VUI is written
	 * correctly — see file header comment). */
	VTCompressionSessionRef encoder;
	BOOL encoderReady;
	OSStatus encoderStatus;

	/* reusable Annex-B H.264 buffer (start codes + NAL payloads, SPS/PPS
	 * prepended on keyframes) */
	BYTE* annexBuffer;
	size_t annexBufferSize;
	/* keyframe samples carry SPS/PPS; used to decide whether the parameter
	 * sets need to be written ahead of the IDR */
	UINT64 frameCount;

	/* pacing diagnostics */
	UINT64 encodedFrames;
	UINT64 encodeErrors;
};

typedef struct
{
	ICamHal iHal;

	wHashTable* streams; /* Index: deviceId, Value: CamAvfStream */

} CamAvfHal;

/**
 * VideoToolbox compression output callback. Runs on a VideoToolbox internal
 * thread. Converts each encoded sample from AVCC (length-prefixed) to Annex-B
 * H.264 (start-code prefixed, SPS/PPS prepended on keyframes) and hands it to
 * the channel's sample callback. The callback locks the stream critical
 * section itself, so this is thread-safe.
 */
static void cam_avf_vt_compression_output_callback(void* refcon, void* sourceFrameRefCon,
                                                   OSStatus status, VTEncodeInfoFlags infoFlags,
                                                   CMSampleBufferRef sampleBuffer)
{
	CamAvfStream* stream = (CamAvfStream*)refcon;
	(void)sourceFrameRefCon;
	(void)infoFlags;

	if (!stream)
		return;

	if (status != noErr || !sampleBuffer)
	{
		stream->encoderStatus = status;
		stream->encodeErrors++;
		return;
	}

	@autoreleasepool
	{
		/* 提取 SPS/PPS（关键帧前写入，保证解码器可启动） */
		CMVideoFormatDescriptionRef fmtDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
		const uint8_t* sps = NULL;
		const uint8_t* pps = NULL;
		size_t spsSize = 0, ppsSize = 0;
		BOOL haveSpsPps = FALSE;
		if (fmtDesc)
		{
			CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmtDesc, 0, &sps, &spsSize, NULL,
			                                                   NULL);
			CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmtDesc, 1, &pps, &ppsSize, NULL,
			                                                   NULL);
			haveSpsPps = (sps && pps && spsSize > 0 && ppsSize > 0);
		}

		/* 关键帧判定：kCMSampleAttachmentKey_NotSync 缺失或为 NO 即关键帧 */
		BOOL isKeyframe = FALSE;
		CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, 0);
		if (attachments && CFArrayGetCount(attachments) > 0)
		{
			CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
			CFBooleanRef notSync =
			    (CFBooleanRef)CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
			isKeyframe = (notSync == NULL) || !CFBooleanGetValue(notSync);
		}
		else
		{
			isKeyframe = TRUE;
		}

		CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
		if (!block)
			return;

		size_t lenAtOffset = 0, totalLen = 0;
		char* dataPtr = NULL;
		CMBlockBufferGetDataPointer(block, 0, &lenAtOffset, &totalLen, &dataPtr);
		if (!dataPtr || totalLen == 0)
			return;

		/* 预估容量：SPS/PPS + 数据 + 每 NAL 4 字节 start code 膨胀 */
		const size_t overhead = (haveSpsPps && isKeyframe) ? (spsSize + ppsSize) : 0;
		const size_t needed = overhead + totalLen + totalLen / 4 + 64;
		if (needed > stream->annexBufferSize)
		{
			BYTE* nb = (BYTE*)realloc(stream->annexBuffer, needed);
			if (!nb)
				return;
			stream->annexBuffer = nb;
			stream->annexBufferSize = needed;
		}

		BYTE* out = stream->annexBuffer;
		size_t outLen = 0;

		if (isKeyframe && haveSpsPps)
		{
			memcpy(out + outLen, "\x00\x00\x00\x01", 4);
			outLen += 4;
			memcpy(out + outLen, sps, spsSize);
			outLen += spsSize;
			memcpy(out + outLen, "\x00\x00\x00\x01", 4);
			outLen += 4;
			memcpy(out + outLen, pps, ppsSize);
			outLen += ppsSize;
		}

		/* AVCC（4 字节大端长度前缀）→ Annex B（start code + payload） */
		size_t offset = 0;
		while (offset + 4 <= totalLen)
		{
			const uint32_t nalLen =
			    ((uint32_t)(dataPtr[offset] & 0xFF) << 24) |
			    ((uint32_t)(dataPtr[offset + 1] & 0xFF) << 16) |
			    ((uint32_t)(dataPtr[offset + 2] & 0xFF) << 8) | (uint32_t)(dataPtr[offset + 3] & 0xFF);
			offset += 4;
			if (nalLen == 0 || offset + nalLen > totalLen)
				break;
			memcpy(out + outLen, "\x00\x00\x00\x01", 4);
			outLen += 4;
			memcpy(out + outLen, dataPtr + offset, nalLen);
			outLen += nalLen;
			offset += nalLen;
		}

		if (outLen == 0)
			return;

		stream->frameCount++;
		stream->encodedFrames++;

		if (stream->streaming && stream->sampleCallback)
		{
			const UINT error =
			    stream->sampleCallback(stream->dev, stream->streamIndex, stream->annexBuffer, outLen);
			if (error != CHANNEL_RC_OK)
				WLog_ERR(TAG, "Failure in sampleCallback: %" PRIu32, error);
		}
	}
}

@implementation CamAvfSampleDelegate

- (void)captureOutput:(AVCaptureOutput*)captureOutput
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
	(void)captureOutput;
	(void)connection;

	CamAvfStream* stream = self.stream;
	if (!stream)
		return;

	CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (!pixelBuffer)
		return;

	/* 请求 420f，但实际交付格式动态依赖 activeFormat（TN3121）：可能交付
	 * 420f 或 420v，甚至标签与数据范围不一致。不做任何拷贝/范围转换——
	 * CVPixelBuffer 自带正确色彩元数据，直接交给 VideoToolbox 编码器，
	 * VT 按附件写出正确的 SPS VUI（full/video range + BT.601/709）。 */
	const OSType fmt = CVPixelBufferGetPixelFormatType(pixelBuffer);
	if (fmt != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
	    fmt != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
		return;

	/* stream->lock 保护 encoder 指针：stop 路径（cam_avf_destroy_encoder）
	 * 会先置 encoderReady=FALSE 再把 encoder 置 NULL 并 Invalidate，这里
	 * 在锁内取指针并调用 EncodeFrame，避免读到已释放的 session。 */
	EnterCriticalSection(&stream->lock);
	if (!stream->streaming || !stream->encoderReady || !stream->encoder)
	{
		LeaveCriticalSection(&stream->lock);
		return;
	}
	VTCompressionSessionRef encoder = stream->encoder;

	/* 异步：VT 内部 retain pixelBuffer，采集线程不阻塞；编码帧在输出回调
	 * （VT 内部线程）投递。PTS 用采集时间戳保证帧序。 */
	const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
	const OSStatus st = VTCompressionSessionEncodeFrame(encoder, pixelBuffer, pts, kCMTimeInvalid,
	                                                    NULL, NULL, NULL);
	LeaveCriticalSection(&stream->lock);
	if (st != noErr)
	{
		stream->encoderStatus = st;
		stream->encodeErrors++;
	}
}

@end

/**
 * Create the VideoToolbox H.264 compression session for the stream.
 *
 * The encoder consumes the raw CVPixelBuffer delivered by AVFoundation, so the
 * buffer's own color metadata (matrix/range attachments written by the capture
 * pipeline) drives the SPS VUI — no YUV range guessing anywhere in the chain
 * (see the file header for why this fixes macOS 15 purple/green tinting).
 */
static BOOL cam_avf_create_encoder(CamAvfStream* stream)
{
	if (stream->encoder)
		return TRUE;

	@autoreleasepool
	{
		VTCompressionSessionRef session = NULL;
		const OSStatus st = VTCompressionSessionCreate(
		    NULL, (int32_t)stream->width, (int32_t)stream->height, kCMVideoCodecType_H264, NULL,
		    NULL, NULL, cam_avf_vt_compression_output_callback, stream, &session);
		if (st != noErr || !session)
		{
			WLog_ERR(TAG, "VTCompressionSessionCreate failed: %d", (int)st);
			return FALSE;
		}

		/* 实时性 + 低延迟（禁用 B 帧重排，避免解码端延迟） */
		VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
		                     kCFBooleanFalse);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
		                     kVTProfileLevel_H264_Main_AutoLevel);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
		                     (CFNumberRef)@(30));
		VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
		                     (CFNumberRef)@(1.0));

		/* VBR 码率（摄像头画面运动较多，给足避免模糊） */
		UINT32 bitrate = 350000;
		if (stream->height >= 720)
			bitrate = 2500 * 1000;
		else if (stream->height >= 480)
			bitrate = 1200 * 1000;
		else if (stream->height >= 360)
			bitrate = 700 * 1000;
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate,
		                     (CFNumberRef)@(bitrate));
		/* 突发上限 = 2x 平均码率 */
		const int64_t limitBytes = (int64_t)bitrate * 2 / 8;
		const double limitSecs = 1.0;
		CFNumberRef nums[2] = { (CFNumberRef)@(limitBytes), (CFNumberRef)@(limitSecs) };
		CFArrayRef limits =
		    CFArrayCreate(kCFAllocatorDefault, (const void**)nums, 2, &kCFTypeArrayCallBacks);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_DataRateLimits, limits);
		CFRelease(limits);

		/* 关键：不设置 PixelTransferProperties/色彩属性 —— VT 使用输入
		 * CVPixelBuffer 自带的 kCVImageBufferYCbCrMatrixKey 等附件输出正确
		 * 的 SPS VUI，兼容不同系统/设备交付的 full/video range。 */

		const OSStatus prep = VTCompressionSessionPrepareToEncodeFrames(session);
		if (prep != noErr)
		{
			WLog_ERR(TAG, "VTCompressionSessionPrepareToEncodeFrames failed: %d", (int)prep);
			VTCompressionSessionInvalidate(session);
			CFRelease(session);
			return FALSE;
		}

		stream->encoder = session;
		stream->encoderReady = TRUE;
		stream->encoderStatus = noErr;
		return TRUE;
	}
}

/**
 * Tear down the VideoToolbox compression session. Invalidate waits for any
 * in-flight output callbacks, so the stream structure stays valid.
 */
static void cam_avf_destroy_encoder(CamAvfStream* stream)
{
	if (!stream->encoder)
		return;

	/* 锁内先清状态再取走指针：captureOutput 在锁内检查 encoderReady/
	 * encoder，因此这里必须与它互斥，保证不会读到已失效的 session。 */
	VTCompressionSessionRef encoder;
	EnterCriticalSection(&stream->lock);
	stream->encoderReady = FALSE;
	encoder = stream->encoder;
	stream->encoder = NULL;
	LeaveCriticalSection(&stream->lock);

	/* Invalidate 等待所有在途输出回调结束，之后 stream 结构才安全释放 */
	VTCompressionSessionInvalidate(encoder);
	CFRelease(encoder);
	stream->encoderStatus = noErr;
}

/**
 * Bring the app to the foreground right before requesting camera access.
 *
 * qf-client runs as an LSUIElement (accessory) app with no Dock icon and is
 * normally not the active application. On macOS 13/15 TCCUI only presents the
 * permission prompt when the requesting app is the frontmost (active)
 * application, otherwise the request silently hangs and no entry appears in
 * System Settings > Privacy & Security. Activate synchronously on the main
 * thread (no-op when already on it) so the prompt can be presented.
 */
static void cam_avf_activate_app(void)
{
	dispatch_block_t block = ^{
		[[NSRunningApplication currentApplication]
		    activateWithOptions:NSApplicationActivateIgnoringOtherApps];
	};
	if ([NSThread isMainThread])
		block();
	else
		dispatch_sync(dispatch_get_main_queue(), block);
}

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

		/* NotDetermined: activate the app first so the TCC prompt is
		 * presented on macOS 13/15, then ask for permission synchronously */
		cam_avf_activate_app();

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
	/* 本后端在采集侧直接用 VideoToolbox 编码 H.264（Annex B）交付通道，
	 * 因此协商用 {H264,H264} passthrough 对（inputFormat=H264 表示
	 * sampleCallback 交付的已是 H.264，通道不再二次编码）；若该对不存在
	 * 则退回 {NV12,H264}。 */
	INT16 passthroughIndex = -1;
	INT16 nv12ToH264Index = -1;
	for (size_t i = 0; i < nSupportedFormats; i++)
	{
		if (supportedFormats[i].inputFormat == CAM_MEDIA_FORMAT_H264 &&
		    supportedFormats[i].outputFormat == CAM_MEDIA_FORMAT_H264)
			passthroughIndex = WINPR_ASSERTING_INT_CAST(INT16, i);
		if (supportedFormats[i].inputFormat == CAM_MEDIA_FORMAT_NV12 &&
		    supportedFormats[i].outputFormat == CAM_MEDIA_FORMAT_H264)
			nv12ToH264Index = WINPR_ASSERTING_INT_CAST(INT16, i);
	}
	const INT16 formatIndex =
	    (passthroughIndex >= 0) ? passthroughIndex : nv12ToH264Index;
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
		/* 已写入条目的基址（mediaTypes 指针会随写入递增），用于分辨率去重 */
		CAM_MEDIA_TYPE_DESCRIPTION* base = mediaTypes;

		for (AVCaptureDeviceFormat* format in device.formats)
		{
			CMVideoFormatDescriptionRef desc = format.formatDescription;
			CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(desc);

			/* 只上报真正的 NV12 格式：若把 MJPG/YUY2 等格式的分辨率伪装成
			 * NV12 上报给服务器，stream_start 时按宽高匹配 activeFormat 可能
			 * 无法精确命中，导致实际采集分辨率与协商分辨率不一致，编码画面
			 * 错位（左右双画面）。过滤后协商列表即真实可采集的 NV12 分辨率，
			 * 服务器选中后必然能匹配 activeFormat。 */
			const FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(desc);
			if (pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
			    pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
				continue;

			/* Cap resolution to keep the software encoder real-time */
			if ((UINT32)dims.width > CAM_AVF_MAX_WIDTH ||
			    (UINT32)dims.height > CAM_AVF_MAX_HEIGHT)
			{
				anyCapped = TRUE;
				continue;
			}

			/* 同一分辨率去重（FullRange/VideoRange 变体、多 fps 变体） */
			BOOL duplicated = FALSE;
			for (size_t i = 0; i < nTypes; i++)
			{
				if (base[i].Width == (UINT32)dims.width &&
				    base[i].Height == (UINT32)dims.height)
				{
					duplicated = TRUE;
					break;
				}
			}
			if (duplicated)
				continue;

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

			mediaTypes->Format = CAM_MEDIA_FORMAT_H264;
			mediaTypes->Width = (UINT32)dims.width;
			mediaTypes->Height = (UINT32)dims.height;
			mediaTypes->FrameRateNumerator = fps;
			mediaTypes->FrameRateDenominator = 1;
			mediaTypes->PixelAspectRatioNumerator = 1;
			mediaTypes->PixelAspectRatioDenominator = 1;
			mediaTypes->Flags = AM_MEDIA_TYPE_DESCRIPTION_FLAG_Invalid;

			WLog_DBG(TAG, "Camera media type: H264, %ux%u, %u fps", (UINT32)dims.width,
			         (UINT32)dims.height, fps);

			mediaTypes++;
			nTypes++;
			anyTypeAdded = TRUE;
			if (nTypes >= maxMediaTypes)
				break;
		}

		/* 兜底：若所有 NV12 格式都超过 720p 上限（罕见），回退找第一个
		 * NV12 格式；设备完全没有 NV12 时（更罕见）保持原行为用第一个格式，
		 * 避免无媒体类型。 */
		if (!anyTypeAdded && anyCapped)
		{
			AVCaptureDeviceFormat* first = nil;
			for (AVCaptureDeviceFormat* f in device.formats)
			{
				const FourCharCode pf = CMFormatDescriptionGetMediaSubType(f.formatDescription);
				if (pf == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
				    pf == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
				{
					first = f;
					break;
				}
			}
			if (!first)
				first = [device.formats firstObject];
			if (first)
			{
				CMVideoDimensions dims =
				    CMVideoFormatDescriptionGetDimensions(first.formatDescription);
				mediaTypes->Format = CAM_MEDIA_FORMAT_H264;
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
static CAM_ERROR_CODE cam_avf_stream_stop(CamAvfStream* stream);

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

	/* 本后端交付 H.264（VT 硬编），协商 inputFormat=H264。 */
	if (mediaType->Format != CAM_MEDIA_FORMAT_H264)
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

			/* 把 activeFormat 精确配置为请求的分辨率：必须同时匹配尺寸、
			 * 像素格式（420f 优先、420v 兜底）与帧率范围，避免选中同分辨率
			 * 的其它条目（如 420v/MJPEG），否则实际采集分辨率/格式与协商
			 * 不一致会导致画面错位（左右双画面）或格式不匹配。
			 * （WebRTC 案例与 Apple TN3121 均证实同分辨率多条目普遍存在。） */
			const int32_t targetFps = (mediaType->FrameRateNumerator > 0)
			                              ? (int32_t)mediaType->FrameRateNumerator
			                              : CAM_AVF_DEFAULT_FPS;
			AVCaptureDeviceFormat* selected = nil;
			BOOL selectedFullRange = FALSE;
			for (AVCaptureDeviceFormat* fmt in device.formats)
			{
				CMVideoDimensions dims =
				    CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
				if (dims.width != (int32_t)stream->width ||
				    dims.height != (int32_t)stream->height)
					continue;

				const FourCharCode pf = CMFormatDescriptionGetMediaSubType(fmt.formatDescription);
				if (pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
				    pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
					continue;

				/* 帧率范围需能覆盖目标帧率 */
				BOOL fpsOk = FALSE;
				for (AVFrameRateRange* range in fmt.videoSupportedFrameRateRanges)
				{
					if (targetFps >= (int32_t)range.minFrameRate &&
					    targetFps <= (int32_t)range.maxFrameRate)
					{
						fpsOk = TRUE;
						break;
					}
				}
				if (!fpsOk)
					continue;

				const BOOL isFullRange = (pf == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
				if (!selected || (isFullRange && !selectedFullRange))
				{
					selected = fmt;
					selectedFullRange = isFullRange;
				}
				if (selectedFullRange)
					break; /* 已选到 420f 即停止 */
			}

			if (selected)
			{
				if ([device lockForConfiguration:&error])
				{
					device.activeFormat = selected;
					device.activeVideoMinFrameDuration = CMTimeMake(1, targetFps);
					device.activeVideoMaxFrameDuration = CMTimeMake(1, targetFps);
					[device unlockForConfiguration];
				}
				CMVideoDimensions selDims =
				    CMVideoFormatDescriptionGetDimensions(selected.formatDescription);
				WLog_WARN(TAG, "activeFormat set: %ux%u %s (requested %ux%u @ %d fps)",
				          selDims.width, selDims.height,
				          selectedFullRange ? "420f" : "420v", (int)stream->width,
				          (int)stream->height, targetFps);
			}
			else
			{
				WLog_ERR(TAG, "No exact NV12 format match for %ux%u @ %d fps; using default",
				         (int)stream->width, (int)stream->height, targetFps);
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

	/* 创建 VideoToolbox 硬件编码器（必须在 startRunning 之后、streaming
	 * 置位之前，captureOutput 以 encoderReady 为门槛）。VT 消费
	 * CVPixelBuffer 自带的色彩元数据输出正确 SPS，见 cam_avf_create_encoder。 */
	if (!cam_avf_create_encoder(stream))
	{
		WLog_ERR(TAG, "Failed to create VideoToolbox encoder for %s", dev->deviceId);
		cam_avf_stream_stop(stream);
		return CAM_ERROR_CODE_UnexpectedError;
	}

	stream->streaming = TRUE;

	WLog_INFO(TAG, "Camera stream started: %s %ux%u @ %u fps (VideoToolbox)", dev->deviceId,
	          (unsigned int)mediaType->Width, (unsigned int)mediaType->Height,
	          mediaType->FrameRateNumerator);

	return CAM_ERROR_CODE_None;
}

static CAM_ERROR_CODE cam_avf_stream_stop(CamAvfStream* stream)
{
	if (!stream)
		return CAM_ERROR_CODE_NotInitialized;

	stream->streaming = FALSE;

	/* 先停采集（dispatch_sync 完成后不再有新帧入编码器），再销毁 VT 编码
	 * 器：Invalidate 会等待在途输出回调排空，stream 结构在此之后才安全。 */
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

	cam_avf_destroy_encoder(stream);

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

	free(stream->annexBuffer);

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
