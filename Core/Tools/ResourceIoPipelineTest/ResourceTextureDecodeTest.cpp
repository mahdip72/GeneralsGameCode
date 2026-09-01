#include "WWLib/TARGA.h"
#include "WWLib/ffactory.h"
#include "WWLib/WWFILE.h"
#include "WW3D2/ddsfile.h"
#include "WW3D2/texturemipbuffer.h"
#include "Lib/ResourceIoPipeline.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

unsigned Get_Bytes_Per_Pixel(WW3DFormat format)
{
	switch (format)
	{
	case WW3D_FORMAT_R8G8B8: return 3;
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8: return 4;
	case WW3D_FORMAT_R5G6B5:
	case WW3D_FORMAT_X1R5G5B5:
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_A8R3G3B2:
	case WW3D_FORMAT_X4R4G4B4:
	case WW3D_FORMAT_A8P8:
	case WW3D_FORMAT_A8L8:
	case WW3D_FORMAT_U8V8:
	case WW3D_FORMAT_L6V5U5: return 2;
	case WW3D_FORMAT_R3G3B2:
	case WW3D_FORMAT_A8:
	case WW3D_FORMAT_P8:
	case WW3D_FORMAT_L8:
	case WW3D_FORMAT_A4L4: return 1;
	default: return 0;
	}
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

unsigned ddsFourCC(WW3DFormat format)
{
	switch (format)
	{
	case WW3D_FORMAT_DXT1: return 0x31545844;
	case WW3D_FORMAT_DXT2: return 0x32545844;
	case WW3D_FORMAT_DXT3: return 0x33545844;
	case WW3D_FORMAT_DXT4: return 0x34545844;
	case WW3D_FORMAT_DXT5: return 0x35545844;
	default: return 0;
	}
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

std::vector<unsigned char> makeDDS(WW3DFormat format, bool cube,
	unsigned width = 8, unsigned height = 8, unsigned mipCount = 4)
{
	LegacyDDSURFACEDESC2 header;
	std::memset(&header, 0, sizeof(header));
	header.Size = sizeof(header); header.Width = width; header.Height = height;
	header.MipMapCount = mipCount;
	header.PixelFormat.Size = sizeof(header.PixelFormat);
	header.PixelFormat.FourCC = ddsFourCC(format);
	header.Caps.Caps2 = cube ? 0x200 : 0;
	const unsigned blockBytes = format == WW3D_FORMAT_DXT1 ? 8 : 16;
	unsigned faceBytes = 0;
	unsigned levelWidth = width, levelHeight = height;
	for (unsigned level = 0; level != mipCount; ++level)
	{
		faceBytes += ((levelWidth + 3) / 4) * ((levelHeight + 3) / 4) * blockBytes;
		levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
		levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
	}
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
	check(sizeof(LegacyDDSURFACEDESC2) == 124,
		"DDS disk descriptor remains exactly 124 bytes");
	check(offsetof(LegacyDDSURFACEDESC2, Surface) == 36,
		"DDS Surface disk field remains at byte offset 36");
	check(sizeof(((LegacyDDSURFACEDESC2*)0)->Surface) == 4,
		"DDS Surface disk field remains exactly 32 bits");
	LegacyDDSURFACEDESC2 layoutHeader;
	std::memset(&layoutHeader, 0, sizeof(layoutHeader));
	layoutHeader.Surface = 0x78563412U;
	unsigned char layoutBytes[sizeof(layoutHeader)];
	std::memcpy(layoutBytes, &layoutHeader, sizeof(layoutHeader));
	check(layoutBytes[36] == 0x12 && layoutBytes[37] == 0x34 &&
		layoutBytes[38] == 0x56 && layoutBytes[39] == 0x78,
		"DDS Surface value retains its byte-exact little-endian disk layout");

	std::vector<unsigned char> twoDBytes = makeDDS(WW3D_FORMAT_DXT1, false);
	// Make the retained 4x4 mip alternate pure red and green.  Its R3G3B2
	// expansion must pack those channels into exactly one byte per pixel.
	const unsigned char r3g3b2Block[8] = {0x00, 0xF8, 0xE0, 0x07,
		0x44, 0x44, 0x44, 0x44};
	std::memcpy(twoDBytes.data() + 160, r3g3b2Block, sizeof(r3g3b2Block));
	DDSFileClass twoD(nullptr, 0);
	check(twoD.Set_Memory_Header(twoDBytes.data(), twoDBytes.size()) &&
		twoD.Load_From_Memory(twoDBytes.data(), twoDBytes.size()),
		"byte-exact 2D DDS fixture loads from memory");
	check(twoD.Get_Mip_Level_Count() == 2 &&
		twoD.Get_Level_Size(0) == 32 && twoD.Get_Level_Size(1) == 8,
		"2D DDS retained mip sizes match the disk block layout");
	check(std::memcmp(twoD.Get_Memory_Pointer(0), twoDBytes.data() + 128, 32) == 0 &&
		std::memcmp(twoD.Get_Memory_Pointer(1), twoDBytes.data() + 160, 8) == 0,
		"2D DDS mip pointers select the exact payload bytes");
	std::vector<unsigned char> pitched2D(40, 0xCC);
	twoD.Copy_Level_To_Surface(0, WW3D_FORMAT_DXT1, 8, 8,
		pitched2D.data(), 20, Vector3(0, 0, 0));
	check(std::memcmp(pitched2D.data(), twoDBytes.data() + 128, 16) == 0 &&
		std::memcmp(pitched2D.data() + 20, twoDBytes.data() + 144, 16) == 0,
		"2D DDS copy preserves exact block bytes across pitched rows");
	bool twoDPaddingUntouched = true;
	for (unsigned row = 0; row != 2; ++row)
		for (unsigned byte = 16; byte != 20; ++byte)
			twoDPaddingUntouched = twoDPaddingUntouched && pitched2D[row * 20 + byte] == 0xCC;
	check(twoDPaddingUntouched,
		"2D DDS copy leaves destination pitch padding untouched");

	unsigned char tightR3G3B2[18];
	std::memset(tightR3G3B2, 0xCC, sizeof(tightR3G3B2));
	twoD.Copy_Level_To_Surface(1, WW3D_FORMAT_R3G3B2, 4, 4,
		tightR3G3B2 + 1, 4, Vector3(0, 0, 0));
	bool tightR3G3B2Exact = true;
	const unsigned char expectedR3G3B2Row[4] = {0xE0, 0x1C, 0xE0, 0x1C};
	for (unsigned row = 0; row != 4; ++row)
		tightR3G3B2Exact = tightR3G3B2Exact &&
			std::memcmp(tightR3G3B2 + 1 + row * 4,
				expectedR3G3B2Row, sizeof(expectedR3G3B2Row)) == 0;
	check(tightR3G3B2Exact,
		"tight-pitch R3G3B2 conversion writes the exact sixteen bytes");
	check(tightR3G3B2[0] == 0xCC && tightR3G3B2[17] == 0xCC,
		"tight-pitch R3G3B2 conversion preserves both canaries");

	unsigned char paddedR3G3B2[29];
	std::memset(paddedR3G3B2, 0xCC, sizeof(paddedR3G3B2));
	twoD.Copy_Level_To_Surface(1, WW3D_FORMAT_R3G3B2, 4, 4,
		paddedR3G3B2, 7, Vector3(0, 0, 0));
	bool paddedR3G3B2Exact = true;
	for (unsigned row = 0; row != 4; ++row)
	{
		paddedR3G3B2Exact = paddedR3G3B2Exact &&
			std::memcmp(paddedR3G3B2 + row * 7,
				expectedR3G3B2Row, sizeof(expectedR3G3B2Row)) == 0;
		for (unsigned byte = 4; byte != 7; ++byte)
			paddedR3G3B2Exact = paddedR3G3B2Exact &&
				paddedR3G3B2[row * 7 + byte] == 0xCC;
	}
	check(paddedR3G3B2Exact && paddedR3G3B2[28] == 0xCC,
		"R3G3B2 conversion writes exact active bytes and preserves row padding");

	const unsigned char edgeBlock[8] = {0x00, 0xF8, 0xE0, 0x07,
		0x44, 0x44, 0x44, 0x44};
	std::vector<unsigned char> edge2DBytes = makeDDS(
		WW3D_FORMAT_DXT1, false, 5, 5, 1);
	for (unsigned block = 0; block != 4; ++block)
		std::memcpy(edge2DBytes.data() + 128 + block * 8,
			edgeBlock, sizeof(edgeBlock));
	DDSFileClass edge2D(nullptr, 0);
	check(edge2D.Set_Memory_Header(edge2DBytes.data(), edge2DBytes.size()) &&
		edge2D.Load_From_Memory(edge2DBytes.data(), edge2DBytes.size()),
		"5x5 2D DDS fixture loads from memory");
	const unsigned edgeGuardSize = 96;
	const unsigned edge2DPitch = 20;
	std::vector<unsigned char> edge2DSurface(
		edgeGuardSize + edge2DPitch * 5 + edgeGuardSize, 0xCC);
	edge2D.Copy_Level_To_Surface(0, WW3D_FORMAT_A8R8G8B8, 5, 5,
		edge2DSurface.data() + edgeGuardSize, edge2DPitch, Vector3(0, 0, 0));
	// Preserve the legacy RGB565 expansion exactly (high bits are shifted,
	// not replicated into the low bits).
	const unsigned char redPixel[4] = {0x00, 0x00, 0xF8, 0xFF};
	const unsigned char greenPixel[4] = {0x00, 0xFC, 0x00, 0xFF};
	bool edge2DExact = true;
	for (unsigned row = 0; row != 5; ++row)
		for (unsigned column = 0; column != 5; ++column)
			edge2DExact = edge2DExact && std::memcmp(
				edge2DSurface.data() + edgeGuardSize + row * edge2DPitch + column * 4,
				(column & 1) ? greenPixel : redPixel, 4) == 0;
	bool edge2DCanaries = true;
	for (unsigned byte = 0; byte != edgeGuardSize; ++byte)
	{
		edge2DCanaries = edge2DCanaries && edge2DSurface[byte] == 0xCC;
		edge2DCanaries = edge2DCanaries &&
			edge2DSurface[edgeGuardSize + edge2DPitch * 5 + byte] == 0xCC;
	}
	check(edge2DExact,
		"5x5 2D DDS conversion clips edge blocks with exact active pixels");
	check(edge2DCanaries,
		"5x5 2D DDS conversion preserves allocation canaries");

	std::vector<unsigned char> edgeCubeBytes = makeDDS(
		WW3D_FORMAT_DXT1, true, 5, 5, 1);
	const unsigned edgeCubeFace = 3;
	const unsigned edgeCubeFaceBytes = 32;
	for (unsigned block = 0; block != 4; ++block)
		std::memcpy(edgeCubeBytes.data() + 128 +
			edgeCubeFace * edgeCubeFaceBytes + block * 8,
			edgeBlock, sizeof(edgeBlock));
	DDSFileClass edgeCube(nullptr, 0);
	check(edgeCube.Set_Memory_Header(edgeCubeBytes.data(), edgeCubeBytes.size()) &&
		edgeCube.Load_From_Memory(edgeCubeBytes.data(), edgeCubeBytes.size()),
		"5x5 cube DDS fixture loads from memory");
	const unsigned edgeCubePitch = 23;
	std::vector<unsigned char> edgeCubeSurface(
		edgeGuardSize + edgeCubePitch * 5 + edgeGuardSize, 0xCC);
	edgeCube.Copy_CubeMap_Level_To_Surface(edgeCubeFace, 0,
		WW3D_FORMAT_A8R8G8B8, 5, 5, edgeCubeSurface.data() + edgeGuardSize,
		edgeCubePitch, Vector3(0, 0, 0));
	bool edgeCubeExact = true;
	for (unsigned row = 0; row != 5; ++row)
	{
		edgeCubeExact = edgeCubeExact && std::memcmp(
			edgeCubeSurface.data() + edgeGuardSize + row * edgeCubePitch,
			edge2DSurface.data() + edgeGuardSize + row * edge2DPitch,
			edge2DPitch) == 0;
		for (unsigned byte = edge2DPitch; byte != edgeCubePitch; ++byte)
			edgeCubeExact = edgeCubeExact &&
				edgeCubeSurface[edgeGuardSize + row * edgeCubePitch + byte] == 0xCC;
	}
	bool edgeCubeCanaries = true;
	for (unsigned byte = 0; byte != edgeGuardSize; ++byte)
	{
		edgeCubeCanaries = edgeCubeCanaries && edgeCubeSurface[byte] == 0xCC;
		edgeCubeCanaries = edgeCubeCanaries &&
			edgeCubeSurface[edgeGuardSize + edgeCubePitch * 5 + byte] == 0xCC;
	}
	check(edgeCubeExact,
		"5x5 cube DDS conversion clips edge blocks and preserves pitch padding");
	check(edgeCubeCanaries,
		"5x5 cube DDS conversion preserves allocation canaries");

	std::vector<unsigned char> cubeBytes = makeDDS(WW3D_FORMAT_DXT1, true);
	DDSFileClass cubeDDS(nullptr, 0);
	check(cubeDDS.Set_Memory_Header(cubeBytes.data(), cubeBytes.size()) &&
		cubeDDS.Load_From_Memory(cubeBytes.data(), cubeBytes.size()),
		"byte-exact cube DDS fixture loads from memory");
	const unsigned face = 4;
	const unsigned faceBytes = 56;
	check(std::memcmp(cubeDDS.Get_CubeMap_Memory_Pointer(face, 0),
		cubeBytes.data() + 128 + face * faceBytes, 32) == 0 &&
		std::memcmp(cubeDDS.Get_CubeMap_Memory_Pointer(face, 1),
		cubeBytes.data() + 128 + face * faceBytes + 32, 8) == 0,
		"cube DDS face and mip pointers select exact face-major bytes");
	std::vector<unsigned char> pitchedCube(12, 0xCC);
	cubeDDS.Copy_CubeMap_Level_To_Surface(face, 1, WW3D_FORMAT_DXT1,
		4, 4, pitchedCube.data(), 12, Vector3(0, 0, 0));
	check(std::memcmp(pitchedCube.data(),
		cubeBytes.data() + 128 + face * faceBytes + 32, 8) == 0,
		"cube DDS mip copy preserves the exact selected face bytes");
	check(pitchedCube[8] == 0xCC && pitchedCube[9] == 0xCC &&
		pitchedCube[10] == 0xCC && pitchedCube[11] == 0xCC,
		"cube DDS mip copy leaves destination pitch padding untouched");

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
