/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

namespace rts
{
	namespace render
	{
		struct W3DShroudDestinationUpdateDecision
		{
			bool copySource;
			bool notifyTexture;
		};

		W3DShroudDestinationUpdateDecision EvaluateW3DShroudDestinationUpdate(
			bool sourceDirtyBeforeSync,
			bool sourceDirtyAfterInterpolation,
			bool destinationBorderDirty);
	}
}
