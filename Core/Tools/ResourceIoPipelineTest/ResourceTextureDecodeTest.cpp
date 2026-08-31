#include "WWLib/TARGA.h"
#include "WWLib/ffactory.h"
#include "WWLib/WWFILE.h"
#include "WW3D2/ddsfile.h"
#include "WW3D2/bitmaphandler.h"
#include "WW3D2/texturemipbuffer.h"
#include "WW3D2/formconv.h"
#include "WW3D2/legacytexturecompat.h"
#include "Lib/ResourceIoPipeline.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// The fixture exercises real CPU loaders, not a GPU/device singleton.
void Log_DX8_ErrorCode(unsigned) {}
void LegacyTextureCreation_Register_DDS_Decode_Callback(LegacyTextureDDSDecodeCallback) {}
unsigned Get_Bytes_Per_Pixel(WW3DFormat format)
{
	WW3DFormatDescriptor descriptor;
	return Try_Get_WW3DFormat_Descriptor(format, &descriptor) ? descriptor.bytesPerPixel : 0;
}

namespace
{
int failures = 0;
std::thread::id owner;
std::atomic<unsigned> factoryWorkerCalls(0);
void check(bool condition, const char *message)
{
	if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
class MemoryFile : public FileClass
{
public:
	MemoryFile(const std::vector<unsigned char> &bytes) : bytes(bytes), position(0), opened(false) {}
	const char *File_Name() const { return "fixture.dds"; }
	const char *Set_Name(const char *name) { return name; }
	int Create() { return 0; }
	int Delete() { return 0; }
	bool Is_Available(int = 0) { return true; }
	bool Is_Open() const { return opened; }
	int Open(const char *, int = READ) { return Open(READ); }
	int Open(int = READ) { opened = true; position = 0; return 1; }
	int Read(void *destination, int count)
	{
		if (!opened || count < 0 || static_cast<size_t>(count) > bytes.size() - position) return 0;
		std::memcpy(destination, bytes.data() + position, count); position += count; return count;
	}
	int Seek(int offset, int direction = SEEK_CUR)
	{
		const long long base = direction == SEEK_SET ? 0 : direction == SEEK_END ? bytes.size() : position;
		const long long next = base + offset;
		if (next < 0 || static_cast<size_t>(next) > bytes.size()) return -1;
		position = static_cast<size_t>(next); return static_cast<int>(position);
	}
	int Size() { return static_cast<int>(bytes.size()); }
	int Write(const void *, int) { return 0; }
	void Close() { opened = false; }
private:
	const std::vector<unsigned char> &bytes;
	size_t position;
	bool opened;
};
class MemoryFactory : public FileFactoryClass
{
public:
	FileClass *Get_File(const char *)
	{
		if (std::this_thread::get_id() != owner) ++factoryWorkerCalls;
		return new MemoryFile(bytes);
	}
	void Return_File(FileClass *file)
	{
		if (std::this_thread::get_id() != owner) ++factoryWorkerCalls;
		delete file;
	}
	std::vector<unsigned char> bytes;
};
class BytesSource : public rts::ResourceIoSource
{
public:
	explicit BytesSource(const std::vector<unsigned char> &source) : bytes(source) {}
	size_t size() const { return bytes.size(); }
	int read(size_t offset, void *out, unsigned count)
	{
		if (offset > bytes.size() || count > bytes.size() - offset) return -1;
		std::memcpy(out, bytes.data() + offset, count); return count;
	}
private:
	std::vector<unsigned char> bytes;
};

std::vector<unsigned char> makeDDS(WW3DFormat format, bool cube)
{
	LegacyDDSURFACEDESC2 header;
	std::memset(&header, 0, sizeof(header));
	header.Size = sizeof(header); header.Width = header.Height = 8; header.MipMapCount = 4;
	header.PixelFormat.Size = sizeof(header.PixelFormat);
	header.PixelFormat.FourCC = WW3DFormat_To_D3DFormat(format);
	header.Caps.Caps2 = cube ? 0x200 : 0;
	const unsigned blockBytes = format == WW3D_FORMAT_DXT1 ? 8 : 16;
	const unsigned faceBytes = 7 * blockBytes;
	std::vector<unsigned char> bytes(128 + faceBytes * (cube ? 6 : 1));
	std::memcpy(bytes.data(), "DDS ", 4);
	std::memcpy(bytes.data() + 4, &header, sizeof(header));
	for (size_t i = 128; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>((i * 17 + i / faceBytes) & 255);
	return bytes;
}

std::vector<unsigned char> copyDDSMips(DDSFileClass &dds, WW3DFormat destination, const Vector3 &hsv)
{
	std::vector<unsigned char> result;
	for (unsigned face = 0; face < (dds.Get_Type() == DDS_CUBEMAP ? 6U : 1U); ++face)
	{
		for (unsigned level = 0; level < dds.Get_Mip_Level_Count(); ++level)
		{
			TextureMipLayout layout;
			if (!CalculateTextureMipLayout(destination, dds.Get_Width(level), dds.Get_Height(level), 1, layout)) return result;
			std::vector<unsigned char> surface(layout.dataSize, 0xCC);
			if (dds.Get_Type() == DDS_CUBEMAP)
				dds.Copy_CubeMap_Level_To_Surface(face, level, destination, dds.Get_Width(level), dds.Get_Height(level), surface.data(), (unsigned)layout.rowPitch, hsv);
			else dds.Copy_Level_To_Surface(level, destination, dds.Get_Width(level), dds.Get_Height(level), surface.data(), (unsigned)layout.rowPitch, hsv);
			result.insert(result.end(), surface.begin(), surface.end());
		}
	}
	return result;
}
class DDSDecode : public rts::ResourceDecodeOperation
{
public:
	DDSDecode(unsigned reduction, WW3DFormat destination, const Vector3 &hsv)
		: dds(nullptr, reduction), destination(destination), hsv(hsv) {}
	bool prepare(const unsigned char *bytes, size_t size, size_t &workspace)
	{
		workspace = size * 16;
		return dds.Set_Memory_Header(bytes, size);
	}
	bool decode(const unsigned char *bytes, size_t size, const rts::ResourceCancellation &cancel)
	{
		if (cancel.isCancelled() || !dds.Load_From_Memory(bytes, size)) return false;
		output = copyDDSMips(dds, destination, hsv); return !output.empty();
	}
	DDSFileClass dds;
	WW3DFormat destination;
	Vector3 hsv;
	std::vector<unsigned char> output;
};

void testDDS(MemoryFactory &factory, rts::ResourceIoPipeline &pipeline)
{
	const WW3DFormat formats[] = {WW3D_FORMAT_DXT1, WW3D_FORMAT_DXT2, WW3D_FORMAT_DXT3, WW3D_FORMAT_DXT4, WW3D_FORMAT_DXT5};
	for (WW3DFormat format : formats)
	for (unsigned cube = 0; cube != 2; ++cube)
	for (unsigned reduction = 0; reduction != 2; ++reduction)
	for (unsigned conversion = 0; conversion != 2; ++conversion)
	{
		factory.bytes = makeDDS(format, cube != 0);
		DDSFileClass serial("fixture.dds", reduction);
		check(serial.Is_Available() && serial.Load(), "retail DDS fixture loads through reference factory");
		const WW3DFormat destination = conversion ? WW3D_FORMAT_A8R8G8B8 : format;
		// The legacy recolorer supports compressed DXT1/DXT5 and expanded colors.
		const Vector3 hsv = conversion || format == WW3D_FORMAT_DXT1 || format == WW3D_FORMAT_DXT5 ?
			Vector3(0.12f, 0.07f, -0.03f) : Vector3(0, 0, 0);
		const std::vector<unsigned char> expected = copyDDSMips(serial, destination, hsv);
		rts::ResourceIoTicket ticket;
		check(pipeline.submit(new BytesSource(factory.bytes), new DDSDecode(reduction, destination, hsv),
			rts::JOB_PRIORITY_STREAMING, ticket), "DDS CPU decode admitted");
		check(pipeline.wait(ticket), "DDS CPU decode completes");
		rts::ResourceDecodeOperation *operation = 0;
		rts::ResourceIoStatus status;
		check(pipeline.take(ticket, status, operation) && status == rts::RESOURCE_IO_SUCCEEDED, "DDS decode result transferred to owner");
		DDSDecode *decoded = static_cast<DDSDecode*>(operation);
		check(decoded && decoded->output == expected, "DDS 2D/cube reduced mip bytes and HSV match serial reference");
		delete operation;
	}
	std::vector<unsigned char> bytes = makeDDS(WW3D_FORMAT_DXT1, true);
	DDSFileClass malformed(nullptr, 0);
	check(!malformed.Set_Memory_Header(bytes.data(), 127), "truncated DDS header rejected");
	check(malformed.Set_Memory_Header(bytes.data(), bytes.size()), "valid DDS header accepted");
	check(!malformed.Load_From_Memory(bytes.data(), bytes.size() - 1), "truncated cube payload rejected");
	check(malformed.Load_From_Memory(bytes.data(), bytes.size()), "failed memory read is retryable");
}

std::vector<unsigned char> makeTGA(unsigned bpp, unsigned origin, bool encoded, bool palette)
{
	TGAHeader header;
	std::memset(&header, 0, sizeof(header));
	header.Width = 4; header.Height = 2; header.PixelDepth = static_cast<char>(bpp * 8);
	header.ImageType = static_cast<char>((palette ? 1 : bpp == 1 ? 3 : 2) + (encoded ? 8 : 0));
	header.ImageDescriptor = static_cast<char>(origin);
	header.ColorMapType = palette ? 1 : 0; header.CMapLength = palette ? 8 : 0; header.CMapDepth = palette ? 24 : 0;
	std::vector<unsigned char> bytes(sizeof(header));
	std::memcpy(bytes.data(), &header, sizeof(header));
	if (palette) for (unsigned i = 0; i < 24; ++i) bytes.push_back(static_cast<unsigned char>(i * 9));
	if (encoded) bytes.push_back(7); // One raw RLE packet containing all eight pixels.
	for (unsigned i = 0; i < 8 * bpp; ++i) bytes.push_back(static_cast<unsigned char>(palette ? i : i * 7));
	bytes.resize(bytes.size() + 26, 0); // Reference Targa::Open reads a footer even for TGA1.
	return bytes;
}
class TGADecode : public rts::ResourceDecodeOperation
{
public:
	bool prepare(const unsigned char *, size_t size, size_t &workspace) { workspace = size * 16 + 1024; return true; }
	bool decode(const unsigned char *bytes, size_t size, const rts::ResourceCancellation &cancel)
	{
		return !cancel.isCancelled() && tga.Load_From_Memory(bytes, size, true) == 0;
	}
	Targa tga;
};
void testTGA(MemoryFactory &factory, rts::ResourceIoPipeline &pipeline)
{
	for (unsigned bpp = 1; bpp <= 4; ++bpp)
	for (unsigned origin = 0; origin < 4; ++origin)
	for (unsigned encoded = 0; encoded < 2; ++encoded)
	{
		// Legacy loader does not define monochrome RLE, but supports palette RLE.
		const bool palette = bpp == 1 && encoded != 0;
		factory.bytes = makeTGA(bpp, origin << 4, encoded != 0, palette);
		Targa serial;
		char paletteBytes[1024] = {0};
		check(serial.Open("fixture.tga", TGA_READMODE) == 0, "reference TGA opens");
		serial.Header.ImageDescriptor ^= TGAIDF_YORIGIN;
		serial.SetPalette(paletteBytes);
		check(serial.Load("fixture.tga", TGAF_IMAGE, false) == 0, "reference TGA loads");
		rts::ResourceIoTicket ticket;
		check(pipeline.submit(new BytesSource(factory.bytes), new TGADecode,
			rts::JOB_PRIORITY_STREAMING, ticket), "TGA decode admitted");
		check(pipeline.wait(ticket), "TGA decode completes");
		rts::ResourceDecodeOperation *operation = 0;
		rts::ResourceIoStatus status;
		check(pipeline.take(ticket, status, operation) && status == rts::RESOURCE_IO_SUCCEEDED, "TGA decode succeeds");
		TGADecode *decoded = static_cast<TGADecode*>(operation);
		check(decoded && decoded->tga.GetImage() && serial.GetImage() &&
			std::memcmp(decoded->tga.GetImage(), serial.GetImage(), 8 * bpp) == 0,
			"TGA RGB/alpha/16-bit/palette/luminance and XY origin match serial bytes");
		if (palette && decoded) check(std::memcmp(decoded->tga.GetPalette(), paletteBytes, 24) == 0, "palette ordering preserved");
		delete operation;
	}
	std::vector<unsigned char> broken = makeTGA(4, 0, true, false);
	broken[18] = 0xff; // 128-pixel run into an eight-pixel destination.
	Targa invalidRun;
	check(invalidRun.Load_From_Memory(broken.data(), broken.size(), true) != 0, "RLE run overrun rejected");
	Targa truncated;
	check(truncated.Load_From_Memory(broken.data(), 17, true) != 0, "short TGA header rejected");
}
}

int main()
{
	owner = std::this_thread::get_id();
	MemoryFactory factory;
	FileFactoryClass *saved = _TheFileFactory; _TheFileFactory = &factory;
	rts::JobSystemConfig jobs = rts::JobSystem::startupConfig(); jobs.workerCount = 4;
	check(rts::JobSystem::instance().start(jobs), "texture decode worker pool starts");
	rts::ResourceIoPipeline pipeline;
	check(pipeline.start(rts::ResourceIoConfig(), rts::JobSystem::instance().createGroup()), "texture decode IO starts");
	testDDS(factory, pipeline);
	testTGA(factory, pipeline);
	pipeline.shutdown();
	rts::JobSystem::instance().shutdown();
	_TheFileFactory = saved;
	check(factoryWorkerCalls.load() == 0, "memory decoders never use global factories from workers");
	std::printf("resource texture decode: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
