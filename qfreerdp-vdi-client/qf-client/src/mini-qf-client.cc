#include "freerdp/codec/region.h"
#include "freerdp/event.h"
#include "freerdp/gdi/gdi.h"
#include "freerdp/settings_keys.h"
#include <cwchar>
#include <freerdp/addin.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/rdpecam.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/channels/audin.h>
#include <freerdp/utils/cliprdr_utils.h>
#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/client/disp.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/graphics.h>
#include <freerdp/settings.h>
#include <freerdp/types.h>
#include <freerdp/update.h>

/* POSIX network headers (replaces winsock2.h / ws2tcpip.h on Windows) */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

/* macOS: 解析可执行文件路径，用于定位打包的 OpenSSL legacy provider */
#include <climits>
#include <mach-o/dyld.h>
#include <unistd.h>

/* WinPR provides a portable WaitForMultipleObjects / Sleep on POSIX */
#include <winpr/synch.h>
#include <winpr/string.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <QGuiApplication>
#include <QCursor>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <QClipboard>
#include <QBuffer>
#include <QMimeData>
#include <QTimer>
#include <QDir>
#include <map>

#include "qf_util.h"
#include "qf_log.h"
#include "rdp-view-item.h"

static std::atomic<bool> g_stopped = false;
static RdpViewItem* g_rdpViewItem = nullptr;
static std::unique_ptr<std::thread> g_freerdp_thread = nullptr;
static int g_cli_argc = 0;
static char** g_cli_argv = nullptr;
static bool g_cli_parsed = false;
/* Resolution passed via command-line /w: and /h: (0 = not specified) */
static uint32_t g_cli_width = 0;
static uint32_t g_cli_height = 0;
static std::vector<std::string> g_saved_drive_args;  // 保存的 /drive: 参数，用于重连时恢复
static std::vector<std::string> g_raw_drive_args;    // main() 里过滤出的原始 /drive: 参数（不含前缀），统一由 qf 展开挂载

/* 展开磁盘重定向路径中的环境变量 / 主目录占位符：
 *   $HOME / ${HOME} / % / ~ -> 用户主目录；$VAR / ${VAR} -> 环境变量值。
 * FreeRDP 不会展开 /drive:name,path 里的路径，macOS 下服务器下发的 $HOME
 * 若不展开就是字面量路径，设备会因目录不存在被静默跳过。 */
static std::string qf_expand_drive_path(const std::string& arg)
{
	std::string name;
	std::string path = arg;
	auto comma = path.find(',');
	if (comma != std::string::npos)
	{
		name = path.substr(0, comma + 1); /* 含逗号 */
		path = path.substr(comma + 1);
	}

	std::string result;
	result.reserve(path.size() + 64);
	for (size_t i = 0; i < path.size();)
	{
		const char c = path[i];
		if (c == '$')
		{
			size_t j = i + 1;
			const bool brace = (j < path.size() && path[j] == '{');
			if (brace)
				++j;
			const size_t start = j;
			while (j < path.size() &&
			       (std::isalnum(static_cast<unsigned char>(path[j])) || path[j] == '_'))
				++j;
			if (j > start)
			{
				std::string var = path.substr(start, j - start);
				size_t consumed = j;
				if (brace && j < path.size() && path[j] == '}')
					++consumed;
				const char* val = getenv(var.c_str());
				if (val)
					result += val;
				else
					result += path.substr(i, consumed - i);
				i = consumed;
				continue;
			}
		}
		else if (c == '%')
		{
			if (i + 1 < path.size() && path[i + 1] == '%')
			{
				result += '%';
				i += 2;
				continue;
			}
			const char* home = getenv("HOME");
			result += home ? home : "";
			++i;
			continue;
		}
		else if (c == '~' && i == 0 &&
		         (i + 1 >= path.size() || path[i + 1] == '/'))
		{
			const char* home = getenv("HOME");
			result += home ? home : "~";
			++i;
			continue;
		}
		result += c;
		++i;
	}
	return name + result;
}
static std::shared_ptr<qf::client_t> g_client = {};
static std::atomic<bool> g_reconnectRequested{false};
static freerdp* g_instance = nullptr;
static CliprdrClientContext* g_clipboard_client_context = nullptr;
static DispClientContext* g_dispContext = nullptr;
static RdpgfxClientContext* g_gfxContext = nullptr;

/* =====================================================================
 * RDPECLIP 文件传输（剪贴板拷贝文件）
 *
 * 协议要点（MS-RDPECLIP）：
 *  - 文件列表走标准格式 CF_HDROP(15)，数据为 CLIPRDR_FILELIST（Packed File
 *    List，由 FILEDESCRIPTORW 数组序列化而来，见 cliprdr_serialize_file_list）。
 *  - 文件内容走 CLIPRDR_FILECONTENTS_REQUEST / RESPONSE PDU：
 *      FILECONTENTS_SIZE (0x1)  请求文件大小，返回 8 字节 UINT64（小端）
 *      FILECONTENTS_RANGE (0x2) 请求 [nPosition, nPosition+cbRequested) 数据
 *  - 需在 capability 中声明 CB_STREAM_FILECLIP_ENABLED 等标志，服务器才会启用。
 *  - 注：FreeRDP 3.x 的 ServerFormatDataRequest 不解析请求体里的 FILECONTENTS
 *    结构（requestedFormatData 恒为空），故老式 FormatDataRequest("FileContents")
 *    流程不可用，文件内容只能走 FILECONTENTS PDU。
 * ===================================================================== */
static constexpr UINT32 QF_FILECONTENTS_CHUNK = 1024U * 1024U; /* 单块 1MB */

static inline void qf_utf8_to_wchar(const char* utf8, size_t utf8_len, WCHAR* out, size_t out_cap);

/* 远程文件接收目录：~/Library/Caches/qf-client/cliprdr */
static QString qf_cliprdr_recv_dir()
{
	QString dir = QDir::homePath() + "/Library/Caches/qf-client/cliprdr";
	QDir().mkpath(dir);
	return dir;
}

/* 把本地剪贴板文件列表序列化为 CLIPRDR_FILELIST（返回 buffer，调用方 free） */
static BYTE* qf_serialize_local_file_list(UINT32* out_len)
{
	*out_len = 0;
	std::lock_guard<std::mutex> lock(g_client->local_files_mutex_);
	if (g_client->local_files_.empty())
		return nullptr;

	const size_t count = g_client->local_files_.size();
	std::vector<FILEDESCRIPTORW> descs(count);
	for (size_t i = 0; i < count; ++i)
	{
		const auto& f = g_client->local_files_[i];
		FILEDESCRIPTORW& d = descs[i];
		memset(&d, 0, sizeof(d));
		d.dwFlags = FD_FILESIZE | FD_ATTRIBUTES | FD_WRITESTIME;
		d.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		d.nFileSizeLow = static_cast<UINT32>(f.size & 0xFFFFFFFF);
		d.nFileSizeHigh = static_cast<UINT32>((f.size >> 32) & 0xFFFFFFFF);
		QByteArray name8 = f.name.toUtf8();
		qf_utf8_to_wchar(name8.constData(), name8.size(), d.cFileName, ARRAYSIZE(d.cFileName));
	}

	BYTE* data = nullptr;
	UINT32 len = 0;
	if (cliprdr_serialize_file_list(descs.data(), static_cast<UINT32>(count), &data, &len) !=
	    CHANNEL_RC_OK)
		return nullptr;
	*out_len = len;
	return data;
}

/* 读取本地文件 [offset, offset+cbRequested) 到新分配的 buffer（调用方 free） */
static BYTE* qf_read_local_file(const QString& path, qint64 offset, UINT32 cbRequested,
                                UINT32* out_len)
{
	*out_len = 0;
	FILE* fp = fopen(path.toUtf8().constData(), "rb");
	if (!fp)
		return nullptr;
	BYTE* buf = static_cast<BYTE*>(malloc(cbRequested));
	if (!buf)
	{
		fclose(fp);
		return nullptr;
	}
	if (fseeko(fp, static_cast<off_t>(offset), SEEK_SET) != 0)
	{
		free(buf);
		fclose(fp);
		return nullptr;
	}
	*out_len = static_cast<UINT32>(fread(buf, 1, cbRequested, fp));
	fclose(fp);
	return buf;
}

/* MultiByteToWideChar 在 winpr 里不可用，用 winpr 的转换（见 qf_util 依赖） */
static inline void qf_utf8_to_wchar(const char* utf8, size_t utf8_len, WCHAR* out, size_t out_cap)
{
	ConvertUtf8NToWChar(utf8, utf8_len, out, out_cap);
	if (out_cap > 0)
		out[out_cap - 1] = 0;
}

/* 解析服务器下发的文件列表。Windows 服务器返回 CLIPRDR_FILELIST（4 字节 cItems +
 * FILEDESCRIPTORW 数组），个别实现直接返回裸 FILEDESCRIPTORW 数组，这里兼容两者。
 * 返回的数组由调用方 free()。 */
static bool qf_parse_remote_file_list(const BYTE* data, UINT32 dataLen, FILEDESCRIPTORW** out,
                                      UINT32* count)
{
	*out = nullptr;
	*count = 0;
	if (!data || dataLen == 0)
		return false;

	UINT rc = cliprdr_parse_file_list(data, dataLen, out, count);
	if (rc == CHANNEL_RC_OK && *out && *count > 0)
		return true;
	free(*out);
	*out = nullptr;
	*count = 0;

	if (dataLen >= sizeof(FILEDESCRIPTORW) && (dataLen % sizeof(FILEDESCRIPTORW)) == 0)
	{
		const UINT32 n = dataLen / static_cast<UINT32>(sizeof(FILEDESCRIPTORW));
		FILEDESCRIPTORW* arr =
		    static_cast<FILEDESCRIPTORW*>(calloc(n, sizeof(FILEDESCRIPTORW)));
		if (!arr)
			return false;
		memcpy(arr, data, dataLen);
		*out = arr;
		*count = n;
		return true;
	}
	return false;
}

/* =====================================================================
 * Resize debug tracking — helps diagnose why dynamic resolution
 * sometimes fails to take effect.
 * ===================================================================== */
static std::chrono::steady_clock::time_point g_last_disp_send_ts{};
static std::atomic<int> g_pending_resize_count{0};
static std::atomic<uint32_t> g_last_disp_w{0};
static std::atomic<uint32_t> g_last_disp_h{0};
static std::atomic<uint32_t> g_last_gfx_resize_w{0};
static std::atomic<uint32_t> g_last_gfx_resize_h{0};

/* =====================================================================
 * RDP Pointer (Cursor) Support
 * ===================================================================== */
static std::unordered_map<rdpPointer*, QCursor> g_pointer_cache;
static std::mutex g_pointer_mutex;
static QCursor g_blank_cursor;
static std::atomic<bool> g_cursor_hidden{false}; /* SYSPTR_NULL active */
static QCursor g_last_rdp_cursor;                 /* last non-null cursor */
static std::atomic<uint64_t> g_last_mouse_move_ms{0}; /* steady_clock ms */

/* Window devicePixelRatio cached on the GUI thread (Retina: 2.0).
 * Used to size RDP pointers correctly — without it a 32x32 px cursor is
 * interpreted as 32 points (= 64 physical px) and appears twice as large. */
static std::atomic<double> g_pointer_dpr{1.0};

/* Position tracking for hover restore threshold. */
static constexpr double CURSOR_RESTORE_THRESHOLD = 3.0;
static double g_hide_pos_x = -9999;
static double g_hide_pos_y = -9999;

static void ensure_blank_cursor()
{
	static bool done = false;
	if (!done)
	{
		QPixmap blank(1, 1);
		blank.fill(Qt::transparent);
		g_blank_cursor = QCursor(blank);
		done = true;
	}
}

/**
 * Called from RdpViewItem::mouseMoveEvent / hoverMoveEvent (GUI thread)
 * on every mouse move.  If the cursor was hidden by the server (SYSPTR_NULL),
 * restore it and record the timestamp so my_pointer_setnull won't
 * immediately re-hide it.
 */
void rdp_notify_mouse_moved(double qx, double qy)
{
	if (!g_cursor_hidden.load(std::memory_order_acquire))
		return;

	/* Don't restore for tiny movements — require a minimum distance
	 * from the position where setnull hid the cursor. */
	double dx = qx - g_hide_pos_x;
	double dy = qy - g_hide_pos_y;
	if ((dx * dx + dy * dy) < CURSOR_RESTORE_THRESHOLD * CURSOR_RESTORE_THRESHOLD)
		return;

	/* Cursor was hidden AND moved far enough — restore it */
	auto now = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	              now.time_since_epoch())
	              .count();
	g_last_mouse_move_ms.store(ms, std::memory_order_release);

	g_cursor_hidden.store(false, std::memory_order_release);
	g_rdpViewItem->setCursor(g_last_rdp_cursor);
}

static BOOL my_pointer_new(rdpContext* context, rdpPointer* pointer)
{
	rdpGdi* gdi = context->gdi;

	const uint32_t w = pointer->width;
	const uint32_t h = pointer->height;

	if (w == 0 || h == 0 || w > 512 || h > 512)
	{
		return TRUE;
	}

	/* Allocate temporary buffer for decoded pixel data (BGRA32 = 4 Bpp) */
	const uint32_t stride = w * 4;
	const size_t bufSize = static_cast<size_t>(stride) * h;
	BYTE* data = static_cast<BYTE*>(winpr_aligned_malloc(bufSize ? bufSize : 1, 16));
	if (!data)
	{
		qf::log::error("rdp/pointer", "alloc {} bytes failed", bufSize);
		return FALSE;
	}

	/* Use FreeRDP's official pointer decoder — handles all XOR/AND formats */
	if (!freerdp_image_copy_from_pointer_data(
	        data, PIXEL_FORMAT_BGRA32, stride, 0, 0, w, h,
	        pointer->xorMaskData, pointer->lengthXorMask,
	        pointer->andMaskData, pointer->lengthAndMask,
	        pointer->xorBpp, &gdi->palette))
	{
		qf::log::warn("rdp/pointer", "freerdp_image_copy_from_pointer_data failed");
		winpr_aligned_free(data);
		return TRUE; /* skip, not fatal */
	}

	/* Wrap decoded data in QImage → QPixmap → QCursor (QPixmap copies).
	 * On Retina (dpr=2.0) the pixmap must carry the window's devicePixelRatio
	 * and the hotspot must be divided by it, otherwise a 32x32 px pointer is
	 * rendered as 32 points (= 64 physical px) — twice the intended size. */
	const double dpr = g_pointer_dpr.load();
	QImage image(data, static_cast<int>(w), static_cast<int>(h),
	             static_cast<int>(stride), QImage::Format_ARGB32);
	QPixmap pm = QPixmap::fromImage(image);
	if (dpr > 1.0)
	{
		pm.setDevicePixelRatio(dpr);
	}
	QCursor cursor(pm,
	               static_cast<int>(pointer->xPos / dpr),
	               static_cast<int>(pointer->yPos / dpr));

	{
		std::lock_guard<std::mutex> lock(g_pointer_mutex);
		g_pointer_cache[pointer] = cursor;
	}

	qf::log::info("rdp/pointer", "pointer {}x{}@{}bpp, hotspot ({},{}), dpr={}",
	              w, h, pointer->xorBpp, pointer->xPos, pointer->yPos, dpr);

	winpr_aligned_free(data);

	return TRUE;
}

static void my_pointer_free(rdpContext* context, rdpPointer* pointer)
{
	WINPR_UNUSED(context);
	std::lock_guard<std::mutex> lock(g_pointer_mutex);
	g_pointer_cache.erase(pointer);
}

static BOOL my_pointer_set(rdpContext* context, rdpPointer* pointer)
{
	WINPR_UNUSED(context);
	QMetaObject::invokeMethod(g_rdpViewItem,
		[ptr = pointer]()
		{
			std::lock_guard<std::mutex> lock(g_pointer_mutex);
			auto it = g_pointer_cache.find(ptr);
			if (it != g_pointer_cache.end())
			{
				g_last_rdp_cursor = it->second;
				g_cursor_hidden.store(false, std::memory_order_release);
				g_rdpViewItem->setCursor(it->second);
			}
			else
			{
				qf::log::warn("rdp/pointer", "set: cursor {} not in cache", fmt::ptr(ptr));
			}
		},
		Qt::QueuedConnection);
	return TRUE;
}

static BOOL my_pointer_setnull(rdpContext* context)
{
	WINPR_UNUSED(context);

	/* If the user moved the mouse within the last 2 seconds, don't hide the
	 * cursor. */
	{
		auto now = std::chrono::steady_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		              now.time_since_epoch())
		              .count();
		auto last = g_last_mouse_move_ms.load(std::memory_order_acquire);
		if (last != 0 && ms - last < 2000)
		{
			return TRUE;
		}
	}

	g_cursor_hidden.store(true, std::memory_order_release);
	/* ensure_blank_cursor() already ran in start_rdp_connection() on main thread */
	QMetaObject::invokeMethod(g_rdpViewItem,
		[]()
		{
			/* Record the current mouse position so hoverMoveEvent
			 * can require a minimum travel distance before restoring. */
			QPointF local = g_rdpViewItem->mapFromGlobal(QCursor::pos());
			g_hide_pos_x = local.x();
			g_hide_pos_y = local.y();

			g_rdpViewItem->setCursor(g_blank_cursor);
		},
		Qt::QueuedConnection);
	return TRUE;
}

static BOOL my_pointer_setdefault(rdpContext* context)
{
	WINPR_UNUSED(context);
	g_cursor_hidden.store(false, std::memory_order_release);
	QMetaObject::invokeMethod(g_rdpViewItem,
		[]()
		{
			g_rdpViewItem->unsetCursor();
		},
		Qt::QueuedConnection);
	return TRUE;
}

static BOOL my_pointer_setposition(rdpContext* context, UINT32 x, UINT32 y)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(x);
	WINPR_UNUSED(y);
	/* Qt manages cursor position natively — nothing to do. */
	return TRUE;
}

static RECTANGLE_16 scale_frame(rdpContext* context, const RECTANGLE_16* rect)
{
	uint32_t hw = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
	uint32_t hh = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
	uint32_t cw = g_client->view_width_;
	uint32_t ch = g_client->view_height_;
	RECTANGLE_16 rect16 = *rect; // copy rect to rect16

	if (cw == 0)
		cw = hw;
	if (ch == 0)
		ch = hh;

	if (cw != hw || hh != ch)
	{
		rect16.left = rect->left * cw / hw;
		rect16.right = rect->right * cw / hw;
		rect16.top = rect->top * ch / hh;
		rect16.bottom = rect->bottom * ch / hh;
	}

	return rect16;
}

static BOOL noop_begin_paint(rdpContext* context)
{
	WINPR_UNUSED(context);
	return TRUE;
}

static BOOL noop_end_paint(rdpContext* context)
{
	rdpGdi* gdi = context->gdi;

	HGDI_DC hdc = gdi->primary->hdc;
	HGDI_WND hwnd = hdc->hwnd;
	REGION16 region16;

	if (!hwnd || hwnd->invalid->null == TRUE)
		return TRUE;

	region16_init(&region16);

	HGDI_RGN cinvalid = hwnd->cinvalid;
	for (int i = 0; i < hwnd->ninvalid; ++i)
	{
		RECTANGLE_16 rect;
		rect.left = cinvalid[i].x;
		rect.top = cinvalid[i].y;
		rect.right = cinvalid[i].x + cinvalid[i].w;
		rect.bottom = cinvalid[i].y + hwnd->cinvalid[i].h;
		region16_union_rect(&region16, &region16, &rect);
	}

	const RECTANGLE_16* rect = nullptr;
	if (!region16_is_empty(&region16))
	{
		rect = region16_extents(static_cast<const REGION16*>(&region16));
	}

	if (rect)
	{
		RECTANGLE_16 update_rect = scale_frame(context, rect);

		int rx = update_rect.left;
		int ry = update_rect.top;
		int rw = update_rect.right - update_rect.left;
		int rh = update_rect.bottom - update_rect.top;

		/*
		 * Copy frame data from GDI buffer to staging buffer on the
		 * FreeRDP thread, while the data is guaranteed to be stable
		 * (EndPaint just completed).  Then queue a GUI thread update.
		 */
		g_rdpViewItem->copyFrameData(gdi->primary_buffer, gdi->stride,
		                             rx, ry, rw, rh);
		QMetaObject::invokeMethod(
		    g_rdpViewItem,
		    [gdi, rx, ry, rw, rh]() {
			    g_rdpViewItem->updateGdiFrame(gdi, rx, ry, rw, rh);
		    });
	}
	region16_uninit(&region16);

	return TRUE;
}

static BOOL noop_desktop_resize(rdpContext* context)
{
	rdpGdi* gdi = context->gdi;
	rdpSettings* settings = context->settings;
	uint32_t newW = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
	uint32_t newH = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

	/* DEBUG: track GFX reset vs our last sent size */
	int pending = g_pending_resize_count.load(std::memory_order_acquire);
	uint32_t lastSentW = g_last_disp_w.load(std::memory_order_acquire);
	uint32_t lastSentH = g_last_disp_h.load(std::memory_order_acquire);
	if (pending > 0)
	{
		g_pending_resize_count.fetch_sub(1, std::memory_order_acq_rel);
	}
	else
	{
		qf::log::warn("rdp/resize",
		    "GFX_RESET with pending=0 — server-initiated resize or stale response? "
		    "server={}x{} lastSent={}x{}",
		    newW, newH, lastSentW, lastSentH);
	}

	if (lastSentW != 0 && lastSentH != 0 && (newW != lastSentW || newH != lastSentH))
	{
		qf::log::warn("rdp/resize",
		    "MISMATCH: server returned {}x{} but we last sent {}x{}",
		    newW, newH, lastSentW, lastSentH);
	}
	g_last_gfx_resize_w.store(newW, std::memory_order_release);
	g_last_gfx_resize_h.store(newH, std::memory_order_release);

	qf::log::info("rdp/desktop-resize", "resize to {}x{} (gfx={})", newW, newH,
	              gdi->gfx ? "active" : "none");

	if (!gdi->gfx)
	{
		if (!gdi_resize(gdi, newW, newH))
		{
			return FALSE;
		}
	}
	else
	{
		qf::log::info("rdp/desktop-resize",
		              "GFX active, skipping gdi_resize (already done by gdi_ResetGraphics)");
	}

	/*
	 * Update view dimensions so that scale_frame() in noop_end_paint sees
	 * cw==hw and ch==hh and does NOT apply scaling.
	 */
	g_client->view_width_ = newW;
	g_client->view_height_ = newH;

	/* Resize staging buffer synchronously on RDP thread */
	g_rdpViewItem->resizeStagingBuffer(newW, newH);
	/* Dispatch GUI-thread-only work via invokeMethod. */
	QMetaObject::invokeMethod(g_rdpViewItem, [=]() {
		g_rdpViewItem->notifyFrameResized();
	}, Qt::QueuedConnection);

	qf::log::info("rdp/desktop-resize", "resize complete");

	/* Update RdpViewItem's exposed RDP geometry for QML poll logging */
	if (g_rdpViewItem)
	{
		g_rdpViewItem->setRdpGeometry(static_cast<int>(newW), static_cast<int>(newH));

		double cmpDpr = 1.0;
		if (auto* cmpWin = g_rdpViewItem->window())
			cmpDpr = cmpWin->devicePixelRatio();
		int winW = static_cast<int>(std::round(g_rdpViewItem->width() * cmpDpr));
		int winH = static_cast<int>(std::round(g_rdpViewItem->height() * cmpDpr));
		uint32_t alignedWinW = (static_cast<uint32_t>(std::max(winW, 0)) + 3) & ~3u;
		uint32_t alignedWinH = (static_cast<uint32_t>(std::max(winH, 0)) + 3) & ~3u;
		if (alignedWinW >= 640 && alignedWinH >= 480 &&
		    (newW != alignedWinW || newH != alignedWinH))
		{
			qf::log::warn("rdp/resize/dbg",
			    "GFX_RESET done but RDP {}x{} != window aligned {}x{} (window={}x{}) "
			    "— display may be out of sync, waiting for next resize trigger",
			    newW, newH, alignedWinW, alignedWinH, winW, winH);
		}
	}

	return TRUE;
}

static BOOL noop_bitmap_update(rdpContext* context, const BITMAP_UPDATE* bitmap)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(bitmap);
	return TRUE;
}

static BOOL noop_palette_update(rdpContext* context, const PALETTE_UPDATE* palette)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(palette);
	return TRUE;
}

static BOOL noop_play_sound(rdpContext* context, const PLAY_SOUND_UPDATE* play_sound)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(play_sound);
	return TRUE;
}

static BOOL noop_keyboard_set_indicators(rdpContext* context, UINT16 led_flags)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(led_flags);
	return TRUE;
}

static BOOL noop_keyboard_set_ime_status(rdpContext* context, UINT16 imeId, UINT32 imeState,
                                         UINT32 imeConvMode)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(imeId);
	WINPR_UNUSED(imeState);
	WINPR_UNUSED(imeConvMode);
	return TRUE;
}

UINT qf_CliprdrServerFormatListCallBack(CliprdrClientContext* context,
                                        const CLIPRDR_FORMAT_LIST* formatList)
{
	if (!g_client->clipboard_format_from_remote_.empty())
		g_client->clipboard_format_from_remote_.clear();

	/* 新的格式列表到来：中止上一个未完成的远程文件接收（关闭句柄并清状态），
	 * 避免残留 streamId 导致 allDone 永远不成立。 */
	{
		std::lock_guard<std::mutex> lock(g_client->remote_files_mutex_);
		for (auto& kv : g_client->remote_files_)
		{
			if (kv.second.fp)
			{
				fclose(kv.second.fp);
				kv.second.fp = nullptr;
			}
		}
		g_client->remote_files_.clear();
		g_client->remote_received_urls_.clear();
	}

	qf::log::info("cliprdr/format-list", "server advertised {} format(s)", formatList->numFormats);
	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const char* name = formatList->formats[i].formatName ? formatList->formats[i].formatName : "";
		g_client->clipboard_format_from_remote_[formatList->formats[i].formatId] = name;
		qf::log::info("cliprdr/format-list", "  [{}] id=0x{:08x} name='{}'", i,
		              formatList->formats[i].formatId, name);
	}

	auto requestRemoteFormat = [&](UINT32 formatId, const char* formatName)
	{
		CLIPRDR_FORMAT_DATA_REQUEST req = {};
		req.requestedFormatId = formatId;

		g_client->requested_remote_format_id_ = formatId;
		g_client->requested_remote_format_name = formatName ? formatName : "";

		context->ClientFormatDataRequest(context, &req);
	};

	/* 文件传输：服务器剪贴板里有文件。Windows 服务器用命名格式
	 * "FileGroupDescriptorW"（文件列表，数据为 CLIPRDR_FILELIST）和
	 * "FileContents"（流式内容伪格式）广播文件剪贴板。
	 * 注意：FileContents 只能走 CB_FILECONTENTS_REQUEST PDU 拉内容，
	 * 不能对它发 FormatDataRequest（服务器会返回 FAIL），
	 * 因此文件列表必须优先请求 FileGroupDescriptorW，其次 CF_HDROP。 */
	const CLIPRDR_FORMAT* fgDesc = nullptr;
	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const CLIPRDR_FORMAT* f = &formatList->formats[i];
		const char* name = f->formatName;
		if (name && !strcmp(name, "FileGroupDescriptorW"))
		{
			fgDesc = f;
			break;
		}
	}
	if (fgDesc)
	{
		qf::log::info("cliprdr/format-select",
		              "request remote file list [FileGroupDescriptorW] id={}", fgDesc->formatId);
		requestRemoteFormat(fgDesc->formatId, "FileGroupDescriptorW");
		return CHANNEL_RC_OK;
	}
	if (g_client->clipboard_format_from_remote_.contains(CF_HDROP))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_HDROP (file list)");
		requestRemoteFormat(CF_HDROP, nullptr);
		return CHANNEL_RC_OK;
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_UNICODETEXT))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_UNICODETEXT");
		requestRemoteFormat(CF_UNICODETEXT, nullptr);
		return CHANNEL_RC_OK;
	}

	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const CLIPRDR_FORMAT* format = &formatList->formats[i];
		const char* name = format->formatName;
		if (name && !strcmp(name, "PNG"))
		{
			qf::log::info("cliprdr/format-select", "request remote PNG id={}", format->formatId);
			requestRemoteFormat(format->formatId, name);
			return CHANNEL_RC_OK;
		}
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIBV5))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_DIBV5");
		requestRemoteFormat(CF_DIBV5, nullptr);
		return CHANNEL_RC_OK;
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIB))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_DIB");
		requestRemoteFormat(CF_DIB, nullptr);
		return CHANNEL_RC_OK;
	}

	qf::log::warn("cliprdr/format-select", "NO recognizable format in server list — files/text/image all absent");
	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatDataResponseCallBack(
    CliprdrClientContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
	if (!context || !formatDataResponse)
	{
		qf::log::warn("cliprdr/data-response", "invalid clipboard data response");
		return ERROR_INVALID_PARAMETER;
	}

	if (formatDataResponse->common.msgFlags != CB_RESPONSE_OK)
	{
		qf::log::warn("cliprdr/data-response", "remote request failed formatId={} name={}",
		             g_client->requested_remote_format_id_, g_client->requested_remote_format_name);
		return CHANNEL_RC_OK;
	}

	const UINT32 requestedFormatId = g_client->requested_remote_format_id_;
	const QString requestedFormatName = QString::fromStdString(g_client->requested_remote_format_name);

	/* 文件传输：服务器返回文件列表（CF_HDROP 或 "FileGroupDescriptorW"/"FileContents"）
	 * → 解析出每个文件，逐个用 FILECONTENTS_SIZE 请求其大小（后续 RANGE 响应在
	 * qf_CliprdrServerFileContentsResponseCallBack 里落盘）。 */
	const bool isFileList = requestedFormatId == CF_HDROP ||
	                        requestedFormatName == QStringLiteral("FileGroupDescriptorW") ||
	                        requestedFormatName == QStringLiteral("FileContents");
	if (isFileList)
	{
		FILEDESCRIPTORW* files = nullptr;
		UINT32 count = 0;
		if (!qf_parse_remote_file_list(formatDataResponse->requestedFormatData,
		                               formatDataResponse->common.dataLen, &files, &count))
		{
			qf::log::warn("cliprdr/file-list", "cannot parse remote file list (len={})",
			              formatDataResponse->common.dataLen);
			return CHANNEL_RC_OK;
		}
		qf::log::info("cliprdr/file-list", "remote file list: {} item(s)", count);

		std::lock_guard<std::mutex> lock(g_client->remote_files_mutex_);
		for (UINT32 i = 0; i < count; ++i)
		{
			QString name =
			    QString::fromUtf16(reinterpret_cast<const char16_t*>(files[i].cFileName));
			const int slash = std::max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
			const QString base = name.mid(slash + 1);
			const QString outPath = qf_cliprdr_recv_dir() + "/" + base;

			const UINT32 streamId = g_client->next_remote_stream_id_++;
			qf::client_t::RemoteClipFile f;
			f.path = outPath;
			f.list_index_ = i;
			f.size_known = false;
			g_client->remote_files_[streamId] = f;
			qf::log::info("cliprdr/file-list", "  [{}] streamId={} '{}' -> '{}'", i, streamId,
			              base.toUtf8().constData(), outPath.toUtf8().constData());

			CLIPRDR_FILE_CONTENTS_REQUEST req = {};
			req.common.msgType = CB_FILECONTENTS_REQUEST;
			req.streamId = streamId;
			req.listIndex = i;
			req.dwFlags = FILECONTENTS_SIZE;
			req.cbRequested = sizeof(UINT64);
			context->ClientFileContentsRequest(context, &req);
		}
		free(files);
		return CHANNEL_RC_OK;
	}

	QByteArray clipboardData(reinterpret_cast<const char*>(formatDataResponse->requestedFormatData),
	                         static_cast<qsizetype>(formatDataResponse->common.dataLen));
	QMetaObject::invokeMethod(g_rdpViewItem, [clipboardData, requestedFormatId, requestedFormatName]() {
		g_rdpViewItem->updateClipboardDataFromRemote(clipboardData, requestedFormatId,
		                                             requestedFormatName);
	}, Qt::QueuedConnection);

	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatListResponseCallBack(
    CliprdrClientContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatDataRequestCallBack(CliprdrClientContext* context,
                                               const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	if (!context || !formatDataRequest)
	{
		qf::log::warn("cliprdr/data-request", "invalid clipboard data request");
		return ERROR_INVALID_PARAMETER;
	}

	const QMimeData* mimeData = QGuiApplication::clipboard()->mimeData();
	if (!mimeData)
	{
		qf::log::warn("cliprdr/data-request", "no local clipboard data available");
		return CHANNEL_RC_OK;
	}

	qf::log::info("cliprdr/data-request", "server requested formatId={}",
	              formatDataRequest->requestedFormatId);

	auto RemoteFormatDataResponse = [&](const BYTE* rawData, UINT32 dataLen) {
		CLIPRDR_FORMAT_DATA_RESPONSE req = {};
		req.common.msgFlags = CB_RESPONSE_OK;
		req.common.dataLen = dataLen;
		req.requestedFormatData = rawData;

		context->ClientFormatDataResponse(context, &req);
	};

	auto RemoteFormatDataFail = [&]() {
		CLIPRDR_FORMAT_DATA_RESPONSE req = {};
		req.common.msgFlags = CB_RESPONSE_FAIL;
		req.common.dataLen = 0;
		req.requestedFormatData = nullptr;

		context->ClientFormatDataResponse(context, &req);
		qf::log::warn("cliprdr/data-request", "unsupported formatId={}",
		             formatDataRequest->requestedFormatId);
	};

	/* 文件传输：服务器请求文件列表（CF_HDROP 或注册格式 FileGroupDescriptorW，
	 * 数据均为 CLIPRDR_FILELIST） */
	if (formatDataRequest->requestedFormatId == CF_HDROP ||
	    formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_FILELIST)
	{
		UINT32 listLen = 0;
		BYTE* listData = qf_serialize_local_file_list(&listLen);
		if (listData)
		{
			RemoteFormatDataResponse(listData, listLen);
			free(listData);
		}
		else
			RemoteFormatDataFail();
		return CHANNEL_RC_OK;
	}

	if (mimeData->hasText() && formatDataRequest->requestedFormatId == CF_UNICODETEXT)
	{
		const QString text = mimeData->text();
		const char16_t* rawData = reinterpret_cast<const char16_t*>(text.utf16());
		UINT32 dataLen = (std::char_traits<char16_t>::length(rawData) + 1) * sizeof(char16_t); // 16bit char
		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(rawData), dataLen);
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == CF_DIB)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			qf::log::warn("cliprdr/data-request", "no local clipboard image available");
			return CHANNEL_RC_OK;
		}

		QByteArray bmp;
		QBuffer buffer(&bmp);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "BMP"))
		{
			qf::log::warn("cliprdr/data-request", "failed to encode clipboard image as DIB");
			return CHANNEL_RC_OK;
		}

		QByteArray dib = bmp.mid(14);	// skip 14 bytes of BMP header

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(dib.constData()),
		                         static_cast<UINT32>(dib.size()));
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_PNG)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			qf::log::warn("cliprdr/data-request", "no local clipboard image available");
			return CHANNEL_RC_OK;
		}

		QByteArray pngData;
		QBuffer buffer(&pngData);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "PNG"))
		{
			qf::log::warn("cliprdr/data-request", "failed to encode clipboard image as PNG");
			return CHANNEL_RC_OK;
		}

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(pngData.constData()),
		                         static_cast<UINT32>(pngData.size()));
	}
	else
	{
		RemoteFormatDataFail();
	}

	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFileContentsRequestCallBack(CliprdrClientContext* context,
                                                 const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
	if (!context || !fileContentsRequest)
		return ERROR_INVALID_PARAMETER;

	const UINT64 offset = ((UINT64)fileContentsRequest->nPositionHigh << 32) |
	                      (UINT64)fileContentsRequest->nPositionLow;

	auto fail = [&]() {
		CLIPRDR_FILE_CONTENTS_RESPONSE resp = {};
		resp.common.msgFlags = CB_RESPONSE_FAIL;
		resp.streamId = fileContentsRequest->streamId;
		context->ClientFileContentsResponse(context, &resp);
	};

	std::lock_guard<std::mutex> lock(g_client->local_files_mutex_);
	if (fileContentsRequest->listIndex >= g_client->local_files_.size())
	{
		qf::log::warn("cliprdr/file-contents", "request listIndex={} out of range (count={})",
		              fileContentsRequest->listIndex, g_client->local_files_.size());
		fail();
		return CHANNEL_RC_OK;
	}
	const auto& file = g_client->local_files_[fileContentsRequest->listIndex];

	if (fileContentsRequest->dwFlags & FILECONTENTS_SIZE)
	{
		const UINT64 size = static_cast<UINT64>(file.size);
		CLIPRDR_FILE_CONTENTS_RESPONSE resp = {};
		resp.common.msgFlags = CB_RESPONSE_OK;
		resp.streamId = fileContentsRequest->streamId;
		resp.cbRequested = sizeof(size);
		resp.requestedData = reinterpret_cast<const BYTE*>(&size);
		qf::log::info("cliprdr/file-contents", "serve size {} ({} bytes) for '{}'", size,
		              sizeof(size), file.path.toUtf8().constData());
		context->ClientFileContentsResponse(context, &resp);
		return CHANNEL_RC_OK;
	}

	if (fileContentsRequest->dwFlags & FILECONTENTS_RANGE)
	{
		UINT32 got = 0;
		BYTE* data = qf_read_local_file(file.path, offset, fileContentsRequest->cbRequested, &got);
		if (!data)
		{
			qf::log::warn("cliprdr/file-contents", "read failed '{}' offset={} req={}",
			              file.path.toUtf8().constData(), offset, fileContentsRequest->cbRequested);
			fail();
			return CHANNEL_RC_OK;
		}
		CLIPRDR_FILE_CONTENTS_RESPONSE resp = {};
		resp.common.msgFlags = CB_RESPONSE_OK;
		resp.streamId = fileContentsRequest->streamId;
		resp.cbRequested = got;
		resp.requestedData = data;
		qf::log::info("cliprdr/file-contents", "serve range {} -> +{} for '{}'", offset, got,
		              file.path.toUtf8().constData());
		context->ClientFileContentsResponse(context, &resp);
		free(data);
		return CHANNEL_RC_OK;
	}

	fail();
	return CHANNEL_RC_OK;
}

/* 远程 → 本地：收到服务器文件内容数据，写入本地落盘文件；全部完成设置剪贴板 */
UINT qf_CliprdrServerFileContentsResponseCallBack(CliprdrClientContext* context,
                                                  const CLIPRDR_FILE_CONTENTS_RESPONSE* resp)
{
	if (!context || !resp)
		return ERROR_INVALID_PARAMETER;

	std::lock_guard<std::mutex> lock(g_client->remote_files_mutex_);
	auto it = g_client->remote_files_.find(resp->streamId);
	if (it == g_client->remote_files_.end())
	{
		qf::log::warn("cliprdr/file-contents", "unknown streamId={} response", resp->streamId);
		return CHANNEL_RC_OK;
	}
	qf::client_t::RemoteClipFile& file = it->second;

	if (!(resp->common.msgFlags & CB_RESPONSE_OK))
	{
		qf::log::warn("cliprdr/file-contents", "remote request failed streamId={}",
		              resp->streamId);
		if (file.fp)
		{
			fclose(file.fp);
			file.fp = nullptr;
		}
		g_client->remote_files_.erase(it);
		return CHANNEL_RC_OK;
	}

	/* FILECONTENTS_SIZE 响应：8 字节 UINT64 小端。拿到大小后建文件并发起第一块 RANGE */
	if (!file.size_known)
	{
		if (resp->cbRequested < sizeof(UINT64))
		{
			qf::log::warn("cliprdr/file-contents", "bad size response for streamId={}",
			              resp->streamId);
			g_client->remote_files_.erase(it);
			return CHANNEL_RC_OK;
		}
		UINT64 sz = 0;
		memcpy(&sz, resp->requestedData, sizeof(UINT64));
		file.size = static_cast<qint64>(sz);
		file.size_known = true;
		file.fp = fopen(file.path.toUtf8().constData(), "wb");
		if (!file.fp)
		{
			qf::log::error("cliprdr/file-contents", "cannot create '{}'",
			               file.path.toUtf8().constData());
			g_client->remote_files_.erase(it);
			return CHANNEL_RC_OK;
		}
		file.written = 0;
		file.next_offset = 0;
		qf::log::info("cliprdr/file-contents", "remote file size {} for '{}'", file.size,
		              file.path.toUtf8().constData());

		/* 空文件：无需 RANGE，直接完成 */
		if (file.size == 0)
		{
			fclose(file.fp);
			file.fp = nullptr;
			file.done = true;
			g_client->remote_received_urls_.push_back(QUrl::fromLocalFile(file.path));
			bool allDone = true;
			for (const auto& kv : g_client->remote_files_)
				if (!kv.second.done)
					allDone = false;
			if (allDone)
			{
				std::vector<QUrl> urls = std::move(g_client->remote_received_urls_);
				g_client->remote_files_.clear();
				g_client->remote_received_urls_.clear();
				QMetaObject::invokeMethod(g_rdpViewItem,
				                          [urls]() { g_rdpViewItem->setClipboardUrls(urls); },
				                          Qt::QueuedConnection);
			}
			return CHANNEL_RC_OK;
		}

		/* 发起第一块 RANGE 请求 */
		CLIPRDR_FILE_CONTENTS_REQUEST req = {};
		req.common.msgType = CB_FILECONTENTS_REQUEST;
		req.streamId = resp->streamId;
		req.listIndex = it->second.list_index_;
		req.dwFlags = FILECONTENTS_RANGE;
		req.nPositionLow = 0;
		req.nPositionHigh = 0;
		req.cbRequested = QF_FILECONTENTS_CHUNK;
		context->ClientFileContentsRequest(context, &req);
		return CHANNEL_RC_OK;
	}

	/* FILECONTENTS_RANGE 响应：文件数据 */
	if (resp->cbRequested > 0)
	{
		if (file.fp)
		{
			const size_t n = fwrite(resp->requestedData, 1, resp->cbRequested, file.fp);
			file.written += static_cast<qint64>(n);
			file.next_offset += static_cast<qint64>(n);
			qf::log::info("cliprdr/file-contents", "recv {} bytes, {}/{} for '{}'", n,
			              file.written, file.size, file.path.toUtf8().constData());
		}
	}

	/* 文件收完 */
	if (file.written >= file.size)
	{
		if (file.fp)
		{
			fclose(file.fp);
			file.fp = nullptr;
		}
		file.done = true;
		g_client->remote_received_urls_.push_back(QUrl::fromLocalFile(file.path));
		qf::log::info("cliprdr/file-contents", "file done '{}' ({} bytes)",
		              file.path.toUtf8().constData(), file.written);

		/* 检查是否全部完成 */
		bool allDone = true;
		for (const auto& kv : g_client->remote_files_)
			if (!kv.second.done)
				allDone = false;
		if (allDone)
		{
			std::vector<QUrl> urls = std::move(g_client->remote_received_urls_);
			g_client->remote_files_.clear();
			g_client->remote_received_urls_.clear();
			QMetaObject::invokeMethod(g_rdpViewItem,
			                          [urls]() { g_rdpViewItem->setClipboardUrls(urls); },
			                          Qt::QueuedConnection);
		}
		return CHANNEL_RC_OK;
	}

	/* 继续拉取下一个块 */
	CLIPRDR_FILE_CONTENTS_REQUEST req = {};
	req.common.msgType = CB_FILECONTENTS_REQUEST;
	req.streamId = resp->streamId;
	req.listIndex = it->second.list_index_;
	req.dwFlags = FILECONTENTS_RANGE;
	req.nPositionLow = static_cast<UINT32>(file.next_offset & 0xFFFFFFFF);
	req.nPositionHigh = static_cast<UINT32>((file.next_offset >> 32) & 0xFFFFFFFF);
	req.cbRequested = QF_FILECONTENTS_CHUNK;
	context->ClientFileContentsRequest(context, &req);

	return CHANNEL_RC_OK;
}

UINT qf_CliprdrMonitorReadyCallback(CliprdrClientContext* context, const CLIPRDR_MONITOR_READY* monitorReady)
{
    g_client->cliprdr_client_context_ = context;
    qf::log::info("cliprdr/monitor", "monitor ready");

	CLIPRDR_CAPABILITIES capabilities = {};
	CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet = {};
	capabilities.cCapabilitiesSets = 1;
	capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&generalCapabilitySet);
	generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCapabilitySet.capabilitySetLength = 12;
	generalCapabilitySet.version = CB_CAPS_VERSION_2;
	generalCapabilitySet.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED |
	                                    CB_HUGE_FILE_SUPPORT_ENABLED;

	UINT rc = context->ClientCapabilities(context, &capabilities);
	if (rc != CHANNEL_RC_OK)
		return rc;

	CLIPRDR_FORMAT_LIST formatList = {};
	return context->ClientFormatList(context, &formatList);
}

void qt_clipboard_channel_init(CliprdrClientContext* clipboard)
{
	if (!clipboard)
	{
		qf::log::error("cliprdr/init", "clipboard channel init failed: null context");
		return;
	}

    clipboard->MonitorReady = qf_CliprdrMonitorReadyCallback;

	clipboard->ServerFormatList = qf_CliprdrServerFormatListCallBack;
	clipboard->ServerFormatListResponse = qf_CliprdrServerFormatListResponseCallBack;

    clipboard->ServerFormatDataRequest = qf_CliprdrServerFormatDataRequestCallBack;
    clipboard->ServerFormatDataResponse = qf_CliprdrServerFormatDataResponseCallBack;

	clipboard->ServerFileContentsRequest = qf_CliprdrServerFileContentsRequestCallBack;
	clipboard->ServerFileContentsResponse = qf_CliprdrServerFileContentsResponseCallBack;

	qf::log::info("cliprdr/init", "clipboard channel initialized (text/image/files)");
}

void qf_channel_connected_callback(void* context, const ChannelConnectedEventArgs* event)
{
	qf::log::info("channel/connect", "connected name={} interface={}", event->name, fmt::ptr(event->pInterface));

	/* Forward to FreeRDP common handler for standard channels (GFX, disp, etc.) */
	freerdp_client_OnChannelConnectedEventHandler(context, event);

	if (!strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME))
	{
		qf::log::info("cliprdr/init", "initializing clipboard channel");
		qt_clipboard_channel_init(static_cast<CliprdrClientContext*>(event->pInterface));
	}

	if (g_gfxContext == nullptr &&
	    strcmp(event->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
	{
		g_gfxContext = static_cast<RdpgfxClientContext*>(event->pInterface);
		qf::log::info("gfx/init", "GFX context saved");
	}

	/* Save disp context and trigger deferred resize on the main thread. */
	if (strcmp(event->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		g_dispContext = static_cast<DispClientContext*>(event->pInterface);
		qf::log::warn("disp/init", ">>> display control context saved <<<");
		if (g_rdpViewItem)
		{
			QMetaObject::invokeMethod(
			    g_rdpViewItem,
			    []()
			    {
				    if (!g_dispContext || !g_rdpViewItem || !g_instance)
					    return;
				    uint32_t w = static_cast<uint32_t>(g_rdpViewItem->width());
				    uint32_t h = static_cast<uint32_t>(g_rdpViewItem->height());
				    uint32_t winDiPs_w = w, winDiPs_h = h;
				    /* HiDPI：将逻辑像素乘以 DPR 转为物理像素 */
				    double dpr = 1.0;
				    if (auto* win = g_rdpViewItem->window())
					    dpr = win->devicePixelRatio();
				    w = static_cast<uint32_t>(std::round(w * dpr));
				    h = static_cast<uint32_t>(std::round(h * dpr));
				    uint32_t wa = (w + 3) & ~3u;
				    uint32_t ha = (h + 3) & ~3u;
				    uint32_t currentW = freerdp_settings_get_uint32(
				        g_instance->context->settings, FreeRDP_DesktopWidth);
				    uint32_t currentH = freerdp_settings_get_uint32(
				        g_instance->context->settings, FreeRDP_DesktopHeight);
				    qf::log::info("rdp/resize",
				        "disp init: DIPs={}x{} dpr={} phys={}x{} aligned={}x{} currentRdp={}x{}",
				        winDiPs_w, winDiPs_h, dpr, w, h, wa, ha, currentW, currentH);
				    if (w > 0 && h > 0 && (wa != currentW || ha != currentH))
				    {
					    qf::log::warn("rdp/resize", ">>> disp resize {}x{} -> {}x{}",
					                  currentW, currentH, wa, ha);
					    DISPLAY_CONTROL_MONITOR_LAYOUT layout = {};
					    layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
					    layout.Width = wa;
					    layout.Height = ha;
					    layout.Orientation = ORIENTATION_LANDSCAPE;
					    /* Keep server DPI matched to Retina so UI element
					     * physical pixel size stays constant across resizes. */
					    layout.DesktopScaleFactor = static_cast<UINT32>(
					        std::round(g_pointer_dpr.load() * 100.0));
					    layout.DeviceScaleFactor = 100;
					    UINT error =
					        g_dispContext->SendMonitorLayout(g_dispContext, 1, &layout);
					    if (error != CHANNEL_RC_OK)
						    qf::log::error("rdp/resize",
						                   "SendMonitorLayout failed error={}", error);
				    }
				    else
				    {
					    qf::log::warn("rdp/resize", ">>> no resize needed ({}x{})", currentW,
					                  currentH);
				    }
			    },
			    Qt::QueuedConnection);
		}
	}
}

void qf_channel_disconnected_callback(void* context, const ChannelDisconnectedEventArgs* event)
{
	freerdp_client_OnChannelDisconnectedEventHandler(context, event);
}

/* Forward declaration - exported from libfreerdp-client3 */
extern "C" BOOL freerdp_client_load_addins(rdpChannels* channels, rdpSettings* settings);

static BOOL my_load_channels(freerdp* instance)
{
	if (!instance || !instance->context || !instance->context->channels || !instance->context->settings)
		return FALSE;

	rdpSettings* settings = instance->context->settings;

	/* Delegate to FreeRDP's built-in addin loader.
	 * On macOS all channel addins are compiled into the static table of
	 * libfreerdp-client3.dylib, so no per-channel source copy is needed. */
	if (!freerdp_client_load_addins(instance->context->channels, settings))
	{
		qf::log::error("channels/load", "freerdp_client_load_addins failed");
		return FALSE;
	}

	return TRUE;
}

// 1. 预连接回调函数，在这里配置所有连接参数
static BOOL my_pre_connect(freerdp* instance)
{
	rdpSettings* settings = instance->context->settings;

	qf::log::info("rdp/pre-connect", "configuring connection settings");

	// 清除之前连接遗留的设备/动态通道配置，防止重连时重复注册
	freerdp_device_collection_free(settings);
	freerdp_dynamic_channel_collection_free(settings);

	// FreeRDP defaults to enabling clipboard; disable before CLI parsing
	freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, FALSE);

	// 解析命令行参数（仅首次连接时执行，重连时跳过）
	if (!g_cli_parsed && g_cli_argc > 1)
	{
		int status = freerdp_client_settings_parse_command_line(
			settings, g_cli_argc, g_cli_argv, FALSE);
		if (status < 0)
		{
			qf::log::error("rdp/pre-connect",
			               "command-line parsing failed (status=%d)", status);
			freerdp_client_settings_command_line_status_print(
				settings, status, g_cli_argc, g_cli_argv);
			return FALSE;
		}
		qf::log::info("rdp/pre-connect",
		              "command-line arguments parsed successfully");
		g_cli_parsed = true;
		/* Save resolution if provided via /w: /h: or .rdp file */
		g_cli_width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
		g_cli_height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
		if (g_cli_width > 0 && g_cli_height > 0)
			qf::log::warn("rdp/pre-connect",
			    ">>> CLI/.rdp provided resolution: {}x{} <<<",
			    g_cli_width, g_cli_height);
		/* Always ignore .rdp file resolution — use screen/window size for correct display */
		if (g_cli_width > 0 || g_cli_height > 0)
		{
			qf::log::warn("rdp/pre-connect",
			    ">>> ignoring .rdp resolution, will use window size <<<");
			g_cli_width = 0;
			g_cli_height = 0;
		}

		// 保存 /drive: 参数用于重连时恢复（展开 $HOME 等环境变量）。
		// 参数已在 main() 中被过滤、未交给 FreeRDP 解析，这里统一展开后走
		// 下方恢复逻辑挂载——与 /usb: 转磁盘重定向完全同一路径，保证
		// "服务器下发 /drive:HOME,$HOME 即重定向用户主目录" 的一致行为。
		g_saved_drive_args.clear();
		for (const auto& arg : g_raw_drive_args)
			g_saved_drive_args.push_back(qf_expand_drive_path(arg));

		// 处理 /drives 简写 — 枚举 /Volumes 下的本地卷并添加到重定向列表
		for (int i = 1; i < g_cli_argc; i++)
		{
			if (g_cli_argv[i] && strcmp(g_cli_argv[i], "/drives") == 0)
			{
				qf::log::warn("rdp/pre-connect",
				              "/drives: enumerating mounted volumes under /Volumes");
				QDir volumes(QStringLiteral("/Volumes"));
				const QStringList entries = volumes.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
				                                              QDir::Name);
				for (const QString& entry : entries)
				{
					QString path = QStringLiteral("/Volumes/") + entry;
					QByteArray pathUtf8 = path.toUtf8();
					QByteArray nameUtf8 = entry.toUtf8();
					std::string arg = std::string(nameUtf8.constData()) + "," +
					                  std::string(pathUtf8.constData());
					g_saved_drive_args.push_back(arg);
					qf::log::warn("rdp/pre-connect",
					              "/drives: added {}", arg);
				}
				break;
			}
		}

		// FreeRDP 的 CLI 解析器处理 /clipboard:direction-to:* 和 /clipboard:files-to:*
		// 等子选项时，只会更新 FreeRDP_ClipboardFeatureMask，但不会设置
		// FreeRDP_RedirectClipboard = TRUE。
		if (!freerdp_settings_get_bool(settings, FreeRDP_RedirectClipboard))
		{
			for (int i = 1; i < g_cli_argc; i++)
			{
				if (g_cli_argv[i] &&
				    strncmp(g_cli_argv[i], "/clipboard:", 11) == 0)
				{
					freerdp_settings_set_bool(
					    settings, FreeRDP_RedirectClipboard, TRUE);
					qf::log::info("rdp/pre-connect",
					              "clipboard re-enabled for /clipboard: sub-option");
					break;
				}
			}
		}
	}

	// ==== DEBUG: TCP resolve diagnostics ====
	{
		const char* hostname = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
		UINT32 port = freerdp_settings_get_uint32(settings, FreeRDP_ServerPort);
		if (hostname)
		{
			qf::log::warn("rdp/debug/dns",
			    "Hostname='{}' port={}", hostname, port);

			// gai_strerror returns const char* on macOS
			auto gai_err_str = [](int rc) -> const char* {
				if (rc == 0) return "OK";
				return gai_strerror(rc);
			};

			// Test 1: original AF_UNSPEC, no flags
			{
				struct addrinfo hints = {};
				hints.ai_flags = 0;
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				snprintf(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test1 (AF_UNSPEC): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}

			// Test 2: AF_INET
			{
				struct addrinfo hints = {};
				hints.ai_flags = 0;
				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				snprintf(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test2 (AF_INET): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}

			// Test 3: AF_INET | AI_NUMERICHOST
			{
				struct addrinfo hints = {};
				hints.ai_flags = AI_NUMERICHOST;
				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				snprintf(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test3 (AF_INET|AI_NUMERICHOST): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}

			// Test 4: AF_UNSPEC | AI_NUMERICHOST
			{
				struct addrinfo hints = {};
				hints.ai_flags = AI_NUMERICHOST;
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				struct addrinfo* result = nullptr;
				char port_str[16];
				snprintf(port_str, sizeof(port_str), "%u", port);
				int rc = getaddrinfo(hostname, port_str, &hints, &result);
				qf::log::warn("rdp/debug/dns",
				    "Test4 (AF_UNSPEC|AI_NUMERICHOST): rc={} error={}", rc, gai_err_str(rc));
				if (rc == 0) freeaddrinfo(result);
			}
		}
	}

	// 磁盘重定向 — 从保存的 CLI 参数恢复（首次连接或重连均执行）
	if (!g_saved_drive_args.empty() &&
	    !freerdp_device_collection_find_type(settings, RDPDR_DTYP_FILESYSTEM))
	{
		for (const auto& arg : g_saved_drive_args)
		{
			auto comma = arg.find(',');
			std::string name;
			std::string path = arg;
			if (comma != std::string::npos)
			{
				name = arg.substr(0, comma);
				path = arg.substr(comma + 1);
			}
			else
			{
				// 无 name 的 /drive:path 形式（FreeRDP 语法：盘符名由路径推导）。
				// 之前 FreeRDP 解析时自动推导 basename；过滤后这里保持一致。
				auto slash = path.find_last_of('/');
				name = (slash == std::string::npos) ? path
				                                    : path.substr(slash + 1);
			}
			if (name.empty() || path.empty())
			{
				qf::log::warn("rdp/pre-connect",
				              "invalid saved drive arg: {}", arg);
				continue;
			}
			const char* args[] = {"drive", name.c_str(), path.c_str(), nullptr};
			if (!freerdp_client_add_device_channel(settings, 3, args))
				qf::log::error("rdp/pre-connect",
				               "restore drive {} -> {} failed", name, path);
			else
				qf::log::info("rdp/pre-connect",
				              "restored drive {} -> {}", name, path);
		}
	}

	// 摄像头重定向（MS-RDPECAM rdpecam）：默认开启。
	// 无摄像头 / 未授权时 avf HAL 枚举返回 0，不向服务器上报设备，自动忽略。
	// 若 .rdp(camerastoredirect) 已注册 rdpecam，add_dynamic_channel 去重后直接返回 TRUE。
	{
		const char* cam_args[] = {RDPECAM_DVC_CHANNEL_NAME, nullptr};
		if (!freerdp_client_add_dynamic_channel(settings, 1, cam_args))
			qf::log::error("rdp/pre-connect", "failed to register rdpecam channel");
		else
			qf::log::info("rdp/pre-connect", "rdpecam camera redirect channel enabled");
	}

	// 分辨率：使用 QML 窗口布局完成后的实际 Item 尺寸
	{
		uint32_t actualW = g_cli_width;
		uint32_t actualH = g_cli_height;
		if (actualW == 0 || actualH == 0)
		{
			if (auto* win = g_rdpViewItem->window())
			{
				actualW = static_cast<uint32_t>(std::round(
				    g_rdpViewItem->width() * win->devicePixelRatio()));
				actualH = static_cast<uint32_t>(std::round(
				    g_rdpViewItem->height() * win->devicePixelRatio()));
				qf::log::warn("rdp/pre-connect",
				    ">>> pre-connect: item={}x{} dpr={} phys={}x{} <<<",
				    g_rdpViewItem->width(), g_rdpViewItem->height(),
				    win->devicePixelRatio(), actualW, actualH);
			}
		}
		if (actualW == 0 || actualH == 0)
		{
			actualW = g_client->view_width_;
			actualH = g_client->view_height_;
		}
		if (actualW == 0 || actualH == 0)
		{
			actualW = 1024;
			actualH = 768;
		}
		qf::log::warn("rdp/display", ">>> Sending DesktopWidth={} DesktopHeight={} to RDP server <<<", actualW, actualH);
		freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, actualW);
		freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, actualH);

		/* Set monitor layout so the server uses our resolution during GFX init.
		 * DesktopScaleFactor mirrors the Retina dpr (e.g. 200 for dpr=2) so the
		 * server renders UI (icons, text) at 2x physical pixels; otherwise
		 * 48px Windows icons would appear as 24pt on a Retina display and the
		 * session would reflow every time the window is resized. */
		{
			const UINT32 scale = static_cast<UINT32>(std::round(g_pointer_dpr.load() * 100.0));
			rdpMonitor monitors[1] = {};
			monitors[0].x          = 0;
			monitors[0].y          = 0;
			monitors[0].width      = static_cast<INT32>(actualW);
			monitors[0].height     = static_cast<INT32>(actualH);
			monitors[0].is_primary = TRUE;
			monitors[0].attributes.orientation        = ORIENTATION_LANDSCAPE;
			monitors[0].attributes.desktopScaleFactor  = scale;
			monitors[0].attributes.deviceScaleFactor   = 100;
			freerdp_settings_set_monitor_def_array_sorted(settings, monitors, 1);
			qf::log::warn("rdp/display",
			    ">>> Set monitor layout: {}x{} is_primary=1 desktopScale={} <<<",
			    monitors[0].width, monitors[0].height, scale);
		}
	}
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

	// 跳过证书强校验
	freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

	// 允许本地图形解码。
	freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, FALSE);

	// 启用标准 RDP 压缩。
	freerdp_settings_set_bool(settings, FreeRDP_CompressionEnabled, TRUE);

	freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, TRUE);

	// TCP 连接超时（单位毫秒，默认 15000 = 15 秒）
	freerdp_settings_set_uint32(settings, FreeRDP_TcpConnectTimeout, 15000);

	freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, TRUE);

	// USB 重定向 / 摄像头重定向 / 音频重定向（麦克风）暂缓 — 先做到 RDP 画面连接

	// GFX 图形管道
	freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
	freerdp_settings_set_uint32(settings, FreeRDP_FrameAcknowledge, 2);
	freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, TRUE);

	// 音频：播放仍禁用（qf-client 无音频输出设备实现，INFO_NOAUDIOPLAYBACK）。
	// 采集（麦克风重定向，MS-AUDIN audin 通道）开启：
	//  - FreeRDP_AudioCapture=TRUE ① 在 Client Info PDU 置 INFO_AUDIOCAPTURE 能力位，
	//    服务器才会发起 AUDIO_INPUT DVC 并下发 SNDIN_* 消息；
	//    ② freerdp_client_load_addins 自动注册 audin 动态通道（mac 后端 AudioQueue 采集）。
	//  - 无麦克风 / 用户拒绝授权时，audin_main 注册空设备返回 CHANNEL_RC_OK，不阻塞连接。
	freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, TRUE);

	PubSub_SubscribeChannelConnected(instance->context->pubSub, qf_channel_connected_callback);
	PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
	                                    qf_channel_disconnected_callback);

	qf::log::info("rdp/pre-connect", "configuration applied");
	return TRUE;
}

static BOOL my_post_connect(freerdp* instance)
{
	rdpUpdate* update = instance->context->update;

	update->BeginPaint = noop_begin_paint;
	update->EndPaint = noop_end_paint;
	update->DesktopResize = noop_desktop_resize;
	update->Palette = noop_palette_update;
	update->PlaySound = noop_play_sound;
	update->SetKeyboardIndicators = noop_keyboard_set_indicators;
	update->SetKeyboardImeStatus = noop_keyboard_set_ime_status;

	if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
	{
		qf::log::error("rdp/post-connect", "gdi_init failed");
		return FALSE;
	}

		/* Keep FreeRDP's default GDI buffer (heap-allocated by gdi_init).
		 * No DMA-BUF -- frame data is copied to a staging buffer in
		 * updateGdiFrame() and uploaded via QSGSimpleTextureNode.
		 */
		{
			rdpGdi* gdi = instance->context->gdi;
			const uint32_t w = freerdp_settings_get_uint32(
			    instance->context->settings, FreeRDP_DesktopWidth);
			const uint32_t h = freerdp_settings_get_uint32(
			    instance->context->settings, FreeRDP_DesktopHeight);
			/* Resize staging buffer synchronously on RDP thread. */
			g_rdpViewItem->resizeStagingBuffer(w, h);
			/* Dispatch GUI-thread-only work via invokeMethod. */
			QMetaObject::invokeMethod(g_rdpViewItem, [=]() {
				g_rdpViewItem->notifyFrameResized();
			}, Qt::QueuedConnection);
		}
	/* Register RDP pointer (cursor) callbacks. */
	{
		rdpPointer pointer;
		memset(&pointer, 0, sizeof(pointer));
		pointer.size        = sizeof(pointer);
		pointer.New         = my_pointer_new;
		pointer.Free        = my_pointer_free;
		pointer.Set         = my_pointer_set;
		pointer.SetNull     = my_pointer_setnull;
		pointer.SetDefault  = my_pointer_setdefault;
		pointer.SetPosition = my_pointer_setposition;
		graphics_register_pointer(instance->context->graphics, &pointer);
		qf::log::info("rdp/pointer", "pointer callbacks registered");
	}

	g_rdpViewItem->setFreeRDP_context(instance->context);

	QMetaObject::invokeMethod(g_rdpViewItem, []() {
		notify_window_resized();
	}, Qt::QueuedConnection);

	return TRUE;
}

void rdp_loop_thread()
{
	DWORD rc = 1;

	qf::log::info("rdp/session", "starting FreeRDP loop");

	// Register FreeRDP's built-in static addin provider.
	// On macOS all channel addins live in the static table inside
	// libfreerdp-client3.dylib, so no custom wrapper chain is needed.
	freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
	g_instance = freerdp_new();
	if (!g_instance)
	{
		qf::log::error("rdp/session", "failed to allocate FreeRDP instance");
		return;
	}

	// 3. 注册回调
	g_instance->PreConnect = my_pre_connect;
	g_instance->PostConnect = my_post_connect;
	g_instance->LoadChannels = my_load_channels;

	// 4. 创建 RDP 上下文
	if (!freerdp_context_new(g_instance))
	{
		qf::log::error("rdp/session", "failed to allocate RDP context");
		goto fail;
	}

	// 5. 尝试连接（瞬态失败自动重试 3 次）
	qf::log::info("rdp/connect", "attempting to connect");

	{
		BOOL connected = FALSE;
		int attempt = 0;
		while (!connected && attempt < 3 && !g_stopped)
		{
			if (attempt > 0)
			{
				int delay = 500;
				qf::log::info("rdp/connect", "retry attempt {}/3 after {}ms",
				              attempt + 1, delay);
				Sleep(delay);
			}
			attempt++;
			connected = freerdp_connect(g_instance);
			if (connected)
			{
				qf::log::info("rdp/connect", "connected successfully");
				break;
			}
			DWORD freerdp_err = freerdp_get_last_error(g_instance->context);
			qf::log::error("rdp/connect", "connection failed: freerdp_error=0x{:08X} errno={} [{}]",
			               freerdp_err, errno, strerror(errno));
		}

		if (!connected)
		{
			if (g_rdpViewItem)
			{
				QMetaObject::invokeMethod(g_rdpViewItem, "notifyDisconnected",
				                          Qt::QueuedConnection);
			}
		}
	}

	if (!freerdp_shall_disconnect_context(g_instance->context) && !g_stopped)
	{
		enum { STATE_CONNECTED, STATE_RECONNECTING, STATE_DISCONNECTED } state = STATE_CONNECTED;
		DWORD nCount = 0;
		DWORD status = 0;
		HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};

		while (state != STATE_DISCONNECTED && !g_stopped)
		{
			switch (state)
			{
				case STATE_CONNECTED:
				{
					nCount = freerdp_get_event_handles(g_instance->context, handles,
					                                   ARRAYSIZE(handles));

					if (nCount == 0)
					{
						state = STATE_RECONNECTING;
						break;
					}

					status = WaitForMultipleObjects(nCount, handles, FALSE, 100);

					if (status == WAIT_FAILED)
					{
						state = STATE_RECONNECTING;
						break;
					}

					if (status == WAIT_TIMEOUT)
						{ break; }

					if (!freerdp_check_event_handles(g_instance->context))
					{
						state = STATE_RECONNECTING;
						break;
					}

					if (freerdp_shall_disconnect_context(g_instance->context))
						state = STATE_DISCONNECTED;
					break;
				}

				case STATE_RECONNECTING:
				{
					BOOL connected = FALSE;

					for (int attempt = 0; attempt < 3 && !g_stopped; attempt++)
					{
						freerdp_disconnect(g_instance);

						g_dispContext = nullptr;
						g_gfxContext = nullptr;
						g_clipboard_client_context = nullptr;
						g_client->cliprdr_client_context_ = nullptr;

						if (freerdp_connect(g_instance))
						{
							connected = TRUE;
							break;
						}

						for (int i = 0; i < 5 && !g_stopped; i++)
							Sleep(100);
					}

					state = connected ? STATE_CONNECTED : STATE_DISCONNECTED;
					break;
				}

				case STATE_DISCONNECTED:
					break;
			}
		}

		rc = freerdp_get_last_error(g_instance->context);

		qf::log::info("rdp/disconnect", "disconnecting");
		freerdp_disconnect(g_instance);

		if (g_rdpViewItem)
		{
			QMetaObject::invokeMethod(g_rdpViewItem, "notifyDisconnected",
			                          Qt::QueuedConnection);
		}
	}

fail:
	qf::log::info("rdp/session", "test finished");
	freerdp_context_free(g_instance);
	freerdp_free(g_instance);

	qf::log::info("rdp/session", "FreeRDP instance freed rc={}", rc);
}

void stop()
{
	g_stopped = true;
	if (g_freerdp_thread)
	{
		g_freerdp_thread->join();
		g_freerdp_thread.reset();
	}
}

/* Debounce timer for live-resize. */
static QTimer* g_resize_debounce = nullptr;

void notify_window_resized()
{
	if (!g_dispContext || !g_instance || !g_rdpViewItem)
	{
		qf::log::warn("rdp/resize/dbg",
		    "notify_window_resized: SKIP — null guard (disp={} inst={} view={})",
		    fmt::ptr(g_dispContext), fmt::ptr(g_instance), fmt::ptr(g_rdpViewItem));
		return;
	}

	if (!g_resize_debounce)
	{
		g_resize_debounce = new QTimer();
		g_resize_debounce->setSingleShot(true);
		QObject::connect(g_resize_debounce, &QTimer::timeout,
		                 []()
		                 {
			                 if (!g_dispContext || !g_rdpViewItem || !g_instance)
			                 {
				                 qf::log::warn("rdp/resize/dbg",
				                     "timer fired but SKIP — null guard");
				                 return;
			                 }

			                 uint32_t w = static_cast<uint32_t>(g_rdpViewItem->width());
			                 uint32_t h = static_cast<uint32_t>(g_rdpViewItem->height());

			                 double dpr = 1.0;
			                 if (auto* win = g_rdpViewItem->window())
				                 dpr = win->devicePixelRatio();
			                 w = static_cast<uint32_t>(std::round(w * dpr));
			                 h = static_cast<uint32_t>(std::round(h * dpr));

			                 uint32_t wa = (w + 3) & ~3u;
			                 uint32_t ha = (h + 3) & ~3u;

			                 uint32_t currentW = freerdp_settings_get_uint32(
			                     g_instance->context->settings, FreeRDP_DesktopWidth);
			                 uint32_t currentH = freerdp_settings_get_uint32(
			                     g_instance->context->settings, FreeRDP_DesktopHeight);

			                 auto now = std::chrono::steady_clock::now();
			                 int pending = g_pending_resize_count.load(std::memory_order_acquire);
			                 uint32_t lastSentW = g_last_disp_w.load(std::memory_order_acquire);
			                 uint32_t lastSentH = g_last_disp_h.load(std::memory_order_acquire);

			                 if (wa < 640 || ha < 480)
			                 {
				                 return;
			                 }

			                 if (wa != currentW || ha != currentH)
			                 {
				                 if (pending > 0 && wa == lastSentW && ha == lastSentH)
				                 {
					                 return;
				                 }

				                 qf::log::warn("rdp/resize", ">>> disp resize {}x{} -> {}x{}",
				                               currentW, currentH, wa, ha);
				                 DISPLAY_CONTROL_MONITOR_LAYOUT layout = {};
				                 layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
				                 layout.Width = wa;
				                 layout.Height = ha;
				                 layout.Orientation = ORIENTATION_LANDSCAPE;
				                 /* Keep server DPI matched to Retina (dpr*100)
				                  * across window resizes; otherwise the session
				                  * DPI falls back to 96 and desktop icons shrink
				                  * by half. */
				                 layout.DesktopScaleFactor = static_cast<UINT32>(
				                     std::round(g_pointer_dpr.load() * 100.0));
				                 layout.DeviceScaleFactor = 100;
				                 /* NOTE: PhysicalWidth/Height intentionally left 0 —
				                  * MS-RDPEDISP 2.0 computes DPI = 96*Physical/Width,
				                  * so setting them equal to Width would cancel the
				                  * DesktopScaleFactor above. Keep 1.0 semantics. */

				                 g_last_disp_send_ts = now;
				                 g_last_disp_w.store(wa, std::memory_order_release);
				                 g_last_disp_h.store(ha, std::memory_order_release);
				                 g_pending_resize_count.fetch_add(1, std::memory_order_acq_rel);

				                 UINT error = g_dispContext->SendMonitorLayout(g_dispContext, 1, &layout);
				                 if (error != CHANNEL_RC_OK)
				                     qf::log::error("rdp/resize",
				                         "SendMonitorLayout FAILED error={}", error);
			                 }
		                 });
	}

	g_resize_debounce->start(300);
}

void start_rdp_connection()
{
	if (!g_rdpViewItem || !g_client)
		return;

	// 防止在 g_client 初始化后被重复调用
	static std::atomic<bool> s_started{false};
	if (s_started.exchange(true))
		return;

	ensure_blank_cursor();

	// Get primary screen resolution directly (always fullscreen mode)
	{
		auto* screen = QGuiApplication::primaryScreen();
		if (screen)
		{
			QSize screenSize = screen->size();
			double dpr = screen->devicePixelRatio();
			g_pointer_dpr.store(dpr); /* cache for cursor sizing */
			g_client->view_width_ = static_cast<uint32_t>(
			    std::round(screenSize.width() * dpr));
			g_client->view_height_ = static_cast<uint32_t>(
			    std::round(screenSize.height() * dpr));
			qf::log::warn("rdp/start",
			    ">>> screen resolution: {}x{} (dpr={}) <<<",
			    g_client->view_width_, g_client->view_height_, dpr);
		}
		else
		{
			g_client->view_width_  = 1024;
			g_client->view_height_ = 768;
		}
	}

	QTimer::singleShot(50, []()
	{
		// Verify window size matches screen (post-layout sanity check)
		if (auto* win = g_rdpViewItem->window())
		{
			uint32_t w = static_cast<uint32_t>(std::round(
			    g_rdpViewItem->width() * win->devicePixelRatio()));
			uint32_t h = static_cast<uint32_t>(std::round(
			    g_rdpViewItem->height() * win->devicePixelRatio()));
			qf::log::warn("rdp/start",
			    ">>> deferred check: item={}x{} dpr={} phys={}x{} <<<",
			    g_rdpViewItem->width(), g_rdpViewItem->height(),
			    win->devicePixelRatio(), w, h);
			if (w >= 640 && h >= 480 && g_client->view_width_ != w)
			{
				qf::log::warn("rdp/start",
				    ">>> updating view size to match window: {}x{} <<<", w, h);
				g_client->view_width_  = w;
				g_client->view_height_ = h;
			}
		}
		qf::log::warn("rdp/start",
		    ">>> starting connection with window size {}x{}",
		    g_client->view_width_, g_client->view_height_);
		g_freerdp_thread = std::make_unique<std::thread>(rdp_loop_thread);
	});
}

int main(int argc, char* argv[])
{
	// OpenSSL legacy provider（md4 摘要所在，NTLM/NLA 认证必需）在打包后位于
	// <app>/Contents/Frameworks/ossl-modules/。OpenSSL 默认只按编译时 MODULESDIR
	// 或 OPENSSL_MODULES 环境变量查找 provider 模块，必须在任何 OpenSSL 调用
	// （WinPR 初始化）之前设置，否则 md4 缺失 → NLA 认证失败
	// （SEC_E_NO_CREDENTIALS）→ RDP 连接失败。
	{
		char exePath[PATH_MAX] = {0};
		uint32_t exeSize = sizeof(exePath);
		if (_NSGetExecutablePath(exePath, &exeSize) == 0)
		{
			std::string exeDir(exePath);
			const size_t slash = exeDir.find_last_of('/');
			if (slash != std::string::npos)
				exeDir.erase(slash);
			const std::string modulesDir = exeDir + "/../Frameworks/ossl-modules";
			if (access(modulesDir.c_str(), R_OK) == 0)
				setenv("OPENSSL_MODULES", modulesDir.c_str(), 1);
		}
	}

	// Store command-line arguments for later use in rdp_loop_thread
	//
	// macOS 无法做原始 USB 透传：libusb 枚举/open 可以，但 claim_interface
	// 会被系统拒绝（实测 LIBUSB_ERROR_ACCESS，接口被内核驱动独占）。USB
	// 重定向已取消（/usb: 参数直接忽略）；磁盘重定向 /drive: 由 qf 统一
	// 展开 $HOME 后挂载（见 g_raw_drive_args / my_pre_connect）。
	// /cam:（rdpecam 摄像头）仍不支持，继续过滤避免解析失败阻塞连接。
	{
		static std::vector<char*> filtered_argv;
		filtered_argv.reserve(argc);
		filtered_argv.push_back(argv[0]);
		for (int i = 1; i < argc; i++)
		{
			if (argv[i] && strncmp(argv[i], "/drive:", 7) == 0)
			{
				// /drive: 不交给 FreeRDP 解析。原因：FreeRDP 会按字面量路径先
				// 校验存在性——路径存在时直接把服务器原始路径挂载进设备集合
				// （导致"重定向的不是用户目录"）；路径为 $HOME 等字面量时又
				// 跳过设备。两分支行为不一致且都不受 qf 的 $HOME 展开控制。
				// 统一由 qf 展开后走恢复逻辑挂载，保证行为一致。
				g_raw_drive_args.push_back(argv[i] + 7);
				continue;
			}
			if (argv[i] && strncmp(argv[i], "/usb:", 5) == 0)
			{
				// USB 重定向已取消：不再把 /usb: 参数转换为磁盘重定向。
				// 仅过滤避免未知参数阻塞 FreeRDP 解析。
				qf::log::warn("rdp/cli", "usb redirection disabled; ignoring {}", argv[i]);
				continue;
			}
			if (argv[i] && strncmp(argv[i], "/cam:", 5) == 0)
			{
				// 摄像头重定向（rdpecam）默认开启，服务器下发的 /cam: 设备过滤参数
				// 目前不消费（avf 后端枚举所有本地摄像头），仅避免未知参数阻塞解析。
				qf::log::warn("rdp/cli",
				              "rdpecam is enabled by default; ignoring /cam: filter arg: {}",
				              argv[i]);
				continue;
			}
			if (argv[i] && strncmp(argv[i], "/mic:", 5) == 0)
			{
				// 麦克风重定向（audin）默认开启（FreeRDP_AudioCapture=TRUE），
				// 服务器下发的 /mic: 设备/格式过滤参数暂不消费（mac 后端使用
				// 系统默认输入设备），仅避免未知参数阻塞解析。
				qf::log::warn("rdp/cli",
				              "audin is enabled by default; ignoring /mic: filter arg: {}",
				              argv[i]);
				continue;
			}
			filtered_argv.push_back(argv[i]);
		}
		g_cli_argc = static_cast<int>(filtered_argv.size());
		g_cli_argv = filtered_argv.data();
	}

	qf::log::init();

	// Disable Qt Quick pipeline cache to avoid creating cache dirs
	qputenv("QSG_PIPELINE_CACHE", "0");

	QGuiApplication app(argc, argv);

	/* 运行时应用图标（Qt 标准：QApplication::setWindowIcon）。
	 * Finder/启动时图标由 .app bundle 的 logo.icns 提供（CMake MACOSX_BUNDLE）。 */
	app.setWindowIcon(QIcon(QStringLiteral(":/logo.png")));

	QQmlApplicationEngine engine;

	// Always use fullscreen window mode — expose to QML
	engine.rootContext()->setContextProperty("cliFullscreen", QVariant(true));

	// QML 文件路径: qrc:/MyTestApp/src/main.qml（因 CMake QML_FILES 使用相对路径 src/main.qml）
	const QUrl url(QStringLiteral("qrc:/MyTestApp/src/main.qml"));

	QObject::connect(
	    &engine, &QQmlApplicationEngine::objectCreated, &app,
	    [url](QObject* obj, const QUrl& objUrl)
	    {
		    if (!obj && url == objUrl)
		    {
			    QCoreApplication::exit(-1);
		    }
	    },
	    Qt::QueuedConnection);

	engine.load(url);

	if (engine.rootObjects().isEmpty())
	{
		qWarning("Failed to load QML file.");
		stop();
		return -1;
	}

	QObject* root = engine.rootObjects().first();
	g_rdpViewItem = root->findChild<RdpViewItem*>("rdpViewItem");
	if (!g_rdpViewItem)
	{
		qWarning("Failed to find RdpViewItem in QML.");
		stop();
		return -1;
	}

	g_client = std::make_shared<qf::client_t>();
	g_rdpViewItem->set_qfclient_context(g_client);
	g_client->rdpViewItem = g_rdpViewItem;

	// 所有初始化完成后，从 C++ 侧启动 RDP 连接（避免 QML Component.onCompleted
	// 在 engine.load() 期间触发时 g_client 仍为 nullptr 的问题）
	start_rdp_connection();

	int rt = app.exec();

	stop();

	return rt;
}
