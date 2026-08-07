#pragma once

#include <QObject>
#include <QQuickItem>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QQuickWindow>
#include <QRunnable>
#include <QMutex>
#include <QImage>
#include <QtQml/qqmlregistration.h>
#include <qeventloop.h>
#include <qnamespace.h>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QClipboard>
#include <QMimeData>
#include <QUrl>
#include <QByteArray>
#include <QtEndian>
#include <QFileInfo>
#include <QDir>
#include <QPixmap>
#include <QCursor>

/* QRhiTexture API for creating/managing GPU textures (Metal backend on macOS) */
#include <QtGui/rhi/qrhi.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>
#include <cstring>

/* Forward declaration — implemented in mini-qf-client.cc. */
extern void rdp_notify_mouse_moved(double qx, double qy);

/* macOS 系统栏（菜单栏/Dock）显示控制 — implemented in mac_chrome.mm */
extern "C" void qf_set_macos_chrome_hidden(bool hide);

#include "freerdp/freerdp.h"
#include "freerdp/client/cliprdr.h"
#include "freerdp/input.h"

#include "qf_util.h"
#include "qf_log.h"

/* Forward declarations from mini-qf-client.cc */
void start_rdp_connection();
void notify_window_resized();

/* =========================================================================
 * RdpFrameTexture — QSGTexture backed by a standard QRhiTexture
 *
 * Architecture (方案A: QRhiTexture + resource update batch, Metal backend):
 *
 *   beforeRendering (render thread):
 *     ensureTexture() — once per resize:
 *       1. Get QRhi from the window renderer interface
 *       2. rhi->newTexture(QRhiTexture::BGRA8, size) + create()
 *
 *   scene graph render phase (render thread):
 *     commitTextureOperations() — every dirty frame:
 *       3. copy frame data from the CPU staging buffer (under mutex)
 *       4. resourceUpdates->uploadTexture() — QRhi copies the data into
 *          its own staging buffer synchronously, safe against concurrent
 *          writes from the FreeRDP thread.
 *
 * Threading:
 *   - ensureTexture()            : render thread (beforeRendering)
 *   - commitTextureOperations()  : render thread (called by the SG)
 *   - copyFrameData()            : FreeRDP thread (protected by m_frameMutex)
 * ========================================================================= */
class RdpFrameTexture : public QSGTexture
{
public:
    RdpFrameTexture() = default;

    ~RdpFrameTexture() override { release(); }

    /* Create the QRhiTexture if needed (render thread, beforeRendering).
     * Recreates the texture when the frame size changes. The old texture is
     * only released AFTER the new one is created — everything runs on the
     * render thread, so we never delete a texture the scene graph may still
     * be rendering (deleting it from the GUI-thread sync phase flashes
     * magenta/undefined pixels). */
    void ensureTexture(QRhi* rhi)
    {
        if (!rhi)
            return;

        if (m_rhiTex && m_rhiTex->pixelSize() == m_size)
            return;

        if (m_size.isEmpty())
            return;

        QRhiTexture* newTex = rhi->newTexture(QRhiTexture::BGRA8, m_size, 1,
                                              QRhiTexture::Flags{});
        if (!newTex || !newTex->create())
        {
            delete newTex;
            qf::log::error("qf.rdp.tex", "QRhiTexture::create failed ({}x{})",
                           m_size.width(), m_size.height());
            return;
        }

        /* Safe point to drop the old texture: same render thread, and the
         * new texture already exists so the node never references null. */
        delete m_rhiTex;
        m_rhiTex = newTex;

        /* New texture contents are undefined — force one upload of the
         * staging buffer so the first frame after a resize isn't garbage. */
        if (m_frameDirty)
            m_frameDirty->store(true, std::memory_order_release);

        qf::log::info("qf.rdp.tex", "QRhiTexture created ({}x{})",
                      m_size.width(), m_size.height());
    }

    /* Upload the pending frame data (render thread, called by the scene graph
     * for every texture used in the frame). */
    void commitTextureOperations(QRhi* /*rhi*/,
                                 QRhiResourceUpdateBatch* resourceUpdates) override
    {
        if (!m_rhiTex || !m_frameBuffer || !m_frameDirty || !m_mutex)
            return;

        if (!m_frameDirty->load(std::memory_order_acquire))
            return;

        QMutexLocker lock(m_mutex);
        if (m_frameBuffer->empty())
            return;

        /* Resize 期间 staging buffer 会先于纹理切换到新尺寸。若尺寸不一致
         * 仍上传会把错位数据贴到纹理上（花屏）——此时等待 updatePaintNode
         * 切换纹理尺寸后再上传。 */
        const QSize texSize = m_rhiTex->pixelSize();
        if (m_frameBuffer->size() !=
            static_cast<quint32>(texSize.width()) * static_cast<quint32>(texSize.height()) * 4)
            return;

        /* QRhiTextureSubresourceUploadDescription copies the data into its own
         * QByteArray synchronously — safe against concurrent writes from the
         * FreeRDP thread (which holds the same mutex). */
        QRhiTextureSubresourceUploadDescription desc(m_frameBuffer->data(),
                                                     static_cast<quint32>(m_frameBuffer->size()));
        QRhiTextureUploadEntry entry(0, 0, desc);
        resourceUpdates->uploadTexture(m_rhiTex, QRhiTextureUploadDescription(entry));

        m_frameDirty->store(false, std::memory_order_release);
    }

    QRhiTexture* rhiTexture() const override { return m_rhiTex; }

    qint64 comparisonKey() const override
    {
        return reinterpret_cast<qint64>(m_rhiTex);
    }

    QSize textureSize() const override { return m_size; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    QSize size() const { return m_size; }
    void setSize(QSize sz) { m_size = sz; }

    void setFrameData(std::vector<uint8_t>* fb, QMutex* mtx,
                      std::atomic<bool>* dirty)
    {
        m_frameBuffer = fb;
        m_mutex = mtx;
        m_frameDirty = dirty;
    }

    /* Release all GPU resources. Safe to call multiple times. */
    void release()
    {
        if (m_rhiTex)
        {
            delete m_rhiTex;
            m_rhiTex = nullptr;
        }
    }

private:
    QSize                   m_size;
    QRhiTexture*            m_rhiTex = nullptr;

    std::vector<uint8_t>*   m_frameBuffer = nullptr;
    QMutex*                 m_mutex       = nullptr;
    std::atomic<bool>*      m_frameDirty  = nullptr;
};

/* =========================================================================
 * RdpViewItem — QQuickItem-backed RDP view using QSGSimpleTextureNode
 *
 * Rendering pipeline (QRhiTexture upload via scene graph, Metal backend):
 *
 *   FreeRDP thread → decodes H.264 frames into gdi->primary_buffer (heap)
 *
 *   FreeRDP thread (EndPaint):
 *     copyFrameData() → copies dirty rect from GDI buffer to CPU staging buffer
 *
 *   Qt main thread (QueuedConnection from EndPaint):
 *     updateGdiFrame() → triggers QQuickItem::update()
 *
 *   Qt scene graph sync phase (GUI thread):
 *     updatePaintNode() → sets RdpFrameTexture on QSGSimpleTextureNode
 *
 *   Qt beforeRendering signal (render thread):
 *     ensureTexture() → rhi->newTexture + create (once per resize)
 *
 *   Qt render phase (render thread):
 *     commitTextureOperations() → uploads CPU frame via resource updates
 * ========================================================================= */
class RdpViewItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int rdpWidth READ rdpWidth NOTIFY rdpGeometryChanged)
    Q_PROPERTY(int rdpHeight READ rdpHeight NOTIFY rdpGeometryChanged)
    Q_PROPERTY(bool fullscreen READ isFullscreen WRITE setFullscreen NOTIFY fullscreenChanged)

public:
    RdpViewItem(QQuickItem* parent = nullptr) : QQuickItem(parent)
    {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setFlag(QQuickItem::ItemIsFocusScope, true);
        setFocus(true);
        setFlag(QQuickItem::ItemHasContents, true);

        /* Wire up RdpFrameTexture to our frame buffer/mutex/dirty */
        m_rdpTex.setFrameData(&m_frameBuffer, &m_frameMutex, &m_frameDirty);

        /* Install global event filter to intercept ShortcutOverride events
         * when fullscreen, preventing Qt from consuming keys before they
         * reach the RDP session (e.g. Tab, Ctrl+C, direction keys). */
        if (auto* app = QGuiApplication::instance())
            app->installEventFilter(this);

        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
                this, &RdpViewItem::dataChangedCallback);

        qf::log::info("view/init", "RdpViewItem created");
    }

    bool isFullscreen() const { return m_fullscreen; }

    void setFullscreen(bool fs)
    {
        if (m_fullscreen != fs)
        {
            m_fullscreen = fs;
            emit fullscreenChanged();
        }
    }

    /* Intercept ShortcutOverride when fullscreen to prevent Qt
     * from consuming shortcut keys before they reach RDP. */
    bool eventFilter(QObject* /*obj*/, QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride && m_fullscreen)
        {
            if (QGuiApplication::modalWindow())
                return false;
            event->accept();
            return true;
        }
        return false;
    }

    /* DEBUG: track geometry changes */
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override
    {
        QQuickItem::geometryChange(newGeometry, oldGeometry);
        if (newGeometry.size() != oldGeometry.size()) {
            qf::log::info("view/geometry", "size {}x{} -> {}x{}",
                          static_cast<int>(oldGeometry.width()),
                          static_cast<int>(oldGeometry.height()),
                          static_cast<int>(newGeometry.width()),
                          static_cast<int>(newGeometry.height()));
        }
    }

    ~RdpViewItem() override
    {
    }

    /* Expose current RDP resolution to QML for poll logging. */
    int rdpWidth() const { return m_rdpWidth; }
    int rdpHeight() const { return m_rdpHeight; }
    void setRdpGeometry(int w, int h)
    {
        if (m_rdpWidth != w || m_rdpHeight != h)
        {
            m_rdpWidth = w;
            m_rdpHeight = h;
            emit rdpGeometryChanged();
        }
    }

    /* Called from QML after layout completes. */
    Q_INVOKABLE void startConnection()
    {
        start_rdp_connection();
    }

    /* macOS：全屏时隐藏顶部菜单栏和底部 Dock，窗口模式时恢复。
     * 由 QML 工具栏的显示模式切换按钮调用（见 main.qml）。 */
    Q_INVOKABLE void setHideMenuBarDock(bool hide)
    {
        qf_set_macos_chrome_hidden(hide);
    }

    Q_INVOKABLE void notifyWindowResized()
    {
        notify_window_resized();
    }

    /* Called from FreeRDP thread when the connection drops. */
    Q_INVOKABLE void notifyDisconnected()
    {
        m_rdpContext = nullptr;
        m_qfClientContext.reset();
        QCoreApplication::quit();
    }

    void setFreeRDP_context(rdpContext* context)
    {
        m_rdpContext = context;
    }

    void set_qfclient_context(std::shared_ptr<qf::client_t> context)
    {
        m_qfClientContext = std::move(context);
    }

    /* =====================================================================
     * Frame buffer management — CPU staging buffer
     * ===================================================================== */

    /* Called from RDP thread (noop_desktop_resize / my_post_connect).
     * Synchronously updates staging buffer dimensions. */
    void resizeStagingBuffer(uint32_t w, uint32_t h)
    {
        if (w == m_frameWidth && h == m_frameHeight && !m_frameBuffer.empty())
            return;

        QMutexLocker lock(&m_frameMutex);
        m_frameWidth = w;
        m_frameHeight = h;
        m_frameBuffer.resize(static_cast<size_t>(w) * h * 4); // BGRA, 4 Bpp

        /* 新分配的 buffer 内容未定义：resize 后 ensureTexture() 会强制上传一次，
         * 若直接上传未初始化数据会花屏。这里清为纯黑，等服务器重发完整帧即可。 */
        const size_t size = static_cast<size_t>(w) * h * 4;
        std::fill(m_frameBuffer.begin(), m_frameBuffer.end(), 0x00);
        for (size_t i = 3; i < size; i += 4)
            m_frameBuffer[i] = 0xFF;

        qf::log::info("view/frame", "staging buffer resized to {}x{}", w, h);
    }

    /* Called from GUI thread — triggers a scene graph redraw. */
    void notifyFrameResized()
    {
        update();
    }

    /* Called from FreeRDP thread — copies dirty rect from GDI buffer to staging buffer.
     * Only touches m_frameBuffer (non-Qt memory), protected by mutex. */
    void copyFrameData(const uint8_t* srcBuffer, uint32_t srcStride,
                       int rx, int ry, int rw, int rh)
    {
        if (!srcBuffer || !m_frameWidth || !m_frameHeight)
            return;

        QMutexLocker lock(&m_frameMutex);
        if (m_frameBuffer.empty())
            return;

        /* Clamp dirty rect to frame dimensions */
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > static_cast<int>(m_frameWidth))  rw = m_frameWidth - rx;
        if (ry + rh > static_cast<int>(m_frameHeight)) rh = m_frameHeight - ry;

        if (rw <= 0 || rh <= 0)
            return;

        const int bpp = 4; // BGRA, 4 bytes per pixel
        uint8_t* dst = m_frameBuffer.data();
        const uint32_t dstStride = m_frameWidth * bpp;

        /* Copy dirty rect row by row */
        for (int y = 0; y < rh; y++)
        {
            memcpy(dst + (static_cast<size_t>(ry) + y) * dstStride + static_cast<size_t>(rx) * bpp,
                   srcBuffer + (static_cast<size_t>(ry) + y) * srcStride + static_cast<size_t>(rx) * bpp,
                   static_cast<size_t>(rw) * bpp);
        }

        m_frameDirty.store(true, std::memory_order_release);
    }

    /* Called from Qt main thread (via QueuedConnection from noop_end_paint).
     * Data was already copied by copyFrameData on the FreeRDP thread. */
    void updateGdiFrame(rdpGdi* /*gdi*/, int /*rx*/, int /*ry*/, int /*rw*/, int /*rh*/)
    {
        update();
    }

    void clearFrame()
    {
        update();
    }

    /* =====================================================================
     * Scene graph rendering (sync phase, GUI thread)
     *
     * RdpFrameTexture provides:
     *   - ensureTexture()          — QRhiTexture::create (in beforeRendering)
     *   - commitTextureOperations() — uploads frame data (in render phase)
     * ===================================================================== */
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             QQuickItem::UpdatePaintNodeData*) override
    {
        auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);

        if (!m_frameWidth || !m_frameHeight || boundingRect().isEmpty())
        {
            qf::log::warn("qf.rdp.paint", "no frame/size, deleting node");
            delete node;
            m_rdpTex.release();
            return nullptr;
        }

        const QSize texSize(static_cast<int>(m_frameWidth),
                            static_cast<int>(m_frameHeight));

        bool haveNewFrame = m_frameDirty.load(std::memory_order_acquire);

        /* Recreate QRhiTexture on resize. The actual swap and release happen
         * on the render thread inside ensureTexture(); never delete here in
         * the sync phase or the render thread may still use the old texture
         * (magenta flash while resizing the window).
         *
         * 尺寸切换延迟到新帧数据就绪后再做：resize 已把 staging buffer 重建为
         * 新尺寸（纯黑），若立即切换纹理上传的就是黑屏；等服务器 GFX_RESET 后
         * 重发的第一帧到达（dirty=true）再一次性切换并上传，过渡期旧纹理被
         * 拉伸显示，无花屏、无黑屏。 */
        if (m_rdpTex.size() != texSize && haveNewFrame)
        {
            m_rdpTex.setSize(texSize);
            m_rdpTex.setFrameData(&m_frameBuffer, &m_frameMutex, &m_frameDirty);
            qf::log::info("qf.rdp.paint", "RdpFrameTexture resized to {}x{}",
                          texSize.width(), texSize.height());
        }

        /* Ensure beforeRendering is connected once */
        if (!m_brConnected)
        {
            if (auto* w = window())
            {
                connect(w, &QQuickWindow::beforeRendering, this,
                        [this, w]() {
                    auto* ri = w->rendererInterface();
                    if (!ri) return;
                    auto* rhi = static_cast<QRhi*>(
                        ri->getResource(w, QSGRendererInterface::RhiResource));
                    m_rdpTex.ensureTexture(rhi);
                }, Qt::DirectConnection);
                m_brConnected = true;
                qf::log::info("qf.rdp.paint", "beforeRendering connected");
            }
        }

        if (!haveNewFrame && !node)
        {
            qf::log::info("qf.rdp.paint", "no frame & no node, returning null");
            return nullptr;
        }

        if (!node)
        {
            node = new QSGSimpleTextureNode();
            node->setFiltering(QSGTexture::Nearest);
            qf::log::info("qf.rdp.paint", "new QSGSimpleTextureNode created");
        }

        /* Set our RdpFrameTexture on the node */
        node->setTexture(&m_rdpTex);
        node->setOwnsTexture(false);
        node->setRect(boundingRect());

        return node;
    }

    /* =====================================================================
     * Mouse / Keyboard / Clipboard
     * ===================================================================== */

    std::string get_mouse_flags_string(UINT16 flags) {
        std::string buffer{};
        if (flags & PTR_FLAGS_MOVE) buffer += "MOVE, ";
        if (flags & PTR_FLAGS_DOWN) buffer += "DOWN, ";
        if (flags & PTR_FLAGS_BUTTON1) buffer += "BUTTON1 (Left), ";
        if (flags & PTR_FLAGS_BUTTON2) buffer += "BUTTON2 (Right), ";
        if (flags & PTR_FLAGS_BUTTON3) buffer += "BUTTON3 (Middle), ";
        if (flags & PTR_FLAGS_WHEEL) buffer += "WHEEL, ";
        if (flags & PTR_FLAGS_HWHEEL) buffer += "HWHEEL, ";
        return buffer;
    }

    void mouseEventScaleSend(uint32_t mouse_x, uint32_t mouse_y, uint16_t freerdp_mouse_event) {
        if(!m_rdpContext) return;
        uint32_t host_w = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopWidth);
        uint32_t host_h = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopHeight);
        uint32_t map_x = mouse_x * host_w / width();
        uint32_t map_y = mouse_y * host_h / height();
        if (!freerdp_input_send_mouse_event(m_rdpContext->input, freerdp_mouse_event, map_x, map_y))
            qf::log::warn("input/mouse", "failed to send mouse event flags={} x={} y={}",
                          freerdp_mouse_event, map_x, map_y);
    }

    void mousePressEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN : PTR_FLAGS_BUTTON2 | PTR_FLAGS_DOWN;
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), flags);
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 : PTR_FLAGS_BUTTON2;
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), flags);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        rdp_notify_mouse_moved(event->position().x(), event->position().y());
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), PTR_FLAGS_MOVE);
        event->accept();
    }
    void hoverMoveEvent(QHoverEvent* event) override {
        rdp_notify_mouse_moved(event->position().x(), event->position().y());
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), PTR_FLAGS_MOVE);
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override {
        int delta = event->angleDelta().y();
        uint16_t flags = PTR_FLAGS_WHEEL;
        if (delta < 0) { flags |= PTR_FLAGS_WHEEL_NEGATIVE; delta = -delta; }
        flags |= static_cast<uint16_t>(delta) & WheelRotationMask;
        mouseEventScaleSend(static_cast<uint32_t>(event->position().x()), static_cast<uint32_t>(event->position().y()), flags);
        event->accept();
    }

    void keyboardUnicodeEventSend(QKeyEvent* event, bool down) {
        if (!m_rdpContext) return;
        UINT32 freerdp_key_code = qf::to_freerdp_key_code(event);
        if (freerdp_key_code == RDP_SCANCODE_UNKNOWN) {
            uint16_t flags = down ? 0 : KBD_FLAGS_RELEASE;
            if (!event->text().isEmpty())
                freerdp_input_send_unicode_keyboard_event(m_rdpContext->input, flags, event->text().unicode()->unicode());
            return;
        }
        freerdp_input_send_keyboard_event_ex(m_rdpContext->input, down,
                                             down && event->isAutoRepeat(), freerdp_key_code);
    }

    void keyPressEvent(QKeyEvent* event) override { keyboardUnicodeEventSend(event, true); event->accept(); }
    void keyReleaseEvent(QKeyEvent* event) override { keyboardUnicodeEventSend(event, false); event->accept(); }

    /* Send Ctrl+Alt+Delete to the RDP server (from toolbar button). */
    Q_INVOKABLE void sendCtrlAltDelete()
    {
        if (!m_rdpContext || !m_rdpContext->input)
            return;
        rdpInput* input = m_rdpContext->input;
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LCONTROL);
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LMENU);
        freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_DELETE);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_DELETE);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LMENU);
        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LCONTROL);
    }

    /* =====================================================================
     * Clipboard (text / image only in v1 — file transfer is deferred)
     * ===================================================================== */

    static QImage imageFromDib(const QByteArray& dib) {
        if (dib.size() < 40) return {};
        const uchar* bytes = reinterpret_cast<const uchar*>(dib.constData());
        const quint32 headerSize = qFromLittleEndian<quint32>(bytes);
        if (headerSize < 12 || static_cast<qsizetype>(headerSize) > dib.size()) return {};
        quint16 bitCount = 0; quint32 compression = 0; quint32 colorUsed = 0;
        if (headerSize >= 40 && dib.size() >= 40) {
            bitCount = qFromLittleEndian<quint16>(bytes + 14);
            compression = qFromLittleEndian<quint32>(bytes + 16);
            colorUsed = qFromLittleEndian<quint32>(bytes + 32);
        }
        quint32 colorTableBytes = 0;
        if (colorUsed > 0) colorTableBytes = colorUsed * 4;
        else if (bitCount > 0 && bitCount <= 8) colorTableBytes = (1u << bitCount) * 4;
        const quint32 bitfieldsBytes = (headerSize == 40 && compression == 3) ? 12 : 0;
        const quint32 pixelOffset = 14 + headerSize + bitfieldsBytes + colorTableBytes;
        const quint32 fileSize = 14 + static_cast<quint32>(dib.size());
        QByteArray bmp; bmp.reserve(static_cast<qsizetype>(fileSize));
        bmp.append('B'); bmp.append('M');
        auto appendLe16 = [&bmp](quint16 v) { char buf[2]; qToLittleEndian(v, (uchar*)buf); bmp.append(buf, 2); };
        auto appendLe32 = [&bmp](quint32 v) { char buf[4]; qToLittleEndian(v, (uchar*)buf); bmp.append(buf, 4); };
        appendLe32(fileSize); appendLe16(0); appendLe16(0); appendLe32(pixelOffset);
        bmp.append(dib);
        return QImage::fromData(bmp, "BMP");
    }

    void updateClipboardDataFromRemote(const QByteArray& data, uint32_t formatId, const QString& formatName) {
        if (!m_qfClientContext || !m_qfClientContext->cliprdr_client_context_) return;
        m_clipboardDataFromRemote = true;
        if (formatName == QStringLiteral("PNG")) {
            QImage image = QImage::fromData(data, "PNG");
            if (!image.isNull()) QGuiApplication::clipboard()->setImage(image);
        } else if (formatId == CF_DIB || formatId == CF_DIBV5) {
            QImage image = imageFromDib(data);
            if (!image.isNull()) QGuiApplication::clipboard()->setImage(image);
        } else if(formatId == CF_UNICODETEXT) {
            qsizetype charCount = data.size() / static_cast<qsizetype>(sizeof(char16_t));
            const char16_t* textData = reinterpret_cast<const char16_t*>(data.constData());
            if (charCount > 0 && textData[charCount - 1] == u'\0') --charCount;
            QGuiApplication::clipboard()->setText(QString::fromUtf16(textData, charCount));
        }
        m_clipboardDataFromRemote = false;
    }

    void dataChangedCallback() {
        if (!m_qfClientContext) return;
        auto& clipboardContext = m_qfClientContext->cliprdr_client_context_;
        if(!clipboardContext) return;
        if (m_clipboardDataFromRemote) return;

        QClipboard* cb = QGuiApplication::clipboard();
        const QMimeData* md = cb->mimeData();
        if (!md) return;

        qf::log::info("cliprdr/local", "Mac clipboard dataChanged: hasUrls={} hasText={} hasImage={}",
                      md->hasUrls(), md->hasText(), md->hasImage());

        /* 文件传输：Finder 里复制的文件 → 填充本地文件列表，上报 CF_HDROP，
         * 服务器粘贴时通过 FormatDataRequest(CF_HDROP) 取列表、FILECONTENTS 拉内容。 */
        if (md->hasUrls()) {
            std::vector<qf::client_t::LocalClipFile> files;
            for (const QUrl& url : md->urls()) {
                if (!url.isLocalFile()) continue;
                const QString path = url.toLocalFile();
                QFileInfo info(path);
                if (!info.exists()) continue;
                qf::client_t::LocalClipFile f;
                f.path = path;
                f.name = info.fileName();
                f.size = info.size();
                files.push_back(std::move(f));
            }
            if (!files.empty()) {
                {
                    std::lock_guard<std::mutex> lock(m_qfClientContext->local_files_mutex_);
                    m_qfClientContext->local_files_ = std::move(files);
                }
                qf::log::info("cliprdr/file", "Mac clipboard has {} local file(s), advertising CF_HDROP",
                              m_qfClientContext->local_files_.size());
                CLIPRDR_FORMAT_LIST fl = {}; CLIPRDR_FORMAT formats[2] = {};
                formats[0].formatId = CF_HDROP; formats[0].formatName = nullptr;
                formats[1].formatId = qf::CLIPBOARD_FORMAT_FILELIST;
                formats[1].formatName = const_cast<char*>("FileGroupDescriptorW");
                fl.numFormats = 2; fl.formats = formats;
                clipboardContext->ClientFormatList(clipboardContext, &fl);
                return;
            }
        }

        /* 非文件内容：清空本地文件列表，避免残留影响后续粘贴 */
        {
            std::lock_guard<std::mutex> lock(m_qfClientContext->local_files_mutex_);
            m_qfClientContext->local_files_.clear();
        }

        auto RemoteClipboardFormatList = [&,this](uint32_t fid, const char* fname) {
            CLIPRDR_FORMAT_LIST fl = {}; CLIPRDR_FORMAT f = {};
            f.formatId = fid; f.formatName = const_cast<char*>(fname);
            fl.numFormats = 1; fl.formats = &f;
            clipboardContext->ClientFormatList(clipboardContext, &fl);
        };
        if (md->hasText()) {
            RemoteClipboardFormatList(CF_UNICODETEXT, nullptr);
        } else if (md->hasImage()) {
            CLIPRDR_FORMAT_LIST fl = {}; CLIPRDR_FORMAT formats[2] = {};
            formats[0].formatId = qf::CLIPBOARD_FORMAT_PNG; formats[0].formatName = const_cast<char*>("PNG");
            formats[1].formatId = CF_DIB; formats[1].formatName = nullptr;
            fl.numFormats = 2; fl.formats = formats;
            clipboardContext->ClientFormatList(clipboardContext, &fl);
        }
    }

    /* Called (QueuedConnection) from the FreeRDP thread after ALL remote files
     * have been received to disk — put the local file paths on the clipboard so
     * Finder can paste them. */
    void setClipboardUrls(const std::vector<QUrl>& urls) {
        if (urls.empty()) return;
        auto* mime = new QMimeData();
        mime->setUrls(QList<QUrl>(urls.cbegin(), urls.cend()));
        QGuiApplication::clipboard()->setMimeData(mime);
        qf::log::info("cliprdr/file", "pushed {} received file(s) to Finder clipboard",
                      urls.size());
    }

signals:
    void rdpGeometryChanged();
    void clipboardDataResponseFromRemote();
    void fullscreenChanged();

private:
    /* Frame buffer — CPU-side copy of the decoded frame */
    std::vector<uint8_t> m_frameBuffer;
    uint32_t             m_frameWidth  = 0;
    uint32_t             m_frameHeight = 0;
    mutable QMutex       m_frameMutex;

    /* Dirty flag for new frame data */
    std::atomic<bool>    m_frameDirty{false};

    /* QRhi-managed texture + QSGTexture wrapper */
    RdpFrameTexture      m_rdpTex;

    /* beforeRendering connection state */
    bool                 m_brConnected = false;

    int                               m_rdpWidth = 0;
    int                               m_rdpHeight = 0;
    rdpContext*                       m_rdpContext = nullptr;
    std::shared_ptr<qf::client_t>     m_qfClientContext;
    bool                              m_clipboardDataFromRemote = false;
    bool                              m_fullscreen = false;
};
