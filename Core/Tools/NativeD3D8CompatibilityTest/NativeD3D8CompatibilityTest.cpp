#include <windows.h>
#include <d3d8.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
typedef IDirect3D8 *(WINAPI *Direct3DCreate8Function)(UINT);

int Fail(const char *message, HRESULT result = E_FAIL)
{
	std::fprintf(stderr, "%s (0x%08lx)\n", message,
		static_cast<unsigned long>(result));
	return 1;
}

LRESULT CALLBACK TestWindowProcedure(HWND window, UINT message, WPARAM wparam,
	LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

HMODULE LoadExactRuntime(const wchar_t *path)
{
	return LoadLibraryExW(path, nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

bool EqualMatrix(const D3DMATRIX &left, const D3DMATRIX &right)
{
	return std::memcmp(&left, &right, sizeof(left)) == 0;
}

const wchar_t kWindowClassName[] = L"GeneralsNativeD3D8CompatibilityTest";

bool GetCanonicalPath(const wchar_t *path, std::wstring *canonical_path)
{
	if (path == nullptr || canonical_path == nullptr)
		return false;

	wchar_t buffer[32768];
	const DWORD length = GetFullPathNameW(path,
		static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])), buffer, nullptr);
	if (length == 0 || length >= sizeof(buffer) / sizeof(buffer[0]))
		return false;
	canonical_path->assign(buffer, length);
	return true;
}

bool LoadedModuleMatches(HMODULE module, const wchar_t *expected_path)
{
	if (module == nullptr)
		return false;

	wchar_t actual_path[32768];
	const DWORD actual_length = GetModuleFileNameW(module, actual_path,
		static_cast<DWORD>(sizeof(actual_path) / sizeof(actual_path[0])));
	if (actual_length == 0 || actual_length >= sizeof(actual_path) / sizeof(actual_path[0]))
		return false;

	std::wstring canonical_actual_path;
	std::wstring canonical_expected_path;
	if (!GetCanonicalPath(actual_path, &canonical_actual_path) ||
		!GetCanonicalPath(expected_path, &canonical_expected_path))
		return false;

	return CompareStringOrdinal(canonical_actual_path.c_str(), -1,
		canonical_expected_path.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool IsX64PeFile(const wchar_t *path)
{
	if (path == nullptr || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ||
		(GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return false;

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
		FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	IMAGE_DOS_HEADER dos_header = {};
	DWORD bytes_read = 0;
	bool valid = ReadFile(file, &dos_header, sizeof(dos_header), &bytes_read, nullptr) != FALSE &&
		bytes_read == sizeof(dos_header) && dos_header.e_magic == IMAGE_DOS_SIGNATURE &&
		dos_header.e_lfanew >= 0;
	if (valid)
	{
		LARGE_INTEGER offset = {};
		offset.QuadPart = dos_header.e_lfanew;
		valid = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) != FALSE;
	}

	DWORD signature = 0;
	IMAGE_FILE_HEADER file_header = {};
	if (valid)
		valid = ReadFile(file, &signature, sizeof(signature), &bytes_read, nullptr) != FALSE &&
			bytes_read == sizeof(signature) && signature == IMAGE_NT_SIGNATURE;
	if (valid)
		valid = ReadFile(file, &file_header, sizeof(file_header), &bytes_read, nullptr) != FALSE &&
			bytes_read == sizeof(file_header) && file_header.Machine == IMAGE_FILE_MACHINE_AMD64;

	CloseHandle(file);
	return valid;
}

bool StagedFileMatchesModuleDirectory(const wchar_t *path, HMODULE module)
{
	if (path == nullptr || module == nullptr || !IsX64PeFile(path))
		return false;

	wchar_t module_path[32768];
	const DWORD module_length = GetModuleFileNameW(module, module_path,
		static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0])));
	if (module_length == 0 || module_length >= sizeof(module_path) / sizeof(module_path[0]))
		return false;

	std::wstring canonical_file_path;
	std::wstring canonical_module_path;
	if (!GetCanonicalPath(path, &canonical_file_path) ||
		!GetCanonicalPath(module_path, &canonical_module_path))
		return false;

	const std::wstring::size_type file_separator = canonical_file_path.find_last_of(L"\\/");
	const std::wstring::size_type module_separator = canonical_module_path.find_last_of(L"\\/");
	if (file_separator == std::wstring::npos || module_separator == std::wstring::npos)
		return false;

	return CompareStringOrdinal(canonical_file_path.c_str(),
		static_cast<int>(file_separator), canonical_module_path.c_str(),
		static_cast<int>(module_separator), TRUE) == CSTR_EQUAL;
}

struct TestResources
{
	HMODULE compatibility = nullptr;
	HINSTANCE instance = nullptr;
	ATOM window_class = 0;
	HWND window = nullptr;
	IDirect3D8 *d3d = nullptr;
	IDirect3DDevice8 *device = nullptr;
	IDirect3DTexture8 *texture = nullptr;
	IDirect3DVertexBuffer8 *vertex_buffer = nullptr;
	IDirect3DIndexBuffer8 *index_buffer = nullptr;
	IDirect3DSurface8 *depth_surface = nullptr;
	IDirect3DSurface8 *render_target = nullptr;
	IDirect3DTexture8 *post_reset_texture = nullptr;

	~TestResources()
	{
		if (post_reset_texture != nullptr) post_reset_texture->Release();
		if (render_target != nullptr) render_target->Release();
		if (depth_surface != nullptr) depth_surface->Release();
		if (index_buffer != nullptr) index_buffer->Release();
		if (vertex_buffer != nullptr) vertex_buffer->Release();
		if (texture != nullptr) texture->Release();
		if (device != nullptr) device->Release();
		if (d3d != nullptr) d3d->Release();
		if (window != nullptr) DestroyWindow(window);
		if (window_class != 0) UnregisterClassW(kWindowClassName, instance);
		if (compatibility != nullptr) FreeLibrary(compatibility);
	}
};
}

int wmain(int argc, wchar_t **argv)
{
	if (argc != 4)
		return Fail("Expected app-local d3d8.dll, staged D3DCompiler_43.dll, and D3DX9_43.dll paths");

	TestResources resources;
	HMODULE compatibility = LoadExactRuntime(argv[1]);
	if (compatibility == nullptr)
		return Fail("Failed to load app-local d3d8.dll",
			HRESULT_FROM_WIN32(GetLastError()));
	resources.compatibility = compatibility;
	if (!LoadedModuleMatches(resources.compatibility, argv[1]))
		return Fail("Loaded d3d8.dll did not resolve to the exact app-local path");
	if (!StagedFileMatchesModuleDirectory(argv[2], resources.compatibility))
		return Fail("Staged D3DCompiler_43.dll is not an x64 PE beside the loaded d3d8.dll");

	Direct3DCreate8Function create_d3d8 =
		reinterpret_cast<Direct3DCreate8Function>(GetProcAddress(compatibility,
			"Direct3DCreate8"));
	if (create_d3d8 == nullptr)
		return Fail("App-local d3d8.dll does not export Direct3DCreate8");

	IDirect3D8 *d3d = create_d3d8(D3D_SDK_VERSION);
	resources.d3d = d3d;
	if (d3d == nullptr || d3d->GetAdapterCount() == 0)
		return Fail("Direct3DCreate8 did not expose an adapter");
	if (!LoadedModuleMatches(GetModuleHandleW(L"d3dx9_43.dll"), argv[3]))
		return Fail("Compatibility bridge did not resolve its exact app-local D3DX9 runtime");

	D3DDISPLAYMODE display_mode = {};
	HRESULT result = d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display_mode);
	if (FAILED(result))
		return Fail("Failed to query the default adapter display mode", result);

	WNDCLASSW window_class = {};
	window_class.lpfnWndProc = TestWindowProcedure;
	window_class.hInstance = GetModuleHandleW(nullptr);
	window_class.lpszClassName = kWindowClassName;
	resources.instance = window_class.hInstance;
	resources.window_class = RegisterClassW(&window_class);
	if (resources.window_class == 0 &&
		GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		return Fail("Failed to register the hidden compatibility-test window",
			HRESULT_FROM_WIN32(GetLastError()));
	}

	HWND window = CreateWindowExW(0, kWindowClassName, kWindowClassName, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr,
		window_class.hInstance, nullptr);
	if (window == nullptr)
		return Fail("Failed to create the hidden compatibility-test window",
			HRESULT_FROM_WIN32(GetLastError()));
	resources.window = window;

	D3DPRESENT_PARAMETERS presentation = {};
	presentation.BackBufferWidth = 64;
	presentation.BackBufferHeight = 64;
	presentation.BackBufferFormat = display_mode.Format;
	presentation.BackBufferCount = 1;
	presentation.MultiSampleType = D3DMULTISAMPLE_NONE;
	presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
	presentation.hDeviceWindow = window;
	presentation.Windowed = TRUE;
	presentation.EnableAutoDepthStencil = TRUE;
	presentation.AutoDepthStencilFormat = D3DFMT_D16;
	presentation.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	IDirect3DDevice8 *device = nullptr;
	IDirect3DDevice8 *multithreaded_device = nullptr;
	result = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
		D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE |
			D3DCREATE_MULTITHREADED, &presentation, &multithreaded_device);
	if (result != D3DERR_INVALIDCALL || multithreaded_device != nullptr) {
		if (multithreaded_device != nullptr) multithreaded_device->Release();
		return Fail("Compatibility bridge did not reject unsupported multithreaded device ownership", result);
	}
	result = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
		D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
		&presentation, &device);
	if (FAILED(result) || device == nullptr)
		return Fail("Failed to create the x64 D3D8 compatibility device", result);
	resources.device = device;

	IDirect3DTexture8 *texture = nullptr;
	result = device->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8,
		D3DPOOL_MANAGED, &texture);
	if (FAILED(result) || texture == nullptr)
		return Fail("Failed to create a compatibility texture", result);
	resources.texture = texture;
	D3DLOCKED_RECT locked_rect = {};
	result = texture->LockRect(0, &locked_rect, nullptr, 0);
	if (FAILED(result) || locked_rect.pBits == nullptr)
		return Fail("Failed to lock a compatibility texture", result);
	std::memset(locked_rect.pBits, 0x7f, static_cast<size_t>(locked_rect.Pitch) * 8);
	if (FAILED(texture->UnlockRect(0)))
		return Fail("Failed to unlock a compatibility texture");

	IDirect3DVertexBuffer8 *vertex_buffer = nullptr;
	result = device->CreateVertexBuffer(128, 0, D3DFVF_XYZ, D3DPOOL_MANAGED,
		&vertex_buffer);
	if (FAILED(result) || vertex_buffer == nullptr)
		return Fail("Failed to create a compatibility vertex buffer", result);
	resources.vertex_buffer = vertex_buffer;
	BYTE *vertex_bytes = nullptr;
	if (FAILED(vertex_buffer->Lock(0, 0, &vertex_bytes, 0)) || vertex_bytes == nullptr)
		return Fail("Failed to lock a compatibility vertex buffer");
	std::memset(vertex_bytes, 0, 128);
	if (FAILED(vertex_buffer->Unlock()))
		return Fail("Failed to unlock a compatibility vertex buffer");

	IDirect3DIndexBuffer8 *index_buffer = nullptr;
	result = device->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
		&index_buffer);
	if (FAILED(result) || index_buffer == nullptr)
		return Fail("Failed to create a compatibility index buffer", result);
	resources.index_buffer = index_buffer;
	BYTE *index_bytes = nullptr;
	if (FAILED(index_buffer->Lock(0, 0, &index_bytes, 0)) || index_bytes == nullptr)
		return Fail("Failed to lock a compatibility index buffer");
	std::memset(index_bytes, 0, 64);
	if (FAILED(index_buffer->Unlock()))
		return Fail("Failed to unlock a compatibility index buffer");

	D3DMATRIX identity = {};
	identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
	if (FAILED(device->SetTransform(D3DTS_WORLD, &identity)))
		return Fail("Failed to set a compatibility transform");
	D3DMATRIX queried_matrix = {};
	if (FAILED(device->GetTransform(D3DTS_WORLD, &queried_matrix)) ||
		!EqualMatrix(identity, queried_matrix))
		return Fail("Compatibility transform state did not round-trip");

	D3DVIEWPORT8 viewport = { 0, 0, 64, 64, 0.0f, 1.0f };
	if (FAILED(device->SetViewport(&viewport)))
		return Fail("Failed to set the compatibility viewport");
	D3DVIEWPORT8 queried_viewport = {};
	if (FAILED(device->GetViewport(&queried_viewport)) ||
		std::memcmp(&viewport, &queried_viewport, sizeof(viewport)) != 0)
		return Fail("Compatibility viewport did not round-trip");

	IDirect3DSurface8 *depth_surface = nullptr;
	if (FAILED(device->GetDepthStencilSurface(&depth_surface)) || depth_surface == nullptr)
		return Fail("Failed to query the compatibility depth surface");
	resources.depth_surface = depth_surface;
	IDirect3DSurface8 *render_target = nullptr;
	result = device->CreateRenderTarget(16, 16, display_mode.Format,
		D3DMULTISAMPLE_NONE, TRUE, &render_target);
	if (FAILED(result) || render_target == nullptr)
		return Fail("Failed to create a compatibility render target", result);
	resources.render_target = render_target;

	DWORD state_token = 0;
	result = device->CreateStateBlock(D3DSBT_ALL, &state_token);
	if (FAILED(result) || state_token < 0x20000001u || state_token > 0x3fffffffu)
		return Fail("State block did not return an x64-safe opaque token", result);
	if (FAILED(device->CaptureStateBlock(state_token)) ||
		FAILED(device->ApplyStateBlock(state_token)) ||
		FAILED(device->DeleteStateBlock(state_token)))
		return Fail("State block lifecycle failed");

	if (FAILED(device->BeginStateBlock()))
		return Fail("Failed to begin state-block recording");
	if (FAILED(device->SetRenderState(D3DRS_ZENABLE, TRUE)))
		return Fail("Failed to record state-block state");
	DWORD recorded_state_token = 0;
	result = device->EndStateBlock(&recorded_state_token);
	if (FAILED(result) || recorded_state_token < 0x20000001u ||
		recorded_state_token > 0x3fffffffu ||
		FAILED(device->DeleteStateBlock(recorded_state_token)))
		return Fail("Recorded state block did not use an opaque token", result);

	const DWORD declaration[] = {
		D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3), D3DVSD_END()
	};
	DWORD vertex_shader = 0;
	result = device->CreateVertexShader(declaration, nullptr, &vertex_shader, 0);
	if (FAILED(result) || vertex_shader < 0x80000001u)
		return Fail("Vertex declaration did not return an opaque shader handle", result);
	if (FAILED(device->SetVertexShader(vertex_shader)) ||
		FAILED(device->DeleteVertexShader(vertex_shader)) ||
		device->SetVertexShader(vertex_shader) != D3DERR_INVALIDCALL)
		return Fail("Vertex shader handle lifecycle failed");

	const DWORD pixel_shader_program[] = {
		0xffff0101, 0x0009fffe, 0x58443344, 0x68532038,
		0x72656461, 0x73734120, 0x6c626d65, 0x56207265,
		0x69737265, 0x30206e6f, 0x0031392e, 0x00000042,
		0xb00f0000, 0x00000042, 0xb00f0001, 0x00000042,
		0xb00f0002, 0xb0e40001, 0x00000005, 0x800f0000,
		0x90e40000, 0xb0e40000, 0x00000005, 0x80070001,
		0xb0e40002, 0xa0e40000, 0x00000002, 0x80070000,
		0x80e40000, 0x80e40001, 0x0000ffff
	};
	DWORD pixel_shader = 0;
	result = device->CreatePixelShader(pixel_shader_program, &pixel_shader);
	if (FAILED(result) || pixel_shader < 0x40000001u ||
		pixel_shader > 0x5fffffffu)
		return Fail("Pixel shader did not return an opaque x64-safe handle", result);
	if (!LoadedModuleMatches(GetModuleHandleW(L"d3dcompiler_43.dll"), argv[2]))
		return Fail("Compatibility bridge did not resolve its exact app-local D3DCompiler runtime");
	if (FAILED(device->SetPixelShader(pixel_shader)) ||
		FAILED(device->DeletePixelShader(pixel_shader)) ||
		device->SetPixelShader(pixel_shader) != D3DERR_INVALIDCALL)
		return Fail("Pixel shader handle lifecycle failed");

	if (FAILED(device->BeginScene()) ||
		FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
			0xff102030u, 1.0f, 0)) || FAILED(device->EndScene()))
		return Fail("Compatibility frame lifecycle failed");

	const DWORD state_block_value = 0x5aU;
	const DWORD state_before_reset = 0xa5U;
	const DWORD state_after_reset = 0xc3U;
	DWORD reset_state_token = 0;
	if (FAILED(device->BeginStateBlock()) ||
		FAILED(device->SetRenderState(D3DRS_ALPHAREF, state_block_value)))
		return Fail("Failed to record the state block retained across reset");
	result = device->EndStateBlock(&reset_state_token);
	if (FAILED(result) || reset_state_token < 0x20000001u ||
		reset_state_token > 0x3fffffffu)
		return Fail("Failed to create the state block retained across reset", result);
	if (FAILED(device->SetRenderState(D3DRS_ALPHAREF, state_before_reset)) ||
		FAILED(device->ApplyStateBlock(reset_state_token)))
		return Fail("Failed to apply the state block before reset");
	DWORD queried_state = 0;
	if (FAILED(device->GetRenderState(D3DRS_ALPHAREF, &queried_state)) ||
		queried_state != state_block_value)
		return Fail("State block did not apply its recorded state before reset");

	render_target->Release();
	resources.render_target = nullptr;
	depth_surface->Release();
	resources.depth_surface = nullptr;
	index_buffer->Release();
	resources.index_buffer = nullptr;
	vertex_buffer->Release();
	resources.vertex_buffer = nullptr;
	texture->Release();
	resources.texture = nullptr;

	result = device->Reset(&presentation);
	if (FAILED(result))
		return Fail("Compatibility device reset failed", result);
	if (FAILED(device->SetRenderState(D3DRS_ALPHAREF, state_after_reset)) ||
		device->ApplyStateBlock(reset_state_token) != D3D_OK ||
		FAILED(device->GetRenderState(D3DRS_ALPHAREF, &queried_state)) ||
		queried_state != state_after_reset)
		return Fail("Compatibility reset did not invalidate retained state blocks safely");
	IDirect3DTexture8 *post_reset_texture = nullptr;
	result = device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8,
		D3DPOOL_MANAGED, &post_reset_texture);
	if (FAILED(result) || post_reset_texture == nullptr)
		return Fail("Compatibility resource creation failed after reset", result);
	resources.post_reset_texture = post_reset_texture;

	std::puts("Native x64 D3D8 compatibility device, resources, opaque handles, shaders, and reset passed.");
	return 0;
}
