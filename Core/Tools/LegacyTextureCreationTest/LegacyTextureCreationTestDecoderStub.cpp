#include "WWLib/TARGA.h"

extern int targaOpenCalls;

Targa::Targa()
{
}

Targa::~Targa()
{
}

long Targa::Open(const char *, long)
{
	++targaOpenCalls;
	return TGAERR_OPEN;
}

long Targa::Load(const char *, long, bool)
{
	return TGAERR_OPEN;
}

char *Targa::SetPalette(char *palette)
{
	return palette;
}
