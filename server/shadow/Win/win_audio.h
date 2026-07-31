/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Windows Shadow 系统声音回环采集接口。
 *
 * 该模块只负责采集当前默认播放设备并将标准 PCM 帧投递给 Shadow RDPSND 通道；不持有客户端
 * 或网络连接状态，从而保证设备变化、无声卡和客户端断开不会影响物理桌面视频服务。
 */

#ifndef FREERDP_SERVER_SHADOW_WIN_AUDIO_H
#define FREERDP_SERVER_SHADOW_WIN_AUDIO_H

struct win_shadow_subsystem;

#ifdef __cplusplus
extern "C"
{
#endif

	/** 初始化 Windows 系统声音格式；未找到可用播放设备时返回零而不影响视频服务。 */
	int win_shadow_audio_init(struct win_shadow_subsystem* subsystem);

	/** 启动异步 WASAPI 回环采集线程；音频不可用时返回零且调用方可继续提供视频。 */
	int win_shadow_audio_start(struct win_shadow_subsystem* subsystem);

	/** 请求停止并等待系统声音采集线程退出，允许重复调用。 */
	void win_shadow_audio_stop(struct win_shadow_subsystem* subsystem);

	/** 释放系统声音格式和停止事件；调用前会先同步停止采集线程。 */
	void win_shadow_audio_uninit(struct win_shadow_subsystem* subsystem);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_WIN_AUDIO_H */
