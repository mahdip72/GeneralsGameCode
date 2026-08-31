#include <xaudio2.h>

#include "XAudio2AudioDevice/XAudio2AudioService.h"

#if NTDDI_VERSION < NTDDI_WIN10_19H1
#error The native XAudio2 contract must use topology-aware processor selection.
#endif

int main()
{
	XAudio2AudioService service;
	service.shutdown();
	return service.isOpen() ? 1 : 0;
}
