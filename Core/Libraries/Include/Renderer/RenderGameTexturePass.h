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
		bool useDefaultDepth) : m_open(false), m_ready(false), m_restored(false)
	{
		SetGameRenderTarget(color, depth, useDefaultDepth);
		if (!IsGameRenderingToTexture())
		{
			RestoreTarget();
			return;
		}
		if (IsNativeGameRendererActive())
		{
			const GameRenderColor unused = { 0.0f, 0.0f, 0.0f, 0.0f };
			if (BeginGameRender(false, false, unused, 0.0f) != RENDER_RESULT_OK)
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
	void RestoreTarget()
	{
		if (!m_restored)
		{
			SetGameRenderTarget(0, 0, true);
			m_restored = true;
		}
	}
	RenderResult Finish()
	{
		RestoreTarget();
		if (!m_open) return RENDER_RESULT_OK;
		m_open = false;
		return EndGameRender(false);
	}

private:
	GameTextureRenderPass(const GameTextureRenderPass &);
	GameTextureRenderPass &operator=(const GameTextureRenderPass &);
	bool m_open;
	bool m_ready;
	bool m_restored;
};

} }

#endif
