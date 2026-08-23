#include <xaudio2.h>

#if NTDDI_VERSION < NTDDI_WIN10_19H1
#error The native XAudio2 contract must use topology-aware processor selection.
#endif

int main()
{
	return XAUDIO2_DEFAULT_CHANNELS == 0 ? 0 : 1;
}
