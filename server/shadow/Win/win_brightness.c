/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Windows Shadow 的 WMI 显示器亮度后端。
 *
 * DXGI Desktop Duplication 会把覆盖窗口和物理 DPMS 状态直接反映到远程画面。为同时维持本机
 * 隐私与连续桌面捕获，本模块只修改显示器亮度，保持桌面像素和 DXGI 输出拓扑不变。
 */

#include <windows.h>
#include <wbemidl.h>
#include <oleauto.h>

#include <inttypes.h>
#include <wchar.h>

#include <freerdp/log.h>

#include <winpr/crt.h>

#include "win_brightness.h"

#define TAG SERVER_TAG("shadow.win.brightness")
#define WIN_SHADOW_MAX_BRIGHTNESS_DISPLAYS 16U

typedef struct
{
	BSTR instanceName;
	BSTR methodPath;
	BYTE originalBrightness;
	BOOL dimmed;
} winShadowBrightnessDisplay;

struct win_shadow_brightness_controller
{
	IWbemLocator* locator;
	IWbemServices* services;
	BOOL comInitialized;
	UINT32 count;
	winShadowBrightnessDisplay displays[WIN_SHADOW_MAX_BRIGHTNESS_DISPLAYS];
};

/**
 * 将 WMI VARIANT 的无符号亮度值转换为 BYTE。
 *
 * WMI 在不同驱动中可能返回 VT_UI1、VT_UI2 或 VT_UI4；超出亮度范围或类型不匹配时返回 FALSE，
 * 防止向恢复路径写入未经验证的数值。
 */
static BOOL win_shadow_brightness_variant_to_byte(const VARIANT* value, BYTE* brightness)
{
	UINT32 converted = 0;

	if (!value || !brightness)
		return FALSE;

	switch (value->vt)
	{
		case VT_UI1:
			converted = value->bVal;
			break;
		case VT_UI2:
			converted = value->uiVal;
			break;
		case VT_UI4:
			converted = value->ulVal;
			break;
		default:
			return FALSE;
	}

	if (converted > 100)
		return FALSE;

	*brightness = (BYTE)converted;
	return TRUE;
}

/**
 * 释放单个显示器的 WMI 标识符。
 *
 * 标识符由 SysAllocString 分配，释放后清空整个结构，避免后续恢复逻辑重复调用失效路径。
 */
static void win_shadow_brightness_display_uninit(winShadowBrightnessDisplay* display)
{
	if (!display)
		return;

	SysFreeString(display->instanceName);
	SysFreeString(display->methodPath);
	*display = (winShadowBrightnessDisplay)WINPR_C_ARRAY_INIT;
}

/**
 * 释放控制器中已发现的全部显示器信息。
 *
 * 查询中途失败时同样调用此函数，使构造失败路径与正常销毁路径保持相同的资源语义。
 */
static void win_shadow_brightness_displays_uninit(winShadowBrightnessController* controller)
{
	if (!controller)
		return;

	for (UINT32 index = 0; index < controller->count; index++)
		win_shadow_brightness_display_uninit(&controller->displays[index]);

	controller->count = 0;
}

/**
 * 执行只读 WMI 查询并返回枚举器。
 *
 * 查询语言和文本均由模块固定，不接受外部输入；分配或 WMI 服务调用失败时释放所有临时 BSTR
 * 并返回 FALSE。
 */
static BOOL win_shadow_brightness_query(winShadowBrightnessController* controller,
                                      const WCHAR* query, IEnumWbemClassObject** enumerator)
{
	BSTR language = nullptr;
	BSTR statement = nullptr;
	HRESULT status;

	if (!controller || !controller->services || !query || !enumerator)
		return FALSE;

	*enumerator = nullptr;
	language = SysAllocString(L"WQL");
	statement = SysAllocString(query);
	if (!language || !statement)
		goto fail;

	status = controller->services->lpVtbl->ExecQuery(
	    controller->services, language, statement, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
	    nullptr, enumerator);
	SysFreeString(language);
	SysFreeString(statement);
	return SUCCEEDED(status) && *enumerator;

fail:
	SysFreeString(language);
	SysFreeString(statement);
	return FALSE;
}

/**
 * 读取活动显示器的当前亮度和实例名称。
 *
 * 每个结果都必须同时拥有有效实例名称和 0-100 范围内的亮度；不完整条目被跳过，避免影响其他
 * 显示器。没有任何可用条目时返回 FALSE。
 */
static BOOL win_shadow_brightness_collect_levels(winShadowBrightnessController* controller)
{
	static const WCHAR query[] = L"SELECT * FROM WmiMonitorBrightness WHERE Active=TRUE";
	IEnumWbemClassObject* enumerator = nullptr;
	IWbemClassObject* result = nullptr;
	ULONG returned = 0;
	BOOL success = FALSE;

	if (!win_shadow_brightness_query(controller, query, &enumerator))
		goto out;

	while ((controller->count < WIN_SHADOW_MAX_BRIGHTNESS_DISPLAYS) &&
	       SUCCEEDED(enumerator->lpVtbl->Next(enumerator, WBEM_INFINITE, 1, &result, &returned)) &&
	       (returned == 1))
	{
		VARIANT instanceName = WINPR_C_ARRAY_INIT;
		VARIANT currentBrightness = WINPR_C_ARRAY_INIT;
		winShadowBrightnessDisplay* display = &controller->displays[controller->count];

		if (SUCCEEDED(result->lpVtbl->Get(result, L"InstanceName", 0, &instanceName, nullptr, nullptr)) &&
		    SUCCEEDED(result->lpVtbl->Get(result, L"CurrentBrightness", 0, &currentBrightness, nullptr,
		                                 nullptr)) &&
		    (instanceName.vt == VT_BSTR) && instanceName.bstrVal &&
		    win_shadow_brightness_variant_to_byte(&currentBrightness, &display->originalBrightness))
		{
			display->instanceName = SysAllocString(instanceName.bstrVal);
			if (display->instanceName)
				controller->count++;
		}

		VariantClear(&instanceName);
		VariantClear(&currentBrightness);
		result->lpVtbl->Release(result);
		result = nullptr;
	}

	success = controller->count > 0;
out:
	if (result)
		result->lpVtbl->Release(result);
	if (enumerator)
		enumerator->lpVtbl->Release(enumerator);
	return success;
}

/**
 * 将 WMI 亮度方法对象与已保存的显示器亮度配对。
 *
 * 方法路径和显示器实例名称必须匹配。无可调用方法的显示器不会被调暗，从而保证恢复操作只作用
 * 于本模块确实控制过的输出。
 */
static BOOL win_shadow_brightness_collect_methods(winShadowBrightnessController* controller)
{
	static const WCHAR query[] = L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE";
	IEnumWbemClassObject* enumerator = nullptr;
	IWbemClassObject* result = nullptr;
	ULONG returned = 0;
	UINT32 available = 0;

	if (!win_shadow_brightness_query(controller, query, &enumerator))
		goto out;

	while (SUCCEEDED(enumerator->lpVtbl->Next(enumerator, WBEM_INFINITE, 1, &result, &returned)) &&
	       (returned == 1))
	{
		VARIANT instanceName = WINPR_C_ARRAY_INIT;
		VARIANT methodPath = WINPR_C_ARRAY_INIT;

		if (SUCCEEDED(result->lpVtbl->Get(result, L"InstanceName", 0, &instanceName, nullptr, nullptr)) &&
		    SUCCEEDED(result->lpVtbl->Get(result, L"__PATH", 0, &methodPath, nullptr, nullptr)) &&
		    (instanceName.vt == VT_BSTR) && instanceName.bstrVal && (methodPath.vt == VT_BSTR) &&
		    methodPath.bstrVal)
		{
			for (UINT32 index = 0; index < controller->count; index++)
			{
				winShadowBrightnessDisplay* display = &controller->displays[index];
				if (display->instanceName && !display->methodPath &&
				    (wcscmp(display->instanceName, instanceName.bstrVal) == 0))
				{
					display->methodPath = SysAllocString(methodPath.bstrVal);
					if (display->methodPath)
						available++;
					break;
				}
			}
		}

		VariantClear(&instanceName);
		VariantClear(&methodPath);
		result->lpVtbl->Release(result);
		result = nullptr;
	}

out:
	if (result)
		result->lpVtbl->Release(result);
	if (enumerator)
		enumerator->lpVtbl->Release(enumerator);
	return available > 0;
}

/**
 * 调用单个显示器的 WmiSetBrightness 方法。
 *
 * Timeout 固定为零，亮度写入不会阻塞 RDP 线程；该函数只在隐私线程执行。该 WMI 方法没有
 * 输出参数，因此仅在 ExecMethod 或其前置 COM 调用失败时返回 FALSE。
 */
static BOOL win_shadow_brightness_set(winShadowBrightnessController* controller,
                                    const BSTR methodPath, BYTE brightness)
{
	IWbemClassObject* methodClass = nullptr;
	IWbemClassObject* inputClass = nullptr;
	IWbemClassObject* input = nullptr;
	BSTR className = nullptr;
	BSTR methodName = nullptr;
	VARIANT value = WINPR_C_ARRAY_INIT;
	HRESULT status;
	BOOL success = FALSE;

	if (!controller || !controller->services || !methodPath)
		goto out;

	className = SysAllocString(L"WmiMonitorBrightnessMethods");
	methodName = SysAllocString(L"WmiSetBrightness");
	if (!className || !methodName)
		goto out;

	status = controller->services->lpVtbl->GetObject(controller->services, className, 0, nullptr,
	                                                 &methodClass, nullptr);
	if (FAILED(status))
		goto out;

	status = methodClass->lpVtbl->GetMethod(methodClass, methodName, 0, &inputClass, nullptr);
	if (FAILED(status))
		goto out;

	status = inputClass->lpVtbl->SpawnInstance(inputClass, 0, &input);
	if (FAILED(status))
		goto out;

	value.vt = VT_UI4;
	value.ulVal = 0;
	status = input->lpVtbl->Put(input, L"Timeout", 0, &value, 0);
	if (FAILED(status))
		goto out;

	value.vt = VT_UI1;
	value.bVal = brightness;
	status = input->lpVtbl->Put(input, L"Brightness", 0, &value, 0);
	if (FAILED(status))
		goto out;

	status = controller->services->lpVtbl->ExecMethod(controller->services, methodPath, methodName, 0,
	                                                  nullptr, input, nullptr, nullptr);
	if (SUCCEEDED(status))
		success = TRUE;

out:
	VariantClear(&value);
	SysFreeString(className);
	SysFreeString(methodName);
	if (input)
		input->lpVtbl->Release(input);
	if (inputClass)
		inputClass->lpVtbl->Release(inputClass);
	if (methodClass)
		methodClass->lpVtbl->Release(methodClass);
	return success;
}

/**
 * 创建用于本机隐私模式的显示器亮度控制器。
 *
 * 控制器连接 root\\WMI，保存每个活动显示器的原亮度和调用路径。COM 或 WMI 不可用时完全释放
 * 已分配资源并返回 nullptr，使远程服务保持可用而不尝试不安全的 DPMS 或遮罩回退。
 */
winShadowBrightnessController* win_shadow_brightness_new(void)
{
	winShadowBrightnessController* controller = nullptr;
	BSTR nameSpace = nullptr;
	HRESULT status;

	controller = calloc(1, sizeof(*controller));
	if (!controller)
		goto fail;

	status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(status))
		goto fail;
	controller->comInitialized = TRUE;

	status = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
	                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
	if (FAILED(status) && (status != RPC_E_TOO_LATE))
		goto fail;

	status = CoCreateInstance(&CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, &IID_IWbemLocator,
	                          (void**)&controller->locator);
	if (FAILED(status))
		goto fail;

	nameSpace = SysAllocString(L"ROOT\\WMI");
	if (!nameSpace)
		goto fail;

	status = controller->locator->lpVtbl->ConnectServer(controller->locator, nameSpace, nullptr,
	                                                    nullptr, nullptr, 0, nullptr, nullptr,
	                                                    &controller->services);
	SysFreeString(nameSpace);
	nameSpace = nullptr;
	if (FAILED(status))
		goto fail;

	status = CoSetProxyBlanket((IUnknown*)controller->services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
	                           nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
	                           EOAC_NONE);
	if (FAILED(status) || !win_shadow_brightness_collect_levels(controller) ||
	    !win_shadow_brightness_collect_methods(controller))
		goto fail;

	WLog_INFO(TAG, "已启用 %" PRIu32 " 个本机显示器的亮度隐私控制", controller->count);
	return controller;

fail:
	SysFreeString(nameSpace);
	win_shadow_brightness_free(controller);
	return nullptr;
}

/**
 * 将本机显示器调暗以保护远程会话期间的物理屏幕隐私。
 *
 * 每个成功写入的显示器单独标记，后续恢复只恢复确实由本模块修改的输出；全部失败时返回 FALSE。
 */
BOOL win_shadow_brightness_dim(winShadowBrightnessController* controller)
{
	BOOL success = FALSE;

	if (!controller)
		return FALSE;

	for (UINT32 index = 0; index < controller->count; index++)
	{
		winShadowBrightnessDisplay* display = &controller->displays[index];
		if (display->methodPath && !display->dimmed &&
		    win_shadow_brightness_set(controller, display->methodPath, 0))
		{
			display->dimmed = TRUE;
			success = TRUE;
		}
	}

	return success;
}

/**
 * 恢复本模块调暗前记录的显示器亮度。
 *
 * 远程断开不会调用本函数；它仅在本机真实键鼠输入或服务停止时执行。恢复失败的显示器保留
 * dimmed 标记，以便下一次物理输入仍可重试。
 */
BOOL win_shadow_brightness_restore(winShadowBrightnessController* controller)
{
	BOOL success = TRUE;

	if (!controller)
		return FALSE;

	for (UINT32 index = 0; index < controller->count; index++)
	{
		winShadowBrightnessDisplay* display = &controller->displays[index];
		if (display->dimmed &&
		    win_shadow_brightness_set(controller, display->methodPath, display->originalBrightness))
		{
			display->dimmed = FALSE;
		}
		else if (display->dimmed)
		{
			success = FALSE;
		}
	}

	return success;
}

/**
 * 释放亮度控制器。
 *
 * 调用方应先恢复亮度；本函数仍执行一次恢复作为防御性保障，随后按 COM 初始化顺序释放 WMI
 * 接口、BSTR 和线程级 COM 状态。
 */
void win_shadow_brightness_free(winShadowBrightnessController* controller)
{
	BOOL restored = FALSE;

	if (!controller)
		return;

	restored = win_shadow_brightness_restore(controller);
	WINPR_UNUSED(restored);
	win_shadow_brightness_displays_uninit(controller);
	if (controller->services)
		controller->services->lpVtbl->Release(controller->services);
	if (controller->locator)
		controller->locator->lpVtbl->Release(controller->locator);
	if (controller->comInitialized)
		CoUninitialize();
	free(controller);
}
