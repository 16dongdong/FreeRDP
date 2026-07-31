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
#define SHADOW_CAPTURE_INITIAL_FPS 30U
#define SHADOW_DISPLAY_BLANK_DELAY_MS 1500U

/* https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mouse_event
 * does not mention this flag is only supported if building for _WIN32_WINNT >= 0x0600
 */
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

static BOOL win_shadow_input_synchronize_event(rdpShadowSubsystem* subsystem,
                                               rdpShadowClient* client, UINT32 flags)
{
	WLog_WARN(TAG, "TODO: Implement!");
	return TRUE;
}

/**
 * 关闭当前交互桌面的物理显示器。
 *
 * 仅发送关闭命令，不会在远程会话断开时重新点亮屏幕；Windows 的原生物理键鼠活动仍可唤醒
 * 显示器。广播带有超时，任一窗口无响应时不会阻塞 RDP 协议线程。
 */
static void win_shadow_blank_local_display(void)
{
	DWORD_PTR messageResult = 0;
	const LRESULT delivered = SendMessageTimeout(
	    HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2, SMTO_ABORTIFHUNG, 1000,
	    &messageResult);

	if (delivered == 0)
		WLog_WARN(TAG, "关闭本地显示器失败: %lu", GetLastError());
}

/**
 * 在首帧有机会送达后异步执行息屏。
 *
 * 此线程不持有客户端或子系统指针，因此服务在延迟窗口内停止时也不会访问已释放内存。延迟将
 * DXGI 显示状态切换从 RDP 激活路径移开，避免客户端长时间停留在“正在配置远程主机”。
 */
static DWORD WINAPI win_shadow_deferred_display_blank_thread(void* arg)
{
	WINPR_UNUSED(arg);
	Sleep(SHADOW_DISPLAY_BLANK_DELAY_MS);
	win_shadow_blank_local_display();
	return 0;
}

/**
 * 在 RDP 图形会话激活后安排本地息屏。
 *
 * 创建的线程立即脱离调用方，故不会阻塞握手；线程创建失败只记录告警，远程会话仍可正常使用。
 */
static void win_shadow_client_activated(rdpShadowSubsystem* subsystem, rdpShadowClient* client)
{
	HANDLE blankThread =
	    CreateThread(nullptr, 0, win_shadow_deferred_display_blank_thread, nullptr, 0, nullptr);

	WINPR_UNUSED(subsystem);
	WINPR_UNUSED(client);

	if (!blankThread)
	{
		WLog_WARN(TAG, "创建延后息屏线程失败: %lu", GetLastError());
		return;
	}

	CloseHandle(blankThread);
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
