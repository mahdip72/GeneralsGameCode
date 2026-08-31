#include "Common/AsciiString.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// This support file gives the device-free catalog executable the small,
// allocator-independent AsciiString surface it needs.  The product keeps the
// full implementation; the test deliberately avoids loading the legacy INI
// and memory-manager graph just to exercise the neutral catalog.
const AsciiString AsciiString::TheEmptyString;

#if defined(RTS_DEBUG)
void AsciiString::validate() const
{
	if (m_data == nullptr) {
		return;
	}
	if (m_data->m_refCount <= 0 || m_data->m_refCount >= 32000
		|| m_data->m_numCharsAllocated == 0
		|| std::strlen(m_data->peek()) + 1U > m_data->m_numCharsAllocated) {
		std::fprintf(stderr, "FAIL: invalid device-free AsciiString fixture\n");
		std::exit(EXIT_FAILURE);
	}
}
#endif

AsciiString::AsciiString(const AsciiString &source) : m_data(source.m_data)
{
	if (m_data != nullptr) {
		++m_data->m_refCount;
	}
}

AsciiString::AsciiString(const char *source) : m_data(nullptr)
{
	set(source);
}

AsciiString::AsciiString(const char *source, int length) : m_data(nullptr)
{
	set(source, length);
}

void AsciiString::releaseBuffer()
{
	if (m_data == nullptr) {
		return;
	}
	if (--m_data->m_refCount == 0) {
		std::free(m_data);
	}
	m_data = nullptr;
}

void AsciiString::ensureUniqueBufferOfSize(int, Bool, const char *, const char *)
{
}

void AsciiString::set(const AsciiString &source)
{
	if (m_data == source.m_data) {
		return;
	}
	releaseBuffer();
	m_data = source.m_data;
	if (m_data != nullptr) {
		++m_data->m_refCount;
	}
}

void AsciiString::set(const char *source)
{
	set(source == nullptr ? "" : source,
		static_cast<int>(std::strlen(source == nullptr ? "" : source)));
}

void AsciiString::set(const char *source, int length)
{
	if (source == nullptr) {
		source = "";
		length = 0;
	}
	if (length < 0) {
		length = 0;
	}
	releaseBuffer();
	const std::size_t bytes = sizeof(AsciiStringData) + static_cast<std::size_t>(length) + 1U;
	m_data = static_cast<AsciiStringData *>(std::malloc(bytes));
	if (m_data == nullptr) {
		std::fprintf(stderr, "FAIL: device-free AsciiString allocation failed\n");
		std::exit(EXIT_FAILURE);
	}
	m_data->m_refCount = 1;
	m_data->m_numCharsAllocated = static_cast<unsigned short>(length + 1);
	std::memcpy(m_data->peek(), source, static_cast<std::size_t>(length));
	m_data->peek()[length] = 0;
}
