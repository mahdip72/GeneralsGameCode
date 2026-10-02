#include "Utility/CppMacros.h"
#include <stdio.h>
#include <string.h>

// Compile each title's actual shader-loader definition unchanged. Only the
// external file/device/heap APIs are doubled; no game or D3D device is started.
namespace legacy_shader_asset_test
{
typedef long HRESULT;
typedef unsigned long DWORD;
typedef int Int;
const HRESULT S_OK = 0;
const HRESULT E_FAIL = static_cast<HRESULT>(0x80004005UL);
const HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057UL);
const HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000eUL);
const DWORD HEAP_ZERO_MEMORY = 8;

struct State
{
	bool openFails;
	bool shortRead;
	bool readThrows;
	bool deviceThrows;
	bool allocationFails;
	bool swapFactoryOnRead;
	int size;
	int directGameCalls;
	int opens;
	int closes;
	int reads;
	int allocationCalls;
	int outstandingAllocations;
	int deviceCalls;
	bool vertexCall;
	HRESULT deviceResult;
	const DWORD *declaration;
	DWORD usage;
	DWORD submittedWords[3];
	DWORD heapWords[16];
};
State state;

class FileClass
{
public:
	enum { READ = 1 };
	FileClass() : opened(false) {}
	int Open(int rights = READ)
	{
		++state.opens;
		opened = rights == READ && !state.openFails;
		return opened;
	}
	int Size() { return opened ? state.size : -1; }
	int Read(void *buffer, int size);
	void Close()
	{
		if (opened) ++state.closes;
		opened = false;
	}
	bool opened;
};

class FileFactoryClass
{
public:
	FileFactoryClass() : gets(0), returns(0), wrongReturn(false)
	{
		path[0] = 0;
	}
	FileClass *Get_File(const char *filename)
	{
		++gets;
		strncpy(path, filename, sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
		return &file;
	}
	void Return_File(FileClass *value)
	{
		++returns;
		wrongReturn = value != &file;
		value->Close();
	}
	FileClass file;
	int gets;
	int returns;
	bool wrongReturn;
	char path[64];
};
FileFactoryClass primaryFactory;
FileFactoryClass replacementFactory;
FileFactoryClass *_TheFileFactory = &primaryFactory;

// Match the existing WWLib guard's captured-provider ownership. The fixture
// factory always returns its own file, so its fallback allocation is unused.
class file_auto_ptr
{
public:
	file_auto_ptr(FileFactoryClass *factory, const char *path) :
		factory_(factory), file_(factory->Get_File(path)) {}
	~file_auto_ptr() { factory_->Return_File(file_); }
	FileClass *get() const { return file_; }
	FileClass *operator->() const { return file_; }
private:
	file_auto_ptr(const file_auto_ptr &);
	file_auto_ptr &operator=(const file_auto_ptr &);
	FileFactoryClass *factory_;
	FileClass *file_;
};

int FileClass::Read(void *buffer, int requested)
{
	++state.reads;
	if (state.swapFactoryOnRead) _TheFileFactory = &replacementFactory;
	if (state.readThrows) throw 1;
	const DWORD words[3] = { 0xfffe0101UL, 0x00000000UL, 0x0000ffffUL };
	if (!opened || requested != static_cast<int>(sizeof(words))) return 0;
	memcpy(buffer, words, sizeof(words));
	return state.shortRead ? requested - 1 : requested;
}

// Retain a test-only version of the obsolete dependency so the original
// production function compiles: RED must reveal provider bypass, not a typo.
class AsciiString
{
public:
	explicit AsciiString(const char *) {}
};
struct FileInfo { int sizeLow; };
class File
{
public:
	enum { READ = 1, BINARY = 2 };
	int read(void *buffer, int size) { return backing.Read(buffer, size); }
	void close() { backing.Close(); }
	FileClass backing;
};
class FileSystem
{
public:
	File *openFile(const char *, int)
	{
		++state.directGameCalls;
		return file.backing.Open() ? &file : 0;
	}
	bool getFileInfo(const AsciiString &, FileInfo *info)
	{
		++state.directGameCalls;
		info->sizeLow = state.size;
		return true;
	}
	File file;
};
FileSystem legacyGameFileSystem;
FileSystem *TheFileSystem = &legacyGameFileSystem;

void *GetProcessHeap() { return &state; }
void *HeapAlloc(void *, DWORD, unsigned long bytes)
{
	++state.allocationCalls;
	if (state.allocationFails || bytes > sizeof(state.heapWords)) return 0;
	++state.outstandingAllocations;
	memset(state.heapWords, 0, sizeof(state.heapWords));
	return state.heapWords;
}
int HeapFree(void *, DWORD, void *allocation)
{
	if (allocation != state.heapWords) return 0;
	--state.outstandingAllocations;
	return 1;
}

class Device
{
public:
	HRESULT Submit(const DWORD *words, DWORD *handle, bool vertex,
		const DWORD *declaration, DWORD usage)
	{
		++state.deviceCalls;
		state.vertexCall = vertex;
		state.declaration = declaration;
		state.usage = usage;
		memcpy(state.submittedWords, words, sizeof(state.submittedWords));
		if (state.deviceThrows) throw 2;
		if (state.deviceResult == S_OK) *handle = 73;
		return state.deviceResult;
	}
	HRESULT CreateVertexShader(const DWORD *declaration, const DWORD *words,
		DWORD *handle, DWORD usage)
	{
		return Submit(words, handle, true, declaration, usage);
	}
	HRESULT CreatePixelShader(const DWORD *words, DWORD *handle)
	{
		return Submit(words, handle, false, 0, 0);
	}
};
Device device;
Device *activeDevice = &device;
class DX8Wrapper
{
public:
	static Device *_Get_D3D_Device8() { return activeDevice; }
};

void Reset()
{
	memset(&state, 0, sizeof(state));
	state.size = 12;
	state.deviceResult = S_OK;
	primaryFactory = FileFactoryClass();
	replacementFactory = FileFactoryClass();
	legacyGameFileSystem.file.backing = FileClass();
	_TheFileFactory = &primaryFactory;
	TheFileSystem = &legacyGameFileSystem;
	activeDevice = &device;
}

int Check(bool condition, const char *title, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s legacy shader asset: %s\n", title, message);
	return 1;
}
}

namespace generals_shader_asset_test
{
using namespace legacy_shader_asset_test;
#include "GeneralsLegacyShaderAssetMethods.inc"
}
namespace zerohour_shader_asset_test
{
using namespace legacy_shader_asset_test;
#include "ZeroHourLegacyShaderAssetMethods.inc"
}

namespace legacy_shader_asset_test
{
typedef HRESULT (*CreateShader)(const char *, const DWORD *, DWORD, bool, DWORD *);

int RunTitle(CreateShader create, const char *title)
{
	int failures = 0;
	const DWORD declaration[2] = { 0x20000000UL, 0xffffffffUL };
	DWORD handle = 0;
	Reset();
	failures |= Check(create("shaders\\wave.vso", declaration, 5, true, &handle) == S_OK &&
		handle == 73 && state.deviceCalls == 1 && state.vertexCall &&
		state.declaration == declaration && state.usage == 5 &&
		state.submittedWords[0] == 0xfffe0101UL && state.submittedWords[1] == 0 &&
		state.submittedWords[2] == 0x0000ffffUL,
		title, "vertex bytes, declaration, usage and returned handle are unchanged");
	failures |= Check(primaryFactory.gets == 1 && primaryFactory.returns == 1 &&
		!primaryFactory.wrongReturn && strcmp(primaryFactory.path, "shaders\\wave.vso") == 0 &&
		state.directGameCalls == 0 && state.opens == 1 && state.closes == 1 &&
		state.outstandingAllocations == 0,
		title, "shader assets use the active WWLib provider without a GameEngine dependency");

	Reset();
	TheFileSystem = 0;
	handle = 0;
	failures |= Check(create("shaders\\wave.pso", 0, 0, false, &handle) == S_OK &&
		handle == 73 && state.deviceCalls == 1 && !state.vertexCall &&
		primaryFactory.returns == 1 && state.outstandingAllocations == 0,
		title, "authoring provider can load pixel shaders with no game filesystem");

	Reset();
	failures |= Check(create(0, declaration, 0, true, &handle) == E_INVALIDARG &&
		create("shader.vso", 0, 0, true, &handle) == E_INVALIDARG &&
		create("shader.pso", 0, 0, false, 0) == E_INVALIDARG &&
		primaryFactory.gets == 0 && state.directGameCalls == 0 && state.allocationCalls == 0,
		title, "invalid arguments fail before asset access");
	activeDevice = 0;
	failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_INVALIDARG &&
		primaryFactory.gets == 0 && state.directGameCalls == 0,
		title, "missing device fails before asset access");

	Reset();
	_TheFileFactory = 0;
	failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_FAIL &&
		state.directGameCalls == 0 && state.deviceCalls == 0 && state.allocationCalls == 0,
		title, "absent renderer provider cannot fall through to game globals");

	Reset();
	state.openFails = true;
	failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_FAIL &&
		primaryFactory.returns == 1 && state.closes == 0 && state.allocationCalls == 0,
		title, "failed open returns its provider-owned file without allocation");

	for (int sizeCase = 0; sizeCase < 2; ++sizeCase)
	{
		Reset();
		state.size = sizeCase == 0 ? 0 : -1;
		failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_FAIL &&
			primaryFactory.returns == 1 && state.closes == 1 && state.allocationCalls == 0,
			title, "empty or failed size is rejected before allocation");
	}

	Reset();
	state.shortRead = true;
	handle = 19;
	failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_FAIL &&
		handle == 19 && state.deviceCalls == 0 && primaryFactory.returns == 1 &&
		state.closes == 1 && state.outstandingAllocations == 0,
		title, "short read never creates or publishes a partial shader");

	Reset();
	state.allocationFails = true;
	failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_OUTOFMEMORY &&
		primaryFactory.returns == 1 && state.closes == 1 && state.deviceCalls == 0 &&
		state.outstandingAllocations == 0,
		title, "allocation failure closes and returns the opened file");

	Reset();
	state.deviceResult = E_INVALIDARG;
	handle = 19;
	failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_INVALIDARG &&
		handle == 19 && primaryFactory.returns == 1 && state.closes == 1 &&
		state.outstandingAllocations == 0,
		title, "device HRESULT is preserved and all temporary ownership is released");

	for (int throwCase = 0; throwCase < 2; ++throwCase)
	{
		Reset();
		state.readThrows = throwCase == 0;
		state.deviceThrows = throwCase == 1;
		handle = 19;
		failures |= Check(create("shader.pso", 0, 0, false, &handle) == E_FAIL &&
			handle == 19 && primaryFactory.returns == 1 && state.closes == 1 &&
			state.outstandingAllocations == 0,
			title, "read or device exception cannot leak shader memory or provider ownership");
	}

	Reset();
	state.swapFactoryOnRead = true;
	failures |= Check(create("shader.pso", 0, 0, false, &handle) == S_OK &&
		primaryFactory.returns == 1 && replacementFactory.returns == 0 &&
		!primaryFactory.wrongReturn && state.closes == 1 && state.outstandingAllocations == 0,
		title, "file returns through the captured provider even if the global provider changes");
	return failures;
}
}

int RunLegacyShaderAssetContractTests()
{
	using namespace legacy_shader_asset_test;
	return RunTitle(generals_shader_asset_test::CreateLegacyD3DShader, "Generals") |
		RunTitle(zerohour_shader_asset_test::CreateLegacyD3DShader, "ZeroHour");
}
