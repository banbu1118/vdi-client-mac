// mac_chrome.mm — macOS 系统栏（菜单栏 / Dock）显示控制
//
// Qt 的 showFullScreen() 在 macOS 上只会把窗口铺满屏幕，不会自动隐藏
// 顶部的菜单栏和底部的 Dock（它们仍会悬浮在窗口之上）。
// 通过 NSApplicationPresentationOptions 显式控制：
//   全屏时   -> NSApplicationPresentationHideMenuBar | NSApplicationPresentationHideDock
//   窗口模式 -> NSApplicationPresentationDefault（恢复菜单栏 / Dock）
//
// 注意：setPresentationOptions 必须在主线程调用（ObjC 运行时要求），
// 这里通过 dispatch_async 投递到主队列保证线程安全。

#include <dispatch/dispatch.h>
#import <AppKit/AppKit.h>

extern "C" void qf_set_macos_chrome_hidden(bool hide)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		NSApplicationPresentationOptions options = NSApplicationPresentationDefault;
		if (hide)
			options |= NSApplicationPresentationHideMenuBar | NSApplicationPresentationHideDock;
		[NSApp setPresentationOptions:options];
	});
}
