#include "W3DDevice/GameClient/LegacyTextureFormatSupport.h"

#include "WW3D2/dx8wrapper.h"

namespace rts
{
namespace render
{
	bool IsLegacyTextureFormatSupported(WW3DFormat format)
	{
		return DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(format);
	}
}
}
