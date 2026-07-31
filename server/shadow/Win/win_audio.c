/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Windows Shadow 系统声音回环采集实现。
 *
 * WASAPI 返回的混音格式通常是浮点或多声道格式，而 RDPSND 客户端的通用交集是 PCM。此模块
 * 将默认播放设备的回环数据转换为双声道 16 位 PCM，再交由已有 RDPSND 协商与发送路径处理。
 * 这样视频捕获、输入控制和音频传输相互独立，音频设备不可用时不会使远程桌面服务失效。
 */

#include <freerdp/config.h>

#include "../shadow_rdpsnd.h"
#include "win_shadow.h"

#include <windows.h>
#ifdef WAVE_FORMAT_OPUS
#undef WAVE_FORMAT_OPUS
#endif
#ifdef WAVE_FORMAT_EXTENSIBLE
#undef WAVE_FORMAT_EXTENSIBLE
#endif
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#include <winpr/assert.h>
#include <winpr/crt.h>
#include <winpr/wlog.h>

#define TAG SERVER_TAG("shadow.win.audio")

/* MinGW 的部分 Windows SDK 头只声明 IAudioClient，未声明这些官方标志位。 */
#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#endif

#ifndef AUDCLNT_STREAMFLAGS_EVENTCALLBACK
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00040000
#endif

typedef struct
{
	IAudioClient* client;
	IAudioCaptureClient* capture;
	WAVEFORMATEX* format;
	HANDLE sampleEvent;
} winShadowAudioCapture;

/**
 * 释放一个 WASAPI 采集会话的 COM 对象和事件。
 *
 * 该函数接受部分初始化的结构，适用于所有失败分支；先停止客户端，确保系统不会继续向即将
 * 释放的事件投递采样通知。
 */
static void win_shadow_audio_capture_uninit(winShadowAudioCapture* capture)
{
	if (!capture)
		return;

	if (capture->client)
		(void)capture->client->lpVtbl->Stop(capture->client);
	if (capture->capture)
		capture->capture->lpVtbl->Release(capture->capture);
	if (capture->client)
		capture->client->lpVtbl->Release(capture->client);
	if (capture->format)
		CoTaskMemFree(capture->format);
	if (capture->sampleEvent)
		(void)CloseHandle(capture->sampleEvent);

	ZeroMemory(capture, sizeof(*capture));
}

/**
 * 获取当前默认播放设备对应的 IAudioClient。
 *
 * 调用线程必须已初始化 COM。未配置播放设备或设备已失效时返回负数，由调用方将音频降级为
 * 不可用；枚举器和设备引用在返回前释放，只有 IAudioClient 的所有权移交给调用方。
 */
static int win_shadow_audio_create_client(IAudioClient** result)
{
	IMMDeviceEnumerator* enumerator = nullptr;
	IMMDevice* endpoint = nullptr;
	IAudioClient* client = nullptr;
	HRESULT hr = S_OK;
	int status = -1;

	if (!result)
		return -1;
	*result = nullptr;

	hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
	                      &IID_IMMDeviceEnumerator, (void**)&enumerator);
	if (FAILED(hr))
		goto out;

	hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &endpoint);
	if (FAILED(hr))
		goto out;

	hr = endpoint->lpVtbl->Activate(endpoint, &IID_IAudioClient, CLSCTX_INPROC_SERVER, nullptr,
	                                 (void**)&client);
	if (FAILED(hr))
		goto out;

	*result = client;
	client = nullptr;
	status = 1;

out:
	if (client)
		client->lpVtbl->Release(client);
	if (endpoint)
		endpoint->lpVtbl->Release(endpoint);
	if (enumerator)
		enumerator->lpVtbl->Release(enumerator);
	return status;
}

/**
 * 判断 WASAPI 混音格式是否为可安全转换的 PCM 或 IEEE 浮点流。
 *
 * 输出始终是 16 位 PCM，因此输入仅接受 8/16/24/32 位整数 PCM 或 32 位浮点；未知压缩格式
 * 直接拒绝，避免把非线性编码数据按采样值解释而输出噪声。
 */
static BOOL win_shadow_audio_source_format_supported(const WAVEFORMATEX* format)
{
	BOOL isPcm = FALSE;
	BOOL isFloat = FALSE;

	if (!format || (format->nChannels < 1) || (format->nSamplesPerSec < 1) ||
	    (format->nBlockAlign < 1))
		return FALSE;

	if (format->wFormatTag == WAVE_FORMAT_PCM)
		isPcm = TRUE;
	else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		isFloat = TRUE;
	else if ((format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) &&
	         (format->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))))
	{
		const WAVEFORMATEXTENSIBLE* extensible = (const WAVEFORMATEXTENSIBLE*)format;
		isPcm = IsEqualGUID(&extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM);
		isFloat = IsEqualGUID(&extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
	}

	if (isFloat)
		return format->wBitsPerSample == 32;
	if (!isPcm)
		return FALSE;

	return (format->wBitsPerSample == 8) || (format->wBitsPerSample == 16) ||
	       (format->wBitsPerSample == 24) || (format->wBitsPerSample == 32);
}

/**
 * 判断输入混音格式是否为 IEEE 浮点格式。
 *
 * 扩展波形格式用 SubFormat 区分 32 位整数与 32 位浮点；不能只依赖位深，否则高解析度整数
 * 音频会被错误解释为浮点并产生爆音。
 */
static BOOL win_shadow_audio_source_is_float(const WAVEFORMATEX* format)
{
	if (!format)
		return FALSE;
	if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return TRUE;
	if ((format->wFormatTag != WAVE_FORMAT_EXTENSIBLE) ||
	    (format->cbSize < (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))))
		return FALSE;

	const WAVEFORMATEXTENSIBLE* extensible = (const WAVEFORMATEXTENSIBLE*)format;
	return IsEqualGUID(&extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

/**
 * 查询默认播放设备的混音格式并初始化 Shadow 的 RDPSND 广播格式。
 *
 * 服务端以双声道 16 位 PCM 广播，采样率保持与 Windows 混音器一致，避免额外重采样延迟。若
 * 当前设备不存在或格式无法转换，返回零并让视频服务继续运行；资源分配错误返回负数。
 */
int win_shadow_audio_init(struct win_shadow_subsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	IAudioClient* client = nullptr;
	WAVEFORMATEX* format = nullptr;
	AUDIO_FORMAT* rdpsndFormats = nullptr;
	HRESULT hr = S_OK;
	BOOL uninitializeCom = FALSE;
	int status = 0;

	if (!subsystem || !subsystem->base.server || !subsystem->base.server->systemAudio)
		return 0;
	if (subsystem->base.rdpsndFormats)
		return 1;

	hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr))
		uninitializeCom = TRUE;
	else if (hr != RPC_E_CHANGED_MODE)
		goto out;

	if (win_shadow_audio_create_client(&client) < 0)
		goto out;
	if (FAILED(client->lpVtbl->GetMixFormat(client, &format)) ||
	    !win_shadow_audio_source_format_supported(format) ||
	    (format->nSamplesPerSec > (UINT32_MAX / 4)))
		goto out;

	rdpsndFormats = audio_formats_new(1);
	if (!rdpsndFormats)
	{
		status = -1;
		goto out;
	}

	rdpsndFormats[0].wFormatTag = WAVE_FORMAT_PCM;
	rdpsndFormats[0].nChannels = 2;
	rdpsndFormats[0].nSamplesPerSec = format->nSamplesPerSec;
	rdpsndFormats[0].nAvgBytesPerSec = format->nSamplesPerSec * 4;
	rdpsndFormats[0].nBlockAlign = 4;
	rdpsndFormats[0].wBitsPerSample = 16;
	rdpsndFormats[0].cbSize = 0;
	subsystem->base.rdpsndFormats = rdpsndFormats;
	subsystem->base.nRdpsndFormats = 1;
	rdpsndFormats = nullptr;

	if (!(subsystem->audioStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr)))
	{
		audio_formats_free(subsystem->base.rdpsndFormats, subsystem->base.nRdpsndFormats);
		subsystem->base.rdpsndFormats = nullptr;
		subsystem->base.nRdpsndFormats = 0;
		status = -1;
		goto out;
	}

	WLog_INFO(TAG, "系统声音已启用: %" PRIu32 " Hz PCM", format->nSamplesPerSec);
	status = 1;

out:
	if (rdpsndFormats)
		audio_formats_free(rdpsndFormats, 1);
	if (format)
		CoTaskMemFree(format);
	if (client)
		client->lpVtbl->Release(client);
	if (uninitializeCom)
		CoUninitialize();
	if (status == 0)
		WLog_WARN(TAG, "未检测到可用于系统声音回环的默认播放设备，继续仅视频会话");
	return status;
}

/**
 * 将一个输入采样转换为 16 位有符号 PCM。
 *
 * 输入指针必须指向单个声道的有效样本。该转换仅由已验证的 WAV 格式调用，所有整数路径保留
 * 符号位并在降位深时截断低位；浮点路径钳制到 [-1, 1]，避免驱动报告越界数值造成溢出。
 */
static INT16 win_shadow_audio_convert_sample(const BYTE* source, const WAVEFORMATEX* format)
{
	if (win_shadow_audio_source_is_float(format))
	{
		float value = 0.0f;
		memcpy(&value, source, sizeof(value));
		if (value >= 1.0f)
			return INT16_MAX;
		if (value <= -1.0f)
			return INT16_MIN;
		return (INT16)(value * INT16_MAX);
	}

	switch (format->wBitsPerSample)
	{
		case 8:
			return (INT16)((INT16)source[0] - 128) << 8;

		case 16:
		{
			INT16 value = 0;
			memcpy(&value, source, sizeof(value));
			return value;
		}

		case 24:
		{
			INT32 value = ((INT32)source[0]) | ((INT32)source[1] << 8) |
			              ((INT32)source[2] << 16);
			if ((value & 0x00800000) != 0)
				value |= (INT32)0xFF000000;
			return (INT16)(value >> 8);
		}

		case 32:
		{
			INT32 value = 0;
			memcpy(&value, source, sizeof(value));
			return (INT16)(value >> 16);
		}

		default:
			return 0;
	}
}

/**
 * 转换并投递一批 WASAPI 捕获帧。
 *
 * RDPSND 格式由初始化阶段固定为双声道 16 位 PCM；单声道输入复制到两个声道，多声道输入取
 * 左右前声道。无客户端时广播函数会自行释放消息，因而不积压音频也不阻塞回环线程。
 */
static void win_shadow_audio_publish_frames(winShadowSubsystem* subsystem, const BYTE* source,
	                                            UINT32 frameCount, const WAVEFORMATEX* format)
{
	INT16* samples = nullptr;
	size_t outputBytes = 0;
	const size_t sourceBytesPerSample = format->wBitsPerSample / 8;

	if (!subsystem || !source || !format || !subsystem->base.rdpsndFormats ||
	    (subsystem->base.nRdpsndFormats != 1) || (sourceBytesPerSample == 0) ||
	    (format->nBlockAlign < sourceBytesPerSample * format->nChannels) ||
	    (frameCount > (SIZE_MAX / (2 * sizeof(INT16)))))
		return;

	outputBytes = (size_t)frameCount * 2 * sizeof(INT16);
	samples = (INT16*)malloc(outputBytes);
	if (!samples)
		return;

	for (UINT32 index = 0; index < frameCount; index++)
	{
		const BYTE* frame = source + ((size_t)index * format->nBlockAlign);
		const BYTE* left = frame;
		const BYTE* right = (format->nChannels > 1) ? frame + sourceBytesPerSample : left;
		samples[index * 2] = win_shadow_audio_convert_sample(left, format);
		samples[(index * 2) + 1] = win_shadow_audio_convert_sample(right, format);
	}

	const BOOL dispatched = shadow_client_broadcast_audio_samples(
	    subsystem->base.server, &subsystem->base.rdpsndFormats[0], samples, frameCount,
	    (UINT16)(GetTickCount64() & UINT16_MAX));
	WINPR_UNUSED(dispatched);
	free(samples);
}

/**
 * 提取当前 WASAPI 事件中的全部音频包。
 *
 * 每个包都必须在下一次 GetBuffer 前 ReleaseBuffer。静音包不发送，远端播放端会自然维持静音；
 * 其他包转换后异步广播，客户端拥塞不会阻塞 Windows 音频引擎。
 */
static int win_shadow_audio_drain_packets(winShadowSubsystem* subsystem,
	                                          winShadowAudioCapture* capture)
{
	UINT32 packetFrames = 0;
	HRESULT hr = S_OK;

	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(capture);
	WINPR_ASSERT(capture->capture);
	WINPR_ASSERT(capture->format);

	while (TRUE)
	{
		BYTE* source = nullptr;
		UINT32 frameCount = 0;
		DWORD flags = 0;

		hr = capture->capture->lpVtbl->GetNextPacketSize(capture->capture, &packetFrames);
		if (FAILED(hr))
			return -1;
		if (packetFrames == 0)
			return 1;

		hr = capture->capture->lpVtbl->GetBuffer(capture->capture, &source, &frameCount, &flags,
		                                              nullptr, nullptr);
		if (FAILED(hr))
			return -1;

		if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0)
			win_shadow_audio_publish_frames(subsystem, source, frameCount, capture->format);

		hr = capture->capture->lpVtbl->ReleaseBuffer(capture->capture, frameCount);
		if (FAILED(hr))
			return -1;
	}
}

/**
 * 打开默认播放设备的 WASAPI 回环流。
 *
 * 采集使用共享模式和事件回调，避免固定轮询引入额外延迟。设备格式在启动时再次验证，若默认
 * 设备切换成与已协商采样率不一致的格式则停止音频而保留视频，避免向客户端声明错误格式。
 */
static int win_shadow_audio_capture_init(winShadowSubsystem* subsystem,
	                                        winShadowAudioCapture* capture)
{
	HRESULT hr = S_OK;

	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(capture);

	if (win_shadow_audio_create_client(&capture->client) < 0)
		return -1;
	if (FAILED(capture->client->lpVtbl->GetMixFormat(capture->client, &capture->format)) ||
	    !win_shadow_audio_source_format_supported(capture->format))
		return -1;
	if (!subsystem->base.rdpsndFormats || (subsystem->base.nRdpsndFormats != 1) ||
	    (capture->format->nSamplesPerSec != subsystem->base.rdpsndFormats[0].nSamplesPerSec))
		return -1;

	capture->sampleEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!capture->sampleEvent)
		return -1;

	hr = capture->client->lpVtbl->Initialize(
	    capture->client, AUDCLNT_SHAREMODE_SHARED,
	    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, capture->format,
	    nullptr);
	if (FAILED(hr))
		return -1;
	if (FAILED(capture->client->lpVtbl->SetEventHandle(capture->client, capture->sampleEvent)))
		return -1;
	if (FAILED(capture->client->lpVtbl->GetService(capture->client, &IID_IAudioCaptureClient,
	                                              (void**)&capture->capture)))
		return -1;
	if (FAILED(capture->client->lpVtbl->Start(capture->client)))
		return -1;

	return 1;
}

/**
 * 运行 Windows 默认播放设备的回环采集线程。
 *
 * 线程独立初始化 COM，并同时等待停止事件与 WASAPI 样本事件。任何设备错误都会只结束音频
 * 线程并写入日志，绝不影响 Shadow 视频捕获或远程键鼠控制。
 */
static DWORD WINAPI win_shadow_audio_thread(LPVOID arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;
	winShadowAudioCapture capture = WINPR_C_ARRAY_INIT;
	HRESULT hr = S_OK;
	BOOL uninitializeCom = FALSE;
	DWORD result = ERROR_GEN_FAILURE;

	if (!subsystem || !subsystem->audioStopEvent)
		return ERROR_INVALID_PARAMETER;

	hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr))
		uninitializeCom = TRUE;
	else if (hr != RPC_E_CHANGED_MODE)
		goto out;

	if (win_shadow_audio_capture_init(subsystem, &capture) < 0)
	{
		WLog_WARN(TAG, "无法启动系统声音回环采集，继续仅视频会话");
		result = ERROR_NOT_READY;
		goto out;
	}

	while (TRUE)
	{
		HANDLE events[2] = { subsystem->audioStopEvent, capture.sampleEvent };
		const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE);

		if (wait == WAIT_OBJECT_0)
		{
			result = ERROR_SUCCESS;
			break;
		}
		if (wait != (WAIT_OBJECT_0 + 1))
		{
			WLog_WARN(TAG, "系统声音等待失败: %lu", GetLastError());
			break;
		}
		if (win_shadow_audio_drain_packets(subsystem, &capture) < 0)
		{
			WLog_WARN(TAG, "系统声音采集失败，停止音频而保持视频会话");
			break;
		}
	}

out:
	win_shadow_audio_capture_uninit(&capture);
	if (uninitializeCom)
		CoUninitialize();
	return result;
}

/**
 * 启动 Windows 系统声音回环线程。
 *
 * 初始化阶段未找到可用设备时该函数返回零；线程创建失败返回负数。重复调用会复用现有线程，
 * 从而避免会话重连时重复占用同一 WASAPI 设备。
 */
int win_shadow_audio_start(struct win_shadow_subsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem || !subsystem->base.server || !subsystem->base.server->systemAudio)
		return 0;
	if (!subsystem->base.rdpsndFormats || !subsystem->audioStopEvent)
		return 0;
	if (subsystem->audioThread)
		return 1;
	if (!ResetEvent(subsystem->audioStopEvent))
		return -1;

	if (!(subsystem->audioThread =
	          CreateThread(nullptr, 0, win_shadow_audio_thread, subsystem, 0, nullptr)))
	{
		WLog_WARN(TAG, "无法创建系统声音线程: %lu", GetLastError());
		return -1;
	}

	return 1;
}

/**
 * 停止并回收 Windows 系统声音线程。
 *
 * 先通知 WASAPI 等待循环，再同步等待线程退出，保证所有异步 RDPSND 消息都不再引用即将释放的
 * 子系统状态。函数可重入，已停止或未启动音频时不执行任何操作。
 */
void win_shadow_audio_stop(struct win_shadow_subsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem || !subsystem->audioThread)
		return;

	if (subsystem->audioStopEvent)
		(void)SetEvent(subsystem->audioStopEvent);
	(void)WaitForSingleObject(subsystem->audioThread, INFINITE);
	(void)CloseHandle(subsystem->audioThread);
	subsystem->audioThread = nullptr;
}

/**
 * 释放 Windows 系统声音资源。
 *
 * 资源只由 Windows 子系统拥有；RDPSND 客户端在初始化时已经深拷贝格式，因此停止并回收所有
 * 客户端后可安全释放共享格式。未启用音频的服务同样可调用本函数。
 */
void win_shadow_audio_uninit(struct win_shadow_subsystem* arg)
{
	winShadowSubsystem* subsystem = (winShadowSubsystem*)arg;

	if (!subsystem)
		return;

	win_shadow_audio_stop(subsystem);
	if (subsystem->audioStopEvent)
		(void)CloseHandle(subsystem->audioStopEvent);
	subsystem->audioStopEvent = nullptr;
	if (subsystem->base.rdpsndFormats)
		audio_formats_free(subsystem->base.rdpsndFormats, subsystem->base.nRdpsndFormats);
	subsystem->base.rdpsndFormats = nullptr;
	subsystem->base.nRdpsndFormats = 0;
}
