/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2015 Jiang Zihao <zihao.jiang@yahoo.com>
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

#include <freerdp/config.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/cast.h>

#include <freerdp/log.h>
#include <freerdp/codec/dsp.h>
#include <freerdp/server/server-common.h>

#include "shadow.h"

#include "shadow_rdpsnd.h"

#define TAG SERVER_TAG("shadow")

static void rdpsnd_activated(RdpsndServerContext* context)
{
	WINPR_ASSERT(context);
	for (size_t i = 0; i < context->num_client_formats; i++)
	{
		for (size_t j = 0; j < context->num_server_formats; j++)
		{
			if (audio_format_compatible(&context->server_formats[j], &context->client_formats[i]))
			{
				const UINT rc = context->SelectFormat(context, WINPR_ASSERTING_INT_CAST(UINT16, i));
				if (rc != CHANNEL_RC_OK)
					WLog_WARN(TAG, "SelectFormat failed with %" PRIu32, rc);
				return;
			}
		}
	}

	WLog_ERR(TAG, "Could not agree on a audio format with the server\n");
}

/**
 * 释放异步 RDPSND 音频消息持有的样本缓冲区。
 *
 * Shadow 客户端线程会在广播消息的最后一个引用释放时调用此函数。消息缓冲区属于消息本身，
 * 不能引用 WASAPI 的瞬时捕获缓冲区，否则客户端网络积压时会发生悬垂指针访问。
 */
static void shadow_rdpsnd_free_audio_samples(UINT32 messageId, SHADOW_MSG_OUT* common)
{
	SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES* msg = (SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES*)common;

	WINPR_UNUSED(messageId);
	if (!msg)
		return;

	free(msg->buf);
	free(msg);
}

/**
 * 释放 Shadow 客户端私有的 RDPSND 服务端格式副本。
 *
 * Shadow 使用深拷贝隔离子系统与客户端的格式生命周期，而 RDPSND 上下文的通用析构函数仅
 * 释放格式数组本身。这里先释放每个扩展格式的附加数据，再清空上下文字段，保证普通 PCM 与
 * 带私有格式数据的编码都能在连接失败、断线和服务关闭时无泄漏地销毁。
 */
static void shadow_client_rdpsnd_free_server_formats(RdpsndServerContext* rdpsnd)
{
	if (!rdpsnd || !rdpsnd->server_formats)
		return;

	audio_formats_free(rdpsnd->server_formats, rdpsnd->num_server_formats);
	rdpsnd->server_formats = nullptr;
	rdpsnd->num_server_formats = 0;
	rdpsnd->src_format = nullptr;
}

/**
 * 为一个 Shadow 客户端初始化 RDPSND 通道并复制子系统声明的音频格式。
 *
 * 子系统格式由所有客户端共享，而 RDPSND 上下文销毁时会释放自己的格式数组。这里深拷贝可
 * 防止第一个客户端断开时释放共享格式，随后客户端再连接时触发 use-after-free。协商失败时
 * 释放完整上下文并返回负数，调用方会拒绝不完整的通道连接。
 */
int shadow_client_rdpsnd_init(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	RdpsndServerContext* rdpsnd = client->rdpsnd = rdpsnd_server_context_new(client->vcm);

	if (!rdpsnd)
		return -1;

	rdpsnd->data = client;

	if (client->subsystem->rdpsndFormats)
	{
		const size_t count = client->subsystem->nRdpsndFormats;
		rdpsnd->server_formats = audio_formats_new(count);
		if (!rdpsnd->server_formats)
			goto fail;

		for (size_t index = 0; index < count; index++)
		{
			if (!audio_format_copy(&client->subsystem->rdpsndFormats[index],
			                       &rdpsnd->server_formats[index]))
				goto fail;
		}

		rdpsnd->num_server_formats = count;
	}
	else
	{
		rdpsnd->num_server_formats = server_rdpsnd_get_formats(&rdpsnd->server_formats);
	}

	if ((rdpsnd->num_server_formats < 1) || !rdpsnd->server_formats)
		goto fail;

	rdpsnd->src_format = &rdpsnd->server_formats[0];

	rdpsnd->Activated = rdpsnd_activated;

	const UINT error = rdpsnd->Initialize(rdpsnd, TRUE);
	if (error != CHANNEL_RC_OK)
		goto fail;
	return 1;

fail:
	shadow_client_rdpsnd_free_server_formats(client->rdpsnd);
	rdpsnd_server_context_free(client->rdpsnd);
	client->rdpsnd = nullptr;
	return -1;
}

/**
 * 停止并销毁一个 Shadow 客户端的 RDPSND 通道。
 *
 * 在客户端线程退出时调用；先停止通道再释放私有格式副本，确保异步声音消息不再访问格式数据。
 * 客户端没有建立声音通道时不执行操作，函数保持可重复调用。
 */
void shadow_client_rdpsnd_uninit(rdpShadowClient* client)
{
	WINPR_ASSERT(client);
	if (client->rdpsnd)
	{
		client->rdpsnd->Stop(client->rdpsnd);
		shadow_client_rdpsnd_free_server_formats(client->rdpsnd);
		rdpsnd_server_context_free(client->rdpsnd);
		client->rdpsnd = nullptr;
	}
}

/**
 * 复制并广播系统音频样本给所有 Shadow 客户端。
 *
 * WASAPI 缓冲区仅在 ReleaseBuffer 前有效，因此此函数在投递客户端消息前复制音频数据。格式
 * 与帧数不合法、分配失败或没有客户端时均返回 FALSE；所有成功投递的消息最终由引用计数回调
 * 释放，调用方无需也不得释放传入样本。
 */
BOOL shadow_client_broadcast_audio_samples(rdpShadowServer* server, const AUDIO_FORMAT* format,
                                           const void* samples, size_t frameCount, UINT16 timestamp)
{
	SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES* msg = nullptr;
	size_t byteCount = 0;

	if (!server || !format || !samples || (frameCount == 0) || (format->nBlockAlign == 0))
		return FALSE;

	if (frameCount > (SIZE_MAX / format->nBlockAlign))
		return FALSE;
	byteCount = frameCount * format->nBlockAlign;

	msg = (SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES*)calloc(1, sizeof(*msg));
	if (!msg)
		return FALSE;

	msg->buf = malloc(byteCount);
	if (!msg->buf)
	{
		free(msg);
		return FALSE;
	}

	memcpy(msg->buf, samples, byteCount);
	msg->audio_format = (AUDIO_FORMAT*)format;
	msg->nFrames = frameCount;
	msg->wTimestamp = timestamp;
	msg->common.Free = shadow_rdpsnd_free_audio_samples;

	return shadow_client_boardcast_msg(server, nullptr, SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES_ID,
	                                  (SHADOW_MSG_OUT*)msg, nullptr) > 0;
}
