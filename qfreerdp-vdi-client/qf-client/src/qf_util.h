#pragma once

#include <QKeyEvent>
#include <QString>
#include <QUrl>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <freerdp/input.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/client_cliprdr_file.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class RdpViewItem;

namespace qf {

inline UINT to_freerdp_key_code(const QKeyEvent* event)
{
    const int qkey = event->key();
    const bool keypad = event->modifiers() & Qt::KeypadModifier;

    if (keypad) {
        switch (qkey) {
            case Qt::Key_0: return RDP_SCANCODE_NUMPAD0;
            case Qt::Key_1: return RDP_SCANCODE_NUMPAD1;
            case Qt::Key_2: return RDP_SCANCODE_NUMPAD2;
            case Qt::Key_3: return RDP_SCANCODE_NUMPAD3;
            case Qt::Key_4: return RDP_SCANCODE_NUMPAD4;
            case Qt::Key_5: return RDP_SCANCODE_NUMPAD5;
            case Qt::Key_6: return RDP_SCANCODE_NUMPAD6;
            case Qt::Key_7: return RDP_SCANCODE_NUMPAD7;
            case Qt::Key_8: return RDP_SCANCODE_NUMPAD8;
            case Qt::Key_9: return RDP_SCANCODE_NUMPAD9;
            case Qt::Key_Plus: return RDP_SCANCODE_ADD;
            case Qt::Key_Minus: return RDP_SCANCODE_SUBTRACT;
            case Qt::Key_Asterisk: return RDP_SCANCODE_MULTIPLY;
            case Qt::Key_Slash: return RDP_SCANCODE_DIVIDE;
            case Qt::Key_Period: return RDP_SCANCODE_DECIMAL;
            case Qt::Key_Enter: return RDP_SCANCODE_RETURN_KP;
            default: break;
        }
    }

    switch (qkey) {
        case Qt::Key_Escape: return RDP_SCANCODE_ESCAPE;
        case Qt::Key_Tab: return RDP_SCANCODE_TAB;
        case Qt::Key_Backtab: return RDP_SCANCODE_TAB;
        case Qt::Key_Backspace: return RDP_SCANCODE_BACKSPACE;
        case Qt::Key_Return: return RDP_SCANCODE_RETURN;
        case Qt::Key_Enter: return RDP_SCANCODE_RETURN_KP;
        case Qt::Key_Space: return RDP_SCANCODE_SPACE;

        case Qt::Key_0: return RDP_SCANCODE_KEY_0;
        case Qt::Key_1: return RDP_SCANCODE_KEY_1;
        case Qt::Key_2: return RDP_SCANCODE_KEY_2;
        case Qt::Key_3: return RDP_SCANCODE_KEY_3;
        case Qt::Key_4: return RDP_SCANCODE_KEY_4;
        case Qt::Key_5: return RDP_SCANCODE_KEY_5;
        case Qt::Key_6: return RDP_SCANCODE_KEY_6;
        case Qt::Key_7: return RDP_SCANCODE_KEY_7;
        case Qt::Key_8: return RDP_SCANCODE_KEY_8;
        case Qt::Key_9: return RDP_SCANCODE_KEY_9;

        case Qt::Key_A: return RDP_SCANCODE_KEY_A;
        case Qt::Key_B: return RDP_SCANCODE_KEY_B;
        case Qt::Key_C: return RDP_SCANCODE_KEY_C;
        case Qt::Key_D: return RDP_SCANCODE_KEY_D;
        case Qt::Key_E: return RDP_SCANCODE_KEY_E;
        case Qt::Key_F: return RDP_SCANCODE_KEY_F;
        case Qt::Key_G: return RDP_SCANCODE_KEY_G;
        case Qt::Key_H: return RDP_SCANCODE_KEY_H;
        case Qt::Key_I: return RDP_SCANCODE_KEY_I;
        case Qt::Key_J: return RDP_SCANCODE_KEY_J;
        case Qt::Key_K: return RDP_SCANCODE_KEY_K;
        case Qt::Key_L: return RDP_SCANCODE_KEY_L;
        case Qt::Key_M: return RDP_SCANCODE_KEY_M;
        case Qt::Key_N: return RDP_SCANCODE_KEY_N;
        case Qt::Key_O: return RDP_SCANCODE_KEY_O;
        case Qt::Key_P: return RDP_SCANCODE_KEY_P;
        case Qt::Key_Q: return RDP_SCANCODE_KEY_Q;
        case Qt::Key_R: return RDP_SCANCODE_KEY_R;
        case Qt::Key_S: return RDP_SCANCODE_KEY_S;
        case Qt::Key_T: return RDP_SCANCODE_KEY_T;
        case Qt::Key_U: return RDP_SCANCODE_KEY_U;
        case Qt::Key_V: return RDP_SCANCODE_KEY_V;
        case Qt::Key_W: return RDP_SCANCODE_KEY_W;
        case Qt::Key_X: return RDP_SCANCODE_KEY_X;
        case Qt::Key_Y: return RDP_SCANCODE_KEY_Y;
        case Qt::Key_Z: return RDP_SCANCODE_KEY_Z;

        case Qt::Key_Minus: return RDP_SCANCODE_OEM_MINUS;
        case Qt::Key_Equal: return RDP_SCANCODE_OEM_PLUS;
        case Qt::Key_BracketLeft: return RDP_SCANCODE_OEM_4;
        case Qt::Key_BracketRight: return RDP_SCANCODE_OEM_6;
        case Qt::Key_Backslash: return RDP_SCANCODE_OEM_5;
        case Qt::Key_Semicolon: return RDP_SCANCODE_OEM_1;
        case Qt::Key_Apostrophe: return RDP_SCANCODE_OEM_7;
        case Qt::Key_QuoteLeft: return RDP_SCANCODE_OEM_3;
        case Qt::Key_Comma: return RDP_SCANCODE_OEM_COMMA;
        case Qt::Key_Period: return RDP_SCANCODE_OEM_PERIOD;
        case Qt::Key_Slash: return RDP_SCANCODE_OEM_2;

        case Qt::Key_Shift: return RDP_SCANCODE_LSHIFT;
        case Qt::Key_Control: return RDP_SCANCODE_LCONTROL;
        case Qt::Key_Alt: return RDP_SCANCODE_LMENU;
        case Qt::Key_Meta: return RDP_SCANCODE_LWIN;
        case Qt::Key_CapsLock: return RDP_SCANCODE_CAPSLOCK;
        case Qt::Key_NumLock: return RDP_SCANCODE_NUMLOCK;
        case Qt::Key_ScrollLock: return RDP_SCANCODE_SCROLLLOCK;

        case Qt::Key_Left: return RDP_SCANCODE_LEFT;
        case Qt::Key_Right: return RDP_SCANCODE_RIGHT;
        case Qt::Key_Up: return RDP_SCANCODE_UP;
        case Qt::Key_Down: return RDP_SCANCODE_DOWN;
        case Qt::Key_Insert: return RDP_SCANCODE_INSERT;
        case Qt::Key_Delete: return RDP_SCANCODE_DELETE;
        case Qt::Key_Home: return RDP_SCANCODE_HOME;
        case Qt::Key_End: return RDP_SCANCODE_END;
        case Qt::Key_PageUp: return RDP_SCANCODE_PRIOR;
        case Qt::Key_PageDown: return RDP_SCANCODE_NEXT;

        case Qt::Key_F1: return RDP_SCANCODE_F1;
        case Qt::Key_F2: return RDP_SCANCODE_F2;
        case Qt::Key_F3: return RDP_SCANCODE_F3;
        case Qt::Key_F4: return RDP_SCANCODE_F4;
        case Qt::Key_F5: return RDP_SCANCODE_F5;
        case Qt::Key_F6: return RDP_SCANCODE_F6;
        case Qt::Key_F7: return RDP_SCANCODE_F7;
        case Qt::Key_F8: return RDP_SCANCODE_F8;
        case Qt::Key_F9: return RDP_SCANCODE_F9;
        case Qt::Key_F10: return RDP_SCANCODE_F10;
        case Qt::Key_F11: return RDP_SCANCODE_F11;
        case Qt::Key_F12: return RDP_SCANCODE_F12;

        default: return RDP_SCANCODE_UNKNOWN;
    }
}


    struct client_t
    {
        uint32_t view_width_ = 1024;
        uint32_t view_height_ = 768;

        uint32_t requested_remote_format_id_ = 0;
        std::string requested_remote_format_name;
        std::map<uint32_t, std::string> clipboard_format_from_remote_;
        CliprdrClientContext *cliprdr_client_context_ = nullptr;
        RdpViewItem* rdpViewItem = nullptr;

        /* ---- RDPECLIP 文件传输状态 ---- */
        struct LocalClipFile
        {
            QString path;   /* 本地文件绝对路径 */
            QString name;   /* 文件名（Windows cFileName） */
            qint64  size = 0;
        };
        std::vector<LocalClipFile> local_files_;      /* 本地 → 远程（Mac→VM） */
        std::mutex                 local_files_mutex_;

        struct RemoteClipFile
        {
            QString path;     /* 本地落盘路径（临时目录） */
            FILE*   fp = nullptr;
            qint64  size = 0;      /* 0 = 未知，先发 FILECONTENTS_SIZE */
            qint64  written = 0;
            qint64  next_offset = 0;
            UINT32  list_index_ = 0; /* 在远程文件列表中的下标 */
            bool    size_known = false;
            bool    done = false;
        };
        std::map<uint32_t, RemoteClipFile> remote_files_; /* 远程 → 本地，key=streamId */
        std::mutex                          remote_files_mutex_;
        uint32_t                            next_remote_stream_id_ = 1;
        std::vector<QUrl>                   remote_received_urls_; /* 全部接收完成后设置剪贴板 */
    };


    static constexpr UINT32 CLIPBOARD_FORMAT_PNG = 0xC001;
    /* 注册格式：文件列表。Windows 服务器用名称 "FileGroupDescriptorW" 广播/请求
     * 文件列表（数据为 CLIPRDR_FILELIST），标准 CF_HDROP(15) 也可用。 */
    static constexpr UINT32 CLIPBOARD_FORMAT_FILELIST = 0xC002;

}; // namespace qf
