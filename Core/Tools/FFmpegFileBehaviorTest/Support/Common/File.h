#pragma once

#include "Lib/BaseType.h"

class File
{
public:
	enum seekMode
	{
		START,
		CURRENT,
		END,
	};

	virtual ~File() = default;
	virtual Int read(void *buffer, Int bytes) = 0;
	virtual Int seek(Int offset, seekMode mode = CURRENT) = 0;
	virtual Int size() = 0;
	virtual void close() = 0;
};
