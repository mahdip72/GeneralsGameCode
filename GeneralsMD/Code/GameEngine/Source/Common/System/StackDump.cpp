/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if defined(RTS_DEBUG) || defined(IG_DEBUG_STACKTRACE)

#pragma pack(push, 8)

#include "Common/StackDump.h"
#include "Common/Debug.h"

#include "WWLib/DbgHelpLoader.h"

#include "Utility/stdint_adapter.h"

//*****************************************************************************
//	Prototypes
//*****************************************************************************
BOOL InitSymbolInfo();
#if defined(_WIN64)
void MakeStackTrace(const CONTEXT &context, int skipFrames, void (*callback)(const char*));
#else
void MakeStackTrace(DWORD myeip,DWORD myesp,DWORD myebp, int skipFrames, void (*callback)(const char*));
#endif
void GetFunctionDetails(void *pointer, char*name, char*filename, unsigned int* linenumber, uintptr_t* address);
void WriteStackLine(void*address, void (*callback)(const char*));

//*****************************************************************************
//	Mis-named globals :-)
//*****************************************************************************
#if !defined(_WIN64)
static CONTEXT gsContext;
#endif


//*****************************************************************************
//*****************************************************************************
void StackDumpDefaultHandler(const char*line)
{
	DEBUG_LOG((line));
}


//*****************************************************************************
//*****************************************************************************
void StackDump(void (*callback)(const char*))
{
	if (callback == nullptr)
	{
		callback = StackDumpDefaultHandler;
	}

	if (!InitSymbolInfo())
		return;

#if defined(_WIN64)
	CONTEXT context = {};
	::RtlCaptureContext(&context);
	MakeStackTrace(context, 2, callback);
#else
	DWORD myeip,myesp,myebp;

#if defined(_MSC_VER)
_asm
{
MYEIP1:
 mov eax, MYEIP1
 mov dword ptr [myeip] , eax
 mov eax, esp
 mov dword ptr [myesp] , eax
 mov eax, ebp
 mov dword ptr [myebp] , eax
}
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(_M_IX86))
	// GCC/Clang inline assembly for x86-32
	__asm__ __volatile__(
		"call 1f\n\t"
		"1: pop %0\n\t"
		"mov %%esp, %1\n\t"
		"mov %%ebp, %2"
		: "=r"(myeip), "=r"(myesp), "=r"(myebp)
		:
		: "memory"
	);
#else
	#error "Unsupported compiler or architecture for register capture"
#endif

	MakeStackTrace(myeip,myesp,myebp, 2, callback);
#endif
}


//*****************************************************************************
//*****************************************************************************
#if defined(_WIN64)
void StackDumpFromContext(const CONTEXT &context, void (*callback)(const char*))
#else
void StackDumpFromContext(DWORD eip,DWORD esp,DWORD ebp, void (*callback)(const char*))
#endif
{
	if (callback == nullptr)
	{
		callback = StackDumpDefaultHandler;
	}

	if (!InitSymbolInfo())
		return;

#if defined(_WIN64)
	MakeStackTrace(context, 0, callback);
#else
	MakeStackTrace(eip,esp,ebp, 0, callback); // portability-audit: x86-context
#endif
}


//*****************************************************************************
//*****************************************************************************
BOOL InitSymbolInfo()
{
	if (DbgHelpLoader::isLoaded())
		return TRUE;

	if (DbgHelpLoader::isFailed())
		return FALSE;

	if (!DbgHelpLoader::load())
	{
		atexit(DbgHelpLoader::unload);
		return FALSE;
	}

	char pathname[_MAX_PATH+1];
	char drive[10];
	char directory[_MAX_PATH+1];
	HANDLE process;

	DbgHelpLoader::symSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_OMAP_FIND_NEAREST);

	process = GetCurrentProcess();

	//Get the apps name
	::GetModuleFileName(nullptr, pathname, _MAX_PATH);

	// turn it into a search path
	_splitpath(pathname, drive, directory, nullptr, nullptr);
	sprintf(pathname, "%s:\\%s", drive, directory);

	// append the current directory to build a search path for SymInit
	::lstrcat(pathname, ";.;");

	if(DbgHelpLoader::symInitialize(process, pathname, FALSE))
	{
		// regenerate the name of the app
		::GetModuleFileName(nullptr, pathname, _MAX_PATH);
#if defined(_WIN64)
		if(DbgHelpLoader::symLoadModule64(process, nullptr, pathname, nullptr, 0, 0))
#else
		if(DbgHelpLoader::symLoadModule(process, nullptr, pathname, nullptr, 0, 0))
#endif
		{
				//Load any other relevant modules (ie dlls) here
				atexit(DbgHelpLoader::unload);
				return TRUE;
		}
	}

	DbgHelpLoader::unload();
	return FALSE;
}


//*****************************************************************************
//*****************************************************************************
#if defined(_WIN64)
void MakeStackTrace(const CONTEXT &context, int skipFrames, void (*callback)(const char*))
{
	CONTEXT walkContext = context;
	STACKFRAME64 stackFrame = {};
	stackFrame.AddrPC.Mode = AddrModeFlat;
	stackFrame.AddrPC.Offset = walkContext.Rip;
	stackFrame.AddrStack.Mode = AddrModeFlat;
	stackFrame.AddrStack.Offset = walkContext.Rsp;
	stackFrame.AddrFrame.Mode = AddrModeFlat;
	stackFrame.AddrFrame.Offset = walkContext.Rbp;

	HANDLE thread = GetCurrentThread();
	HANDLE process = GetCurrentProcess();
	callback("Call Stack\n**********\n");

	unsigned int remaining = 30 + static_cast<unsigned int>(skipFrames);
	while (remaining-- != 0) {
		const BOOL walked = DbgHelpLoader::stackWalk64(IMAGE_FILE_MACHINE_AMD64,
			process, thread, &stackFrame, &walkContext, nullptr,
			DbgHelpLoader::symFunctionTableAccess64,
			DbgHelpLoader::symGetModuleBase64, nullptr);
		if (!walked || stackFrame.AddrPC.Offset == 0) {
			break;
		}
		if (skipFrames > 0) {
			--skipFrames;
			continue;
		}
		WriteStackLine(reinterpret_cast<void *>(
			static_cast<uintptr_t>(stackFrame.AddrPC.Offset)), callback);
	}
}
#else
void MakeStackTrace(DWORD myeip,DWORD myesp,DWORD myebp, int skipFrames, void (*callback)(const char*))
{
STACKFRAME      stack_frame;
BOOL            b_ret = TRUE;

HANDLE thread = GetCurrentThread();
HANDLE process = GetCurrentProcess();

memset(&gsContext, 0, sizeof(CONTEXT));
gsContext.ContextFlags = CONTEXT_FULL;

memset(&stack_frame, 0, sizeof(STACKFRAME));
stack_frame.AddrPC.Mode = AddrModeFlat;
stack_frame.AddrPC.Offset = myeip;
stack_frame.AddrStack.Mode = AddrModeFlat;
stack_frame.AddrStack.Offset = myesp;
stack_frame.AddrFrame.Mode = AddrModeFlat;
stack_frame.AddrFrame.Offset = myebp;
{
/*
    if(GetThreadContext(thread, &gsContext))
    {
        memset(&stack_frame, 0, sizeof(STACKFRAME));
        stack_frame.AddrPC.Mode = AddrModeFlat;
        stack_frame.AddrPC.Offset = gsContext.Eip;
        stack_frame.AddrStack.Mode = AddrModeFlat;
        stack_frame.AddrStack.Offset = gsContext.Esp;
        stack_frame.AddrFrame.Mode = AddrModeFlat;
        stack_frame.AddrFrame.Offset = gsContext.Ebp;
*/

		//{
			callback("Call Stack\n**********\n");

			// Skip some ?
			unsigned int skip = skipFrames;
			while (b_ret&&skip)
			{
					b_ret = DbgHelpLoader::stackWalk(      IMAGE_FILE_MACHINE_I386,
											process,
											thread,
											&stack_frame,
											nullptr, //&gsContext,
											nullptr,
											DbgHelpLoader::symFunctionTableAccess,
											DbgHelpLoader::symGetModuleBase,
											nullptr);
					skip--;
			}

			skip = 30;
			while(b_ret&&skip)
			{

					b_ret = DbgHelpLoader::stackWalk(      IMAGE_FILE_MACHINE_I386,
											process,
											thread,
											&stack_frame,
											nullptr, //&gsContext,
											nullptr,
											DbgHelpLoader::symFunctionTableAccess,
											DbgHelpLoader::symGetModuleBase,
											nullptr);



					if (b_ret) WriteStackLine((void *) stack_frame.AddrPC.Offset, callback);
					skip--;
			}
	}
}
#endif


//*****************************************************************************
//*****************************************************************************
void GetFunctionDetails(void *pointer, char*name, char*filename, unsigned int* linenumber, uintptr_t* address)
{
	if (!InitSymbolInfo())
		return;

	if (name)
	{
		strcpy(name, "<Unknown>");
	}
	if (filename)
	{
		strcpy(filename, "<Unknown>");
	}
	if (linenumber)
	{
		*linenumber = 0xFFFFFFFF;
	}
	if (address)
	{
		*address = static_cast<uintptr_t>(-1);
	}

#if defined(_WIN64)
	DWORD64 displacement = 0;
	HANDLE process = ::GetCurrentProcess();
	char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
	PSYMBOL_INFO symbol = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen = 512;
	const DWORD64 pointerAddress = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(pointer));
	if (DbgHelpLoader::symFromAddr(process, pointerAddress, &displacement, symbol)) {
		if (name) {
			strcpy(name, symbol->Name);
			strcat(name, "();");
		}
		IMAGEHLP_LINE64 line = {};
		line.SizeOfStruct = sizeof(line);
		DWORD lineDisplacement = 0;
		if (DbgHelpLoader::symGetLineFromAddr64(process, pointerAddress,
				&lineDisplacement, &line)) {
			if (filename) strcpy(filename, line.FileName);
			if (linenumber) *linenumber = static_cast<unsigned int>(line.LineNumber);
			if (address) *address = static_cast<uintptr_t>(line.Address);
		}
	}
#else
	ULONG displacement = 0;

    HANDLE process = ::GetCurrentProcess();

    char symbol_buffer[512 + sizeof(IMAGEHLP_SYMBOL)];
    memset(symbol_buffer, 0, sizeof(symbol_buffer));

    PIMAGEHLP_SYMBOL psymbol = (PIMAGEHLP_SYMBOL)symbol_buffer;
    psymbol->SizeOfStruct = sizeof(symbol_buffer);
    psymbol->MaxNameLength = 512;

	if (DbgHelpLoader::symGetSymFromAddr(process, (DWORD) pointer, &displacement, psymbol))
	{
		if (name)
		{
			strcpy(name, psymbol->Name);
			strcat(name, "();");
		}

		// Get line now

		IMAGEHLP_LINE line;
		memset(&line,0,sizeof(line));
		line.SizeOfStruct = sizeof(line);

		if (DbgHelpLoader::symGetLineFromAddr(process, (DWORD) pointer, &displacement, &line))
		{
			if (filename)
			{
				strcpy(filename, line.FileName);
			}
			if (linenumber)
			{
				*linenumber = (unsigned int)line.LineNumber;
			}
			if (address)
			{
				*address = (unsigned int)line.Address;
			}
		}
	}
#endif
}


//*****************************************************************************
// Gets last x addresses from the stack
//*****************************************************************************
void FillStackAddresses(void**addresses, unsigned int count, unsigned int skip)
{
	if (!InitSymbolInfo())
		return;

#if defined(_WIN64)
	const unsigned int requested = count;
	const ULONG framesToSkip = skip + 1;
	const ULONG framesToCapture = count > USHRT_MAX ? USHRT_MAX : count;
	const USHORT captured = ::CaptureStackBackTrace(framesToSkip, framesToCapture,
		addresses, nullptr);
	for (unsigned int index = captured; index < requested; ++index) {
		addresses[index] = nullptr;
	}
	return;
#else

	STACKFRAME	stack_frame;


	HANDLE thread = GetCurrentThread();
	HANDLE process = GetCurrentProcess();

    memset(&gsContext, 0, sizeof(CONTEXT));
    gsContext.ContextFlags = CONTEXT_FULL;

	DWORD myeip,myesp,myebp;
#if defined(_MSC_VER)
_asm
{
MYEIP2:
 mov eax, MYEIP2
 mov dword ptr [myeip] , eax
 mov eax, esp
 mov dword ptr [myesp] , eax
 mov eax, ebp
 mov dword ptr [myebp] , eax
 xor eax,eax
}
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(_M_IX86))
	// GCC/Clang inline assembly for x86-32
	__asm__ __volatile__(
		"call 1f\n\t"
		"1: pop %0\n\t"
		"mov %%esp, %1\n\t"
		"mov %%ebp, %2\n\t"
		"xor %%eax, %%eax"
		: "=r"(myeip), "=r"(myesp), "=r"(myebp)
		:
		: "eax", "memory"
	);
#else
	#error "Unsupported compiler or architecture for register capture"
#endif
memset(&stack_frame, 0, sizeof(STACKFRAME));
stack_frame.AddrPC.Mode = AddrModeFlat;
stack_frame.AddrPC.Offset = myeip;
stack_frame.AddrStack.Mode = AddrModeFlat;
stack_frame.AddrStack.Offset = myesp;
stack_frame.AddrFrame.Mode = AddrModeFlat;
stack_frame.AddrFrame.Offset = myebp;

{
/*
    if(GetThreadContext(thread, &gsContext))
    {
        memset(&stack_frame, 0, sizeof(STACKFRAME));
        stack_frame.AddrPC.Mode = AddrModeFlat;
        stack_frame.AddrPC.Offset = gsContext.Eip;
        stack_frame.AddrStack.Mode = AddrModeFlat;
        stack_frame.AddrStack.Offset = gsContext.Esp;
        stack_frame.AddrFrame.Mode = AddrModeFlat;
        stack_frame.AddrFrame.Offset = gsContext.Ebp;
*/

		Bool stillgoing = TRUE;
//	unsigned int cd = count;

		// Skip some?
		while (stillgoing&&skip)
		{
			stillgoing = DbgHelpLoader::stackWalk(IMAGE_FILE_MACHINE_I386,
								process,
								thread,
								&stack_frame,
								nullptr,	//&gsContext,
								nullptr,
								DbgHelpLoader::symFunctionTableAccess,
								DbgHelpLoader::symGetModuleBase,
								nullptr) != 0;
			skip--;
		}

		while(stillgoing&&count)
		{
			stillgoing = DbgHelpLoader::stackWalk(IMAGE_FILE_MACHINE_I386,
								process,
								thread,
								&stack_frame,
								nullptr, //&gsContext,
								nullptr,
								DbgHelpLoader::symFunctionTableAccess,
								DbgHelpLoader::symGetModuleBase,
								nullptr) != 0;
			if (stillgoing)
			{
				*addresses  = (void*)stack_frame.AddrPC.Offset;
				addresses++;
				count--;
			}
		}

		// Fill remainder
		while (count)
		{
			*addresses = nullptr;
			addresses++;
			count--;
		}

	}
/*
	else
	{
		memset(addresses,nullptr,count*sizeof(void*));
	}
*/
#endif
}



//*****************************************************************************
// Do full stack dump using an address array
//*****************************************************************************
void StackDumpFromAddresses(void**addresses, unsigned int count, void (*callback)(const char *))
{
	if (callback == nullptr)
	{
		callback = StackDumpDefaultHandler;
	}

	if (!InitSymbolInfo())
		return;

	while ((count--) && (*addresses!=nullptr))
	{
		WriteStackLine(*addresses,callback);
		addresses++;
	}
}


AsciiString g_LastErrorDump;
//*****************************************************************************
//*****************************************************************************
void WriteStackLine(void*address, void (*callback)(const char*))
{
	char line[MAX_PATH];
	char function_name[512];
	char filename[MAX_PATH];
	unsigned int linenumber;
	uintptr_t addr;

	GetFunctionDetails(address, function_name, filename, &linenumber, &addr);
    sprintf(line, "  %s(%d) : %s 0x%08p", filename, linenumber, function_name, address);
		if (g_LastErrorDump.isNotEmpty()) {
			g_LastErrorDump.concat(line);
			g_LastErrorDump.concat("\n");
		}
	callback(line);
}


//*****************************************************************************
//*****************************************************************************
void DumpExceptionInfo( unsigned int u, EXCEPTION_POINTERS* e_info )
{
	DEBUG_LOG_RAW(("\n"));
	DEBUG_LOG(( "********** EXCEPTION DUMP ****************" ));
	/*
	** List of possible exceptions
	*/
	 g_LastErrorDump.clear();

	static const unsigned int _codes[] = {
		EXCEPTION_ACCESS_VIOLATION,
		EXCEPTION_ARRAY_BOUNDS_EXCEEDED,
		EXCEPTION_BREAKPOINT,
		EXCEPTION_DATATYPE_MISALIGNMENT,
		EXCEPTION_FLT_DENORMAL_OPERAND,
		EXCEPTION_FLT_DIVIDE_BY_ZERO,
		EXCEPTION_FLT_INEXACT_RESULT,
		EXCEPTION_FLT_INVALID_OPERATION,
		EXCEPTION_FLT_OVERFLOW,
		EXCEPTION_FLT_STACK_CHECK,
		EXCEPTION_FLT_UNDERFLOW,
		EXCEPTION_ILLEGAL_INSTRUCTION,
		EXCEPTION_IN_PAGE_ERROR,
		EXCEPTION_INT_DIVIDE_BY_ZERO,
		EXCEPTION_INT_OVERFLOW,
		EXCEPTION_INVALID_DISPOSITION,
		EXCEPTION_NONCONTINUABLE_EXCEPTION,
		EXCEPTION_PRIV_INSTRUCTION,
		EXCEPTION_SINGLE_STEP,
		EXCEPTION_STACK_OVERFLOW,
		0xffffffff
	};

	/*
	** Information about each exception type.
	*/
	static char const * _code_txt[] = {
		"Error code: EXCEPTION_ACCESS_VIOLATION\nDescription: The thread tried to read from or write to a virtual address for which it does not have the appropriate access.",
		"Error code: EXCEPTION_ARRAY_BOUNDS_EXCEEDED\nDescription: The thread tried to access an array element that is out of bounds and the underlying hardware supports bounds checking.",
		"Error code: EXCEPTION_BREAKPOINT\nDescription: A breakpoint was encountered.",
		"Error code: EXCEPTION_DATATYPE_MISALIGNMENT\nDescription: The thread tried to read or write data that is misaligned on hardware that does not provide alignment. For example, 16-bit values must be aligned on 2-byte boundaries; 32-bit values on 4-byte boundaries, and so on.",
		"Error code: EXCEPTION_FLT_DENORMAL_OPERAND\nDescription: One of the operands in a floating-point operation is denormal. A denormal value is one that is too small to represent as a standard floating-point value.",
		"Error code: EXCEPTION_FLT_DIVIDE_BY_ZERO\nDescription: The thread tried to divide a floating-point value by a floating-point divisor of zero.",
		"Error code: EXCEPTION_FLT_INEXACT_RESULT\nDescription: The result of a floating-point operation cannot be represented exactly as a decimal fraction.",
		"Error code: EXCEPTION_FLT_INVALID_OPERATION\nDescription: Some strange unknown floating point operation was attempted.",
		"Error code: EXCEPTION_FLT_OVERFLOW\nDescription: The exponent of a floating-point operation is greater than the magnitude allowed by the corresponding type.",
		"Error code: EXCEPTION_FLT_STACK_CHECK\nDescription: The stack overflowed or underflowed as the result of a floating-point operation.",
		"Error code: EXCEPTION_FLT_UNDERFLOW\nDescription:	The exponent of a floating-point operation is less than the magnitude allowed by the corresponding type.",
		"Error code: EXCEPTION_ILLEGAL_INSTRUCTION\nDescription:	The thread tried to execute an invalid instruction.",
		"Error code: EXCEPTION_IN_PAGE_ERROR\nDescription:	The thread tried to access a page that was not present, and the system was unable to load the page. For example, this exception might occur if a network connection is lost while running a program over the network.",
		"Error code: EXCEPTION_INT_DIVIDE_BY_ZERO\nDescription: The thread tried to divide an integer value by an integer divisor of zero.",
		"Error code: EXCEPTION_INT_OVERFLOW\nDescription: The result of an integer operation caused a carry out of the most significant bit of the result.",
		"Error code: EXCEPTION_INVALID_DISPOSITION\nDescription: An exception handler returned an invalid disposition to the exception dispatcher. Programmers using a high-level language such as C should never encounter this exception.",
		"Error code: EXCEPTION_NONCONTINUABLE_EXCEPTION\nDescription: The thread tried to continue execution after a noncontinuable exception occurred.",
		"Error code: EXCEPTION_PRIV_INSTRUCTION\nDescription: The thread tried to execute an instruction whose operation is not allowed in the current machine mode.",
		"Error code: EXCEPTION_SINGLE_STEP\nDescription: A trace trap or other single-instruction mechanism signaled that one instruction has been executed.",
		"Error code: EXCEPTION_STACK_OVERFLOW\nDescription: The thread used up its stack.",
		"Error code: ?????\nDescription: Unknown exception."
	};

	DEBUG_LOG( ("Dump exception info") );
	CONTEXT *context = e_info->ContextRecord;
	/*
	** The following are set for access violation only
	*/
	int access_read_write=-1;
	uintptr_t access_address = 0;
	AsciiString msg;

// DOUBLE_DEBUG does a DEBUG_LOG, and concats to g_LastErrorDump.  jba.
#define DOUBLE_DEBUG(x) { msg.format x; g_LastErrorDump.concat(msg); DEBUG_LOG( x ); }

	if ( e_info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION )
	{
		DOUBLE_DEBUG (("Exception is access violation"));
		access_read_write = e_info->ExceptionRecord->ExceptionInformation[0];  // 0=read, 1=write
		access_address = e_info->ExceptionRecord->ExceptionInformation[1];
	}
	else
	{
		DOUBLE_DEBUG (("Exception code is %x", e_info->ExceptionRecord->ExceptionCode));
	}
	const uintptr_t winMainAddress = reinterpret_cast<uintptr_t>(&WinMain);
	DOUBLE_DEBUG(("WinMain at %p", reinterpret_cast<void *>(winMainAddress)));
	/*
	** Match the exception type with the error string and print it out
	*/
	int i=0;
	for ( ; _codes[i] != 0xffffffff ; i++ )
	{
		if ( _codes[i] == e_info->ExceptionRecord->ExceptionCode )
		{
			DEBUG_LOG ( ("Found exception description") );
			break;
		}
	}
	DOUBLE_DEBUG( ("%s", _code_txt[i]));
	/** For access violations, print out the violation address and if it was read or write.
	*/
	if ( e_info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION )
	{
		if ( access_read_write )
		{
			DOUBLE_DEBUG( ("Access address:%p was written to.", reinterpret_cast<void *>(access_address)));
		}
		else
		{
			DOUBLE_DEBUG( ("Access address:%p was read from.", reinterpret_cast<void *>(access_address)));
		}
	}

	DOUBLE_DEBUG (("\nStack Dump:"));
#if defined(_WIN64)
	StackDumpFromContext(*context, nullptr);
#else
	StackDumpFromContext(context->Eip, context->Esp, context->Ebp, nullptr);
#endif

	DOUBLE_DEBUG (("\nDetails:"));

	DOUBLE_DEBUG (("Register dump..."));

	/*
	** Dump the registers.
	*/
#if defined(_WIN64)
	DOUBLE_DEBUG ( ( "Rip:%016llX\tRsp:%016llX\tRbp:%016llX", context->Rip, context->Rsp, context->Rbp));
	DOUBLE_DEBUG ( ( "Rax:%016llX\tRbx:%016llX\tRcx:%016llX", context->Rax, context->Rbx, context->Rcx));
	DOUBLE_DEBUG ( ( "Rdx:%016llX\tRsi:%016llX\tRdi:%016llX", context->Rdx, context->Rsi, context->Rdi));
#else
	DOUBLE_DEBUG ( ( "Eip:%08X\tEsp:%08X\tEbp:%08X", context->Eip, context->Esp, context->Ebp));
	DOUBLE_DEBUG ( ( "Eax:%08X\tEbx:%08X\tEcx:%08X", context->Eax, context->Ebx, context->Ecx));
	DOUBLE_DEBUG ( ( "Edx:%08X\tEsi:%08X\tEdi:%08X", context->Edx, context->Esi, context->Edi));
#endif
	DOUBLE_DEBUG ( ( "EFlags:%08X ", context->EFlags));
	DOUBLE_DEBUG ( ( "CS:%04x  SS:%04x  DS:%04x  ES:%04x  FS:%04x  GS:%04x", context->SegCs, context->SegSs, context->SegDs, context->SegEs, context->SegFs, context->SegGs));

	/*
	** Dump the bytes at EIP. This will make it easier to match the crash address with later versions of the game.
	*/
	char scrap[512];
	DOUBLE_DEBUG ( ("EIP bytes dump..."));
#if defined(_WIN64)
	const uintptr_t instructionPointer = static_cast<uintptr_t>(context->Rip);
#else
	const uintptr_t instructionPointer = static_cast<uintptr_t>(context->Eip); // portability-audit: x86-context
#endif
	_snprintf(scrap, ARRAY_SIZE(scrap), "\nBytes at instruction pointer (%p)  : ",
		reinterpret_cast<void *>(instructionPointer));

	unsigned char *eip_ptr = reinterpret_cast<unsigned char *>(instructionPointer);
	char bytestr[32];

	for (int c = 0 ; c < 32 ; c++)
	{
		if (IsBadReadPtr(eip_ptr, 1))
		{
			lstrcat (scrap, "?? ");
		}
		else
		{
			sprintf (bytestr, "%02X ", *eip_ptr);
			strlcat(scrap, bytestr, ARRAY_SIZE(scrap));
		}
		eip_ptr++;
	}

	DOUBLE_DEBUG ( ( (scrap)));
	DEBUG_LOG(( "********** END EXCEPTION DUMP ****************" ));
	DEBUG_LOG_RAW(("\n"));
}


#pragma pack(pop)

#endif

