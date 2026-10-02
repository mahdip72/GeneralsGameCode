#ifndef RTS_RENDER_GAME_TEXTURE_PASS_H
#define RTS_RENDER_GAME_TEXTURE_PASS_H

#include "Renderer/RenderGameClient.h"

namespace rts { namespace render {

// Water updates its texture before the visible frame. Native recording needs
// a balanced hidden frame here; the legacy renderer keeps its existing scene
// lifetime. Target restoration also runs on an early clear/render failure.
class GameTextureRenderPass
{
public:
	GameTextureRenderPass(TextureClass *color, ZTextureClass *depth,
		bool useDefaultDepth) : m_open(false), m_ready(false), m_restored(false),
		m_result(RENDER_RESULT_OK)
	{
	#if defined(_WIN64)
		Fail(SetGameRenderTargetChecked(color, depth, useDefaultDepth));
		if (m_result != RENDER_RESULT_OK)
		{
			RestoreTarget();
			return;
		}
	#else
		SetGameRenderTarget(color, depth, useDefaultDepth);
	#endif
		if (!IsGameRenderingToTexture())
		{
			if (IsNativeGameRendererActive()) Fail(RENDER_RESULT_FAILED);
			RestoreTarget();
			return;
		}
		if (IsNativeGameRendererActive())
		{
			const GameRenderColor unused = { 0.0f, 0.0f, 0.0f, 0.0f };
			Fail(BeginGameRender(false, false, unused, 0.0f));
			if (m_result != RENDER_RESULT_OK)
			{
				RestoreTarget();
				return;
			}
			m_open = true;
		}
		m_ready = true;
	}

	~GameTextureRenderPass() { Finish(); }
	bool IsReady() const { return m_ready; }
	void Fail(RenderResult result)
	{
		if (m_result == RENDER_RESULT_OK)
			m_result = result;
	}
	RenderResult RestoreTarget()
	{
		if (!m_restored)
		{
		#if defined(_WIN64)
			if (m_open)
				Fail(FlushGameSortedTriangles());
			const RenderResult restored = SetGameRenderTargetChecked(0, 0, true);
			Fail(restored);
			m_restored = restored == RENDER_RESULT_OK;
		#else
			SetGameRenderTarget(0, 0, true);
			m_restored = true;
		#endif
		}
		return m_result;
	}
	RenderResult Finish()
	{
		RestoreTarget();
		if (!m_open) return m_result;
		m_open = false;
	#if defined(_WIN64)
		Fail(EndGameTextureRenderPass(m_result));
	#else
		Fail(EndGameRender(false));
	#endif
		return m_result;
	}

private:
	GameTextureRenderPass(const GameTextureRenderPass &);
	GameTextureRenderPass &operator=(const GameTextureRenderPass &);
	bool m_open;
	bool m_ready;
	bool m_restored;
	RenderResult m_result;
};

} }

#endif
