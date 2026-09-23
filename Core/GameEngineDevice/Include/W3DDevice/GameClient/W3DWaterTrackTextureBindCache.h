#pragma once

// One instance belongs to one WaterTracksRenderSystem::flush call. Compare
// the actual stage-zero texture object because modules of one wave type can
// be initialized with different textures.
class W3DWaterTrackTextureBindCache
{
public:
	W3DWaterTrackTextureBindCache()
		: m_hasLastTexture(false), m_lastTexture(nullptr) {}

	bool ShouldBind(const void *texture)
	{
		if (m_hasLastTexture && m_lastTexture == texture)
			return false;
		m_hasLastTexture = true;
		m_lastTexture = texture;
		return true;
	}

private:
	bool m_hasLastTexture;
	const void *m_lastTexture;
};
