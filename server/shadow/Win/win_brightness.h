/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Windows Shadow 本机显示器亮度控制。
 *
 * 此模块通过 WMI 控制活动显示器亮度，而不改变 DXGI 输出状态。它仅供 Windows Shadow 的隐私
 * 模式使用：远程会话可让物理屏幕变暗，本机真实输入恢复原亮度，远程注入输入不会触发恢复。
 */

#ifndef FREERDP_SERVER_SHADOW_WIN_BRIGHTNESS_H
#define FREERDP_SERVER_SHADOW_WIN_BRIGHTNESS_H

#include <winpr/wtypes.h>

typedef struct win_shadow_brightness_controller winShadowBrightnessController;

#ifdef __cplusplus
extern "C"
{
#endif

	/** 创建并初始化活动显示器亮度控制器，失败时返回 nullptr。 */
	winShadowBrightnessController* win_shadow_brightness_new(void);

	/** 恢复亮度并释放控制器及其 WMI 资源。 */
	void win_shadow_brightness_free(winShadowBrightnessController* controller);

	/** 将已发现的活动显示器调暗；没有可用显示器或调用失败时返回 FALSE。 */
	WINPR_ATTR_NODISCARD
	BOOL win_shadow_brightness_dim(winShadowBrightnessController* controller);

	/** 恢复此前由本控制器调暗的显示器；部分失败时返回 FALSE。 */
	WINPR_ATTR_NODISCARD
	BOOL win_shadow_brightness_restore(winShadowBrightnessController* controller);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_WIN_BRIGHTNESS_H */
