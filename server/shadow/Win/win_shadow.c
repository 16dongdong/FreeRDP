/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2011-2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#include <windows.h>

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <freerdp/log.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>
#include <freerdp/server/server-common.h>

#include "win_shadow.h"

#define TAG SERVER_TAG("shadow.win")
#define SHADOW_CAPTURE_INITIAL_FPS 20U
#define SHADOW_PRIVACY_BLACKOUT_DELAY_MS 1500U
#define SHADOW_PRIVACY_TIMER_ID 1U
#define WM_SHADOW_PRIVACY_SCHEDULE (WM_APP + 0x201)
#define WM_SHADOW_PRIVACY_HIDE (WM_APP + 0x202)
#define WM_SHADOW_PRIVACY_STOP (WM_APP + 0x203)

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

/* https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mouse_event
 * does not mention this flag is only supported if building for _WIN32_WINNT >= 0x0600
 */
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

/**
 * 绘制本机隐私遮罩。
 *
 * 物理 DPMS 息屏会重置 DXGI Desktop Duplication，从而导致远程画面冻结、残影或重叠。遮罩
 * 仅在本机显示黑色画面，并通过 WDA_EXCLUDEFROMCAPTURE 排除在远程捕获外，保留原始桌面的
 * 连续帧序列。
 */
static void win_shadow_privacy_show(HWND window)
{
	const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	if (!window || (width < 1) || (height < 1))
		return;

	SetWindowPos(window, HWND_TOPMOST, left, top, width, height,
	             SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
	InvalidateRect(window, nullptr, TRUE);
}

/**
 * 隐藏本机隐私遮罩。
 *
 * 此操作只由已验证为非注入的本机键鼠输入触发；远程端通过 SendInput 注入的输入不会进入该
 * 路径，因此不会意外点亮本机屏幕。
 */
static void win_shadow_privacy_hide(HWND window)
{
	if (!window)
		return;

	KillTimer(window, SHADOW_PRIVACY_TIMER_ID);
	ShowWindow(window, SW_HIDE);
}

/**
 * 处理隐私遮罩窗口的绘制与延迟显示。
 *
 * 窗口永不激活且命中测试透明，物理键鼠可继续操作桌面；定时器只在 RDP 图形会话就绪后显示
 * 遮罩，避免连接握手期间改变桌面显示状态。
 */
static LRESULT CALLBACK win_shadow_privacy_window_proc(HWND window, UINT message, WPARAM wParam,
                                                        LPARAM lParam)
{
	switch (message)
	{
		case WM_ERASEBKGND:
			return 1;
		case WM_NCHITTEST:
			return HTTRANSPARENT;
		case WM_PAINT:
		{
			PAINTSTRUCT paint = WINPR_C_ARRAY_INIT;
			HDC deviceContext = BeginPaint(window, &paint);
			FillRect(deviceContext, &paint.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
			EndPaint(window, &paint);
			return 0;
		}
		case WM_TIMER:
			if (wParam == SHADOW_PRIVACY_TIMER_ID)
			{
				KillTimer(window, SHADOW_PRIVACY_TIMER_ID);
				win_shadow_privacy_show(window);
			}
			return 0;
		default:
			return DefWindowProc(window, message, wParam, lParam);
	}
}

/**
 * 创建不会进入 DXGI 捕获流的黑色隐私窗口。
 *
 * WDA_EXCLUDEFROMCAPTURE 不可用时返回 nullptr，而不是显示会污染远程画面的黑窗。调用方保留
 * 正常桌面共享，避免以本机隐私功能换取远程渲染正确性。
 */
static HWND win_shadow_privacy_create_window(void)
{
	static const WCHAR windowClassName[] = L"FreeRDPShadowPrivacyWindow";
	WNDCLASSEXW windowClass = WINPR_C_ARRAY_INIT;
	HWND window;

	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = win_shadow_privacy_window_proc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = windowClassName;
	if (!RegisterClassExW(&windowClass) && (GetLastError() != ERROR_CLASS_ALREADY_EXISTS))
	{
		WLog_WARN(TAG, "注册本机隐私遮罩窗口失败: %lu", GetLastError());
		return nullptr;
	}

	window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
	                         windowClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
	                         windowClass.hInstance, nullptr);
	if (!window)
	{
		WLog_WARN(TAG, "创建本机隐私遮罩窗口失败: %lu", GetLastError());
		return nullptr;
	}

	if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE))
	{
		WLog_WARN(TAG, "无法将本机隐私遮罩排除在捕获外: %lu", GetLastError());
		DestroyWindow(window);
		return nullptr;
	}

	return window;
}

/**
 * 识别真实本机键盘输入并请求撤销隐私遮罩。
 *
 * Windows 将 SendInput 标记为注入事件。只响应没有 LLKHF_INJECTED 标记的事件，可让远程键盘
 * 控制继续工作而不点亮本机画面。
 */
static LRESULT CALLBACK win_shadow_privacy_keyboard_hook(int code, WPARAM wParam, LPARAM lParam)
{
	if ((code == HC_ACTION) && lParam)
	{
		const KBDLLHOOKSTRUCT* input = (const KBDLLHOOKSTRUCT*)lParam;
		if ((input->flags & LLKHF_INJECTED) == 0)
			PostThreadMessage(GetCurrentThreadId(), WM_SHADOW_PRIVACY_HIDE, 0, 0);
	}

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

/**
 * 识别真实本机鼠标输入并请求撤销隐私遮罩。
 *
 * 与键盘钩子相同，LLMHF_INJECTED 的远程输入不会移除遮罩；用户触碰物理鼠标后遮罩立即隐藏，
 * 且不会在当前会话内再次自动显示。
 */
static LRESULT CALLBACK win_shadow_privacy_mouse_hook(int code, WPARAM wParam, LPARAM lParam)
{
	if ((code == HC_ACTION) && lParam)
	{
		const MSLLHOOKSTRUCT* input = (const MSLLHOOKSTRUCT*)lParam;
		if ((input->flags & LLMHF_INJECTED) == 0)
			PostThreadMessage(GetCurrentThreadId(), WM_SHADOW_PRIVACY_HIDE, 0, 0);
	}

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

/**
 * 运行本机隐私遮罩与物理输入钩子的消息循环。
 *
 * 线程只持有子系统中由停止路径等待释放的状态。启动完成后通知初始化者；窗口、钩子或消息循环
 * 创建失败时退出，服务本身仍可继续提供远程桌面。
 */
static DWORD WINAPI win_shadow_privacy_thread(LPVOID arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	MSG message = WINPR_C_ARRAY_INIT;
	HHOOK keyboardHook = nullptr;
	HHOOK mouseHook = nullptr;
	BOOL running = TRUE;

	WINPR_ASSERT(subsystem);
	PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	subsystem->privacyWindow = win_shadow_privacy_create_window();
	if (subsystem->privacyWindow)
	{
		keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, win_shadow_privacy_keyboard_hook,
		                                GetModuleHandleW(nullptr), 0);
		mouseHook = SetWindowsHookExW(WH_MOUSE_LL, win_shadow_privacy_mouse_hook,
		                             GetModuleHandleW(nullptr), 0);
		if (!keyboardHook || !mouseHook)
		{
			WLog_WARN(TAG, "安装本机隐私遮罩输入钩子失败: %lu", GetLastError());
			if (keyboardHook)
				UnhookWindowsHookEx(keyboardHook);
			if (mouseHook)
				UnhookWindowsHookEx(mouseHook);
			DestroyWindow(subsystem->privacyWindow);
			subsystem->privacyWindow = nullptr;
		}
	}

	SetEvent(subsystem->privacyReadyEvent);
	while (running && GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		if (message.hwnd == nullptr)
		{
			switch (message.message)
			{
				case WM_SHADOW_PRIVACY_SCHEDULE:
					if (subsystem->privacyWindow &&
					    !SetTimer(subsystem->privacyWindow, SHADOW_PRIVACY_TIMER_ID,
					              SHADOW_PRIVACY_BLACKOUT_DELAY_MS, nullptr))
						WLog_WARN(TAG, "安排本机隐私遮罩失败: %lu", GetLastError());
					continue;
				case WM_SHADOW_PRIVACY_HIDE:
					win_shadow_privacy_hide(subsystem->privacyWindow);
					continue;
				case WM_SHADOW_PRIVACY_STOP:
					running = FALSE;
					continue;
				default:
					break;
			}
		}

		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	if (keyboardHook)
		UnhookWindowsHookEx(keyboardHook);
	if (mouseHook)
		UnhookWindowsHookEx(mouseHook);
	if (subsystem->privacyWindow)
		DestroyWindow(subsystem->privacyWindow);
	subsystem->privacyWindow = nullptr;
	return 0;
}

/**
 * 初始化本机隐私遮罩线程。
 *
 * 此线程在服务启动时预热，连接阶段只投递异步消息，不会让 RDP 握手等待窗口或钩子创建。失败
 * 时返回 FALSE，由调用方记录告警并继续提供无息屏的稳定远程服务。
 */
static BOOL win_shadow_privacy_init(winShadowSubsystem* subsystem)
{
	DWORD waitResult;

	WINPR_ASSERT(subsystem);
	subsystem->privacyReadyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!subsystem->privacyReadyEvent)
		return FALSE;

	subsystem->privacyThread = CreateThread(nullptr, 0, win_shadow_privacy_thread, subsystem, 0,
	                                        &subsystem->privacyThreadId);
	if (!subsystem->privacyThread)
	{
		CloseHandle(subsystem->privacyReadyEvent);
		subsystem->privacyReadyEvent = nullptr;
		return FALSE;
	}

	waitResult = WaitForSingleObject(subsystem->privacyReadyEvent, 3000);
	if ((waitResult == WAIT_OBJECT_0) && subsystem->privacyWindow)
		return TRUE;

	WLog_WARN(TAG, "本机隐私遮罩未能就绪: %lu", waitResult);
	return FALSE;
}

/**
 * 停止本机隐私遮罩线程并释放其同步句柄。
 *
 * 停止消息在消息队列已就绪后发送；等待线程退出可确保子系统释放后没有输入钩子继续访问其状态。
 */
static void win_shadow_privacy_uninit(winShadowSubsystem* subsystem)
{
	WINPR_ASSERT(subsystem);
	if (subsystem->privacyThread)
	{
		if (subsystem->privacyThreadId)
			PostThreadMessage(subsystem->privacyThreadId, WM_SHADOW_PRIVACY_STOP, 0, 0);
		WaitForSingleObject(subsystem->privacyThread, INFINITE);
		CloseHandle(subsystem->privacyThread);
		subsystem->privacyThread = nullptr;
	}

	if (subsystem->privacyReadyEvent)
	{
		CloseHandle(subsystem->privacyReadyEvent);
		subsystem->privacyReadyEvent = nullptr;
	}
	subsystem->privacyThreadId = 0;
	subsystem->privacyWindow = nullptr;
}

/**
 * 在 RDP 图形会话激活后安排本机隐私黑屏。
 *
 * 该调用只向已就绪的遮罩线程投递消息，绝不阻塞图形握手；客户端断开不会撤销遮罩，只有本机
 * 物理键鼠钩子会隐藏它。
 */
static void win_shadow_client_activated(rdpShadowSubsystem* arg, rdpShadowClient* client)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	WINPR_UNUSED(client);
	WINPR_ASSERT(subsystem);

	if (!subsystem->privacyWindow || !subsystem->privacyThreadId)
	{
		WLog_WARN(TAG, "本机隐私遮罩不可用，未改变显示状态");
		return;
	}

	if (!PostThreadMessage(subsystem->privacyThreadId, WM_SHADOW_PRIVACY_SCHEDULE, 0, 0))
		WLog_WARN(TAG, "安排本机隐私遮罩失败: %lu", GetLastError());
}

/**
 * 同步远程输入状态。
 *
 * Windows Shadow 当前不维护独立的锁键状态，此处保留协议成功语义；本机隐私遮罩通过低级钩子
 * 区分注入输入，不会被该同步事件点亮。
 */
static BOOL win_shadow_input_synchronize_event(rdpShadowSubsystem* subsystem,
                                               rdpShadowClient* client, UINT32 flags)
{
	WINPR_UNUSED(subsystem);
	WINPR_UNUSED(client);
	WINPR_UNUSED(flags);
	return TRUE;
}

static BOOL win_shadow_input_keyboard_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                            UINT16 flags, UINT8 code)
{
	UINT rc;
	INPUT event;
	event.type = INPUT_KEYBOARD;
	event.ki.wVk = 0;
	event.ki.wScan = code;
	event.ki.dwFlags = KEYEVENTF_SCANCODE;
	event.ki.dwExtraInfo = 0;
	event.ki.time = 0;

	if (flags & KBD_FLAGS_RELEASE)
		event.ki.dwFlags |= KEYEVENTF_KEYUP;

	if (flags & KBD_FLAGS_EXTENDED)
		event.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

	rc = SendInput(1, &event, sizeof(INPUT));
	if (rc == 0)
		return FALSE;
	return TRUE;
}

static BOOL win_shadow_input_unicode_keyboard_event(rdpShadowSubsystem* subsystem,
                                                    rdpShadowClient* client, UINT16 flags,
                                                    UINT16 code)
{
	UINT rc;
	INPUT event;
	event.type = INPUT_KEYBOARD;
	event.ki.wVk = 0;
	event.ki.wScan = code;
	event.ki.dwFlags = KEYEVENTF_UNICODE;
	event.ki.dwExtraInfo = 0;
	event.ki.time = 0;

	if (flags & KBD_FLAGS_RELEASE)
		event.ki.dwFlags |= KEYEVENTF_KEYUP;

	rc = SendInput(1, &event, sizeof(INPUT));
	if (rc == 0)
		return FALSE;
	return TRUE;
}

static BOOL win_shadow_input_mouse_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                         UINT16 flags, UINT16 x, UINT16 y)
{
	UINT rc = 1;
	INPUT event = WINPR_C_ARRAY_INIT;
	float width;
	float height;

	event.type = INPUT_MOUSE;

	if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL))
	{
		if (flags & PTR_FLAGS_WHEEL)
			event.mi.dwFlags = MOUSEEVENTF_WHEEL;
		else
			event.mi.dwFlags = MOUSEEVENTF_HWHEEL;
		event.mi.mouseData = flags & WheelRotationMask;

		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			event.mi.mouseData *= -1;

		rc = SendInput(1, &event, sizeof(INPUT));

		/* The build target is a system that did not support MOUSEEVENTF_HWHEEL
		 * but it may run on newer systems supporting it.
		 * Ignore the return value in these cases.
		 */
#if (_WIN32_WINNT < 0x0600)
		if (flags & PTR_FLAGS_HWHEEL)
			rc = 1;
#endif
	}
	else
	{
		width = (float)GetSystemMetrics(SM_CXSCREEN);
		height = (float)GetSystemMetrics(SM_CYSCREEN);
		event.mi.dx = (LONG)((float)x * (65535.0f / width));
		event.mi.dy = (LONG)((float)y * (65535.0f / height));
		event.mi.dwFlags = MOUSEEVENTF_ABSOLUTE;

		if (flags & PTR_FLAGS_MOVE)
		{
			event.mi.dwFlags |= MOUSEEVENTF_MOVE;
			rc = SendInput(1, &event, sizeof(INPUT));
			if (rc == 0)
				return FALSE;
		}

		event.mi.dwFlags = MOUSEEVENTF_ABSOLUTE;

		if (flags & PTR_FLAGS_BUTTON1)
		{
			if (flags & PTR_FLAGS_DOWN)
				event.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
			else
				event.mi.dwFlags |= MOUSEEVENTF_LEFTUP;

			rc = SendInput(1, &event, sizeof(INPUT));
		}
		else if (flags & PTR_FLAGS_BUTTON2)
		{
			if (flags & PTR_FLAGS_DOWN)
				event.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
			else
				event.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;

			rc = SendInput(1, &event, sizeof(INPUT));
		}
		else if (flags & PTR_FLAGS_BUTTON3)
		{
			if (flags & PTR_FLAGS_DOWN)
				event.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
			else
				event.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;

			rc = SendInput(1, &event, sizeof(INPUT));
		}
	}

	if (rc == 0)
		return FALSE;
	return TRUE;
}

static BOOL win_shadow_input_extended_mouse_event(rdpShadowSubsystem* subsystem,
                                                  rdpShadowClient* client, UINT16 flags, UINT16 x,
                                                  UINT16 y)
{
	UINT rc = 1;
	INPUT event = WINPR_C_ARRAY_INIT;
	float width;
	float height;

	if ((flags & PTR_XFLAGS_BUTTON1) || (flags & PTR_XFLAGS_BUTTON2))
	{
		event.type = INPUT_MOUSE;

		if (flags & PTR_FLAGS_MOVE)
		{
			width = (float)GetSystemMetrics(SM_CXSCREEN);
			height = (float)GetSystemMetrics(SM_CYSCREEN);
			event.mi.dx = (LONG)((float)x * (65535.0f / width));
			event.mi.dy = (LONG)((float)y * (65535.0f / height));
			event.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
			rc = SendInput(1, &event, sizeof(INPUT));
			if (rc == 0)
				return FALSE;
		}

		event.mi.dx = event.mi.dy = event.mi.dwFlags = 0;

		if (flags & PTR_XFLAGS_DOWN)
			event.mi.dwFlags |= MOUSEEVENTF_XDOWN;
		else
			event.mi.dwFlags |= MOUSEEVENTF_XUP;

		if (flags & PTR_XFLAGS_BUTTON1)
			event.mi.mouseData = XBUTTON1;
		else if (flags & PTR_XFLAGS_BUTTON2)
			event.mi.mouseData = XBUTTON2;

		rc = SendInput(1, &event, sizeof(INPUT));
	}

	if (rc == 0)
		return FALSE;
	return TRUE;
}

static int win_shadow_invalidate_region(winShadowSubsystem* subsystem, int x, int y, int width,
                                        int height)
{
	rdpShadowServer* server;
	rdpShadowSurface* surface;
	RECTANGLE_16 invalidRect;
	server = subsystem->base.server;
	surface = server->surface;
	invalidRect.left = x;
	invalidRect.top = y;
	invalidRect.right = x + width;
	invalidRect.bottom = y + height;
	EnterCriticalSection(&(surface->lock));
	region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion), &invalidRect);
	LeaveCriticalSection(&(surface->lock));
	return 1;
}

/**
 * 复制 DXGI 已变化的桌面区域并通知所有 Shadow 客户端。
 *
 * 此函数在 Windows 捕获线程中执行。单客户端场景会采纳编码器根据帧确认计算出的建议帧率，
 * 让捕获速度随网络积压自动降低；复制或像素转换失败时返回错误，调用方不会发布损坏帧。
 */
static int win_shadow_surface_copy(winShadowSubsystem* subsystem)
{
	int x, y;
	int width;
	int height;
	int count;
	int status = 1;
	int nDstStep = 0;
	DWORD DstFormat;
	BYTE* pDstData = nullptr;
	rdpShadowServer* server;
	rdpShadowSurface* surface;
	RECTANGLE_16 surfaceRect;
	RECTANGLE_16 invalidRect;
	const RECTANGLE_16* extents;
	server = subsystem->base.server;
	surface = server->surface;

	ArrayList_Lock(server->clients);
	count = ArrayList_Count(server->clients);
	ArrayList_Unlock(server->clients);

	if (count < 1)
		return 1;

	surfaceRect.left = surface->x;
	surfaceRect.top = surface->y;
	surfaceRect.right = surface->x + surface->width;
	surfaceRect.bottom = surface->y + surface->height;
	region16_intersect_rect(&(surface->invalidRegion), &(surface->invalidRegion), &surfaceRect);

	if (region16_is_empty(&(surface->invalidRegion)))
		return 1;

	extents = region16_extents(&(surface->invalidRegion));
	CopyMemory(&invalidRect, extents, sizeof(RECTANGLE_16));
	shadow_capture_align_clip_rect(&invalidRect, &surfaceRect);
	x = invalidRect.left;
	y = invalidRect.top;
	width = invalidRect.right - invalidRect.left;
	height = invalidRect.bottom - invalidRect.top;

	if (0)
	{
		x = 0;
		y = 0;
		width = surface->width;
		height = surface->height;
	}

	WLog_DBG(TAG, "SurfaceCopy x: %d y: %d width: %d height: %d right: %d bottom: %d", x, y, width,
	         height, x + width, y + height);
#if defined(WITH_WDS_API)
	{
		rdpGdi* gdi;
		shwContext* shw;
		rdpContext* context;

		WINPR_ASSERT(subsystem);
		shw = subsystem->shw;
		WINPR_ASSERT(shw);

		context = &shw->common.context;
		WINPR_ASSERT(context);

		gdi = context->gdi;
		WINPR_ASSERT(gdi);

		pDstData = gdi->primary_buffer;
		nDstStep = gdi->width * 4;
		DstFormat = gdi->dstFormat;
	}
#elif defined(WITH_DXGI_1_2)
	DstFormat = PIXEL_FORMAT_BGRX32;
	status = win_shadow_dxgi_fetch_frame_data(subsystem, &pDstData, &nDstStep, x, y, width, height);
#endif

	if (status <= 0)
		return status;

	if (!freerdp_image_copy_no_overlap(surface->data, surface->format, surface->scanline, x, y,
	                                   width, height, pDstData, DstFormat, nDstStep, x, y, nullptr,
	                                   FREERDP_FLIP_NONE))
		return ERROR_INTERNAL_ERROR;

	ArrayList_Lock(server->clients);
	count = ArrayList_Count(server->clients);
	shadow_subsystem_frame_update(&subsystem->base);

	if (count == 1)
	{
		rdpShadowClient* client = (rdpShadowClient*)ArrayList_GetItem(server->clients, 0);

		if (client && client->encoder)
			subsystem->base.captureFrameRate = shadow_encoder_preferred_fps(client->encoder);
	}

	ArrayList_Unlock(server->clients);
	region16_clear(&(surface->invalidRegion));
	return 1;
}

#if defined(WITH_WDS_API)

static DWORD WINAPI win_shadow_subsystem_thread(LPVOID arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	DWORD status;
	DWORD nCount;
	HANDLE events[32];
	HANDLE StopEvent;
	StopEvent = subsystem->base.server->StopEvent;
	nCount = 0;
	events[nCount++] = StopEvent;
	events[nCount++] = subsystem->RdpUpdateEnterEvent;

	while (1)
	{
		status = WaitForMultipleObjects(nCount, events, FALSE, INFINITE);

		if (WaitForSingleObject(StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

		if (WaitForSingleObject(subsystem->RdpUpdateEnterEvent, 0) == WAIT_OBJECT_0)
		{
			win_shadow_surface_copy(subsystem);
			(void)ResetEvent(subsystem->RdpUpdateEnterEvent);
			(void)SetEvent(subsystem->RdpUpdateLeaveEvent);
		}
	}

	ExitThread(0);
	return 0;
}

#elif defined(WITH_DXGI_1_2)

/**
 * 运行 Windows DXGI 桌面捕获循环。
 *
 * 循环以客户端帧确认反馈的自适应帧率拉取桌面更新。处理耗时超过一个帧周期时重新以当前
 * 时间排程，防止旧实现为追赶过期时间点而连续捕获，造成输入延迟和 CPU 峰值。停止事件或
 * 不合法帧率会结束线程，避免继续执行未定义的除零或忙循环。
 */
static DWORD WINAPI win_shadow_subsystem_thread(LPVOID arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	UINT32 fps;
	DWORD status;
	DWORD nCount;
	UINT64 cTime;
	DWORD dwTimeout;
	DWORD dwInterval;
	UINT64 frameTime;
	HANDLE events[32];
	HANDLE StopEvent;
	StopEvent = subsystem->base.server->StopEvent;
	nCount = 0;
	events[nCount++] = StopEvent;
	subsystem->base.captureFrameRate = (subsystem->base.server->h264FrameRate <
	                                    SHADOW_CAPTURE_INITIAL_FPS)
	                                       ? subsystem->base.server->h264FrameRate
	                                       : SHADOW_CAPTURE_INITIAL_FPS;
	fps = subsystem->base.captureFrameRate;

	if (fps == 0)
	{
		WLog_ERR(TAG, "Capture frame rate must be greater than zero");
		return ERROR_INVALID_PARAMETER;
	}

	dwInterval = 1000 / fps;
	frameTime = GetTickCount64() + dwInterval;

	while (1)
	{
		dwTimeout = INFINITE;
		cTime = GetTickCount64();
		dwTimeout = (DWORD)((cTime > frameTime) ? 0 : frameTime - cTime);
		status = WaitForMultipleObjects(nCount, events, FALSE, dwTimeout);

		if (WaitForSingleObject(StopEvent, 0) == WAIT_OBJECT_0)
		{
			break;
		}

		if ((status == WAIT_TIMEOUT) || (GetTickCount64() >= frameTime))
		{
			int dxgi_status;
			dxgi_status = win_shadow_dxgi_get_next_frame(subsystem);

			if (dxgi_status > 0)
				dxgi_status = win_shadow_dxgi_get_invalid_region(subsystem);

			if (dxgi_status > 0)
				win_shadow_surface_copy(subsystem);

			fps = subsystem->base.captureFrameRate;
			if (fps == 0)
			{
				WLog_ERR(TAG, "Capture frame rate became invalid");
				break;
			}

			dwInterval = 1000 / fps;
			frameTime = GetTickCount64() + dwInterval;
		}
	}

	ExitThread(0);
	return 0;
}

#endif

/**
 * 枚举 Windows 主显示器并转换为 Shadow 的包含式坐标。
 *
 * GetDeviceCaps 返回像素数量，而 MONITOR_DEF 的 right 与 bottom 是包含端点。这里减一
 * 可避免捕获缓冲区比实际桌面多出一行一列，从而杜绝客户端首帧或刷新时的越界访问。
 * 无法创建设备上下文或输出参数无效时返回零，调用方据此终止初始化。
 */
static UINT32 win_shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors)
{
	HDC hdc;
	int index;
	int desktopWidth;
	int desktopHeight;
	DWORD iDevNum = 0;
	int numMonitors = 0;
	MONITOR_DEF* monitor;
	DISPLAY_DEVICE displayDevice = WINPR_C_ARRAY_INIT;

	if (!monitors || (maxMonitors < 1))
		return 0;

	displayDevice.cb = sizeof(DISPLAY_DEVICE);

	if (EnumDisplayDevices(nullptr, iDevNum, &displayDevice, 0))
	{
		hdc = CreateDC(displayDevice.DeviceName, nullptr, nullptr, nullptr);
		if (!hdc)
			return 0;

		desktopWidth = GetDeviceCaps(hdc, HORZRES);
		desktopHeight = GetDeviceCaps(hdc, VERTRES);
		if ((desktopWidth <= 0) || (desktopHeight <= 0))
		{
			DeleteDC(hdc);
			return 0;
		}

		index = 0;
		numMonitors = 1;
		monitor = &monitors[index];
		monitor->left = 0;
		monitor->top = 0;
		monitor->right = desktopWidth - 1;
		monitor->bottom = desktopHeight - 1;
		monitor->flags = 1;
		DeleteDC(hdc);
	}

	return numMonitors;
}

/**
 * 初始化 Windows Shadow 的显示器描述和桌面复制后端。
 *
 * 函数在服务监听前运行。DXGI/WDS 初始化失败必须原样返回，避免旧实现继续使用未初始化的
 * 设备对象；成功后建立与编码器一致的初始低帧率捕获策略，等待客户端帧确认后再提升，并按
 * 配置预备独立的 WASAPI 系统声音回环。声音后端不可用只会降级声音，不得影响物理桌面视频。
 */
static int win_shadow_subsystem_init(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	int status;
	MONITOR_DEF* virtualScreen;
	const MONITOR_DEF* selectedMonitor;
	INT64 monitorWidth;
	INT64 monitorHeight;

	if (!subsystem)
		return -1;

	subsystem->base.numMonitors = win_shadow_enum_monitors(subsystem->base.monitors, 16);
	if ((subsystem->base.numMonitors < 1) ||
	    (subsystem->base.selectedMonitor >= subsystem->base.numMonitors))
	{
		WLog_ERR(TAG, "未发现可用于物理桌面共享的显示器");
		return -1;
	}

	selectedMonitor = &subsystem->base.monitors[subsystem->base.selectedMonitor];
	monitorWidth = (INT64)selectedMonitor->right - selectedMonitor->left + 1;
	monitorHeight = (INT64)selectedMonitor->bottom - selectedMonitor->top + 1;
	if ((monitorWidth < 1) || (monitorWidth > INT32_MAX) || (monitorHeight < 1) ||
	    (monitorHeight > INT32_MAX))
	{
		WLog_ERR(TAG, "物理显示器尺寸无效：%" PRId64 "x%" PRId64, monitorWidth, monitorHeight);
		return -1;
	}

	/* DXGI 暂存纹理必须与被共享的显示器完全同尺寸；此前未赋值的零尺寸会导致
	 * CreateTexture2D 返回 E_INVALIDARG，服务在监听端口前退出。 */
	subsystem->width = (int)monitorWidth;
	subsystem->height = (int)monitorHeight;
	virtualScreen = &(subsystem->base.virtualScreen);
	virtualScreen->left = 0;
	virtualScreen->top = 0;
	virtualScreen->right = subsystem->width - 1;
	virtualScreen->bottom = subsystem->height - 1;
	virtualScreen->flags = 1;

#if defined(WITH_WDS_API)
	status = win_shadow_wds_init(subsystem);
#elif defined(WITH_DXGI_1_2)
	status = win_shadow_dxgi_init(subsystem);
#endif
	if (status < 0)
		return status;

	if (!win_shadow_privacy_init(subsystem))
		WLog_WARN(TAG, "本机隐私遮罩不可用，远程会话将保持稳定但不会自动黑屏");

	subsystem->base.captureFrameRate = (subsystem->base.server->h264FrameRate <
	                                    SHADOW_CAPTURE_INITIAL_FPS)
	                                       ? subsystem->base.server->h264FrameRate
	                                       : SHADOW_CAPTURE_INITIAL_FPS;
	status = win_shadow_audio_init(subsystem);
	if (status < 0)
		WLog_WARN(TAG, "系统声音初始化失败，继续仅视频会话");
	WLog_INFO(TAG, "width: %d height: %d", subsystem->width, subsystem->height);
	return 1;
}

/**
 * 停止 Windows Shadow 的系统声音并释放捕获后端。
 *
 * 音频线程先于 DXGI/WDS 资源停止，避免异步 RDPSND 消息在桌面后端或客户端队列销毁后仍访问
 * 子系统。视频后端卸载失败会保留其原有返回语义；音频不可用始终不影响视频服务关闭。
 */
static int win_shadow_subsystem_uninit(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem)
		return -1;

	win_shadow_privacy_uninit(subsystem);
	win_shadow_audio_uninit(subsystem);
#if defined(WITH_WDS_API)
	win_shadow_wds_uninit(subsystem);
#elif defined(WITH_DXGI_1_2)
	win_shadow_dxgi_uninit(subsystem);
#endif
	return 1;
}

/**
 * 启动 Windows Shadow 的视频捕获与可选系统声音回环。
 *
 * 视频线程创建失败时返回负数，因为没有画面无法建立会话；音频线程仅是增强能力，失败时记录
 * 警告后继续提供画面和控制，防止无播放设备的机器被整体拒绝服务。
 */
static int win_shadow_subsystem_start(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	HANDLE thread;

	if (!subsystem)
		return -1;

	if (!(thread =
	          CreateThread(nullptr, 0, win_shadow_subsystem_thread, (void*)subsystem, 0, nullptr)))
	{
		WLog_ERR(TAG, "Failed to create thread");
		return -1;
	}

	if (win_shadow_audio_start(subsystem) < 0)
		WLog_WARN(TAG, "系统声音线程启动失败，继续仅视频会话");

	return 1;
}

/**
 * 停止 Windows Shadow 的附属异步任务。
 *
 * 视频捕获线程由服务全局停止事件统一退出；系统声音使用独立事件以避免等待下一次音频回调，
 * 因而此处主动同步停止。参数无效时返回负数，其他情况保证停止操作幂等。
 */
static int win_shadow_subsystem_stop(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem)
		return -1;

	win_shadow_audio_stop(subsystem);
	return 1;
}

/**
 * 释放 Windows Shadow 子系统实例。
 *
 * FreeRDP 的通用销毁路径可能在显式 Uninit 后再次调用本函数，因此依赖幂等的音频与视频释放
 * 逻辑。释放完成后不再保留任何线程句柄或格式缓冲区。
 */
static void win_shadow_subsystem_free(rdpShadowSubsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem)
		return;

	win_shadow_subsystem_uninit(arg);
	free(subsystem);
}

/**
 * 创建并配置 Windows Shadow 子系统实例。
 *
 * 函数在模块加载时为每个服务创建状态对象，并注册输入处理与认证后激活钩子。内存分配失败
 * 返回空指针，调用方会取消服务初始化，避免使用不完整的平台回调表。
 */
static rdpShadowSubsystem* win_shadow_subsystem_new(void)
{
	winShadowSubsystem* subsystem;
	subsystem = (winShadowSubsystem*)calloc(1, sizeof(winShadowSubsystem));

	if (!subsystem)
		return nullptr;

	subsystem->base.SynchronizeEvent = win_shadow_input_synchronize_event;
	subsystem->base.KeyboardEvent = win_shadow_input_keyboard_event;
	subsystem->base.UnicodeKeyboardEvent = win_shadow_input_unicode_keyboard_event;
	subsystem->base.MouseEvent = win_shadow_input_mouse_event;
	subsystem->base.ExtendedMouseEvent = win_shadow_input_extended_mouse_event;
	subsystem->base.ClientActivated = win_shadow_client_activated;
	return &subsystem->base;
}

FREERDP_API const char* ShadowSubsystemName(void)
{
	return "Win";
}

/**
 * 注册 Windows Shadow 子系统的本地回调。
 *
 * 在 Shadow CLI 动态加载模块时调用。维护状态提示属于上游治理信息，并不影响本地运行条件；
 * 本分支省略固定启动横幅，不改变捕获、认证、日志级别或任何安全校验。回调表无效时由加载器
 * 拒绝模块，因此此处仅在注册完成后返回成功。
 */
FREERDP_API int ShadowSubsystemEntry(RDP_SHADOW_ENTRY_POINTS* pEntryPoints)
{
	pEntryPoints->New = win_shadow_subsystem_new;
	pEntryPoints->Free = win_shadow_subsystem_free;
	pEntryPoints->Init = win_shadow_subsystem_init;
	pEntryPoints->Uninit = win_shadow_subsystem_uninit;
	pEntryPoints->Start = win_shadow_subsystem_start;
	pEntryPoints->Stop = win_shadow_subsystem_stop;
	pEntryPoints->EnumMonitors = win_shadow_enum_monitors;
	return 1;
}
