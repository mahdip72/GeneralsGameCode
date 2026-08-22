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

/////////////////////////////////////////////////////////////////////////EA-V1
// $File: //depot/GeneralsMD/Staging/code/Libraries/Source/debug/debug_stack.cpp $
// $Author: KMorness $
// $Revision: #2 $
// $DateTime: 2005/01/19 15:02:33 $
//
// (c) 2003 Electronic Arts
//
// Stack walker
//////////////////////////////////////////////////////////////////////////////

#include "debug.h"
#include "debug_stack.h"
#include <windows.h>
#include "WWLib/stringex.h"
#include <imagehlp.h>
#include <cstdint>
#include <mutex>

// imagehlp aliases StackWalk to StackWalk64 on x64.  This source owns a
// DebugStackwalk::StackWalk member with the legacy name, so keep that macro
// from rewriting its out-of-class definition.
#ifdef StackWalk
#undef StackWalk
#endif

// Definitions to allow run-time linking to the dbghelp.dll functions.

#define DBGHELP(name,ret,par) typedef ret (WINAPI *name##Type) par;
#include "debug_stack.inl"
#undef DBGHELP

#define DBGHELP(name,ret,par) name##Type _##name;
static union
{
  struct
  {
#include "debug_stack.inl"
  };
  uintptr_t funcPtr[1];
} gDbg;
#undef DBGHELP

#define DBGHELP(name,ret,par) #name,
static char const *const DebughelpFunctionNames[] =
{
#include "debug_stack.inl"
	nullptr
};
#undef DBGHELP

#if defined(_WIN64)
using DebugDwordAddress = DWORD64;
using DebugDisplacement = DWORD64;
using DebugSymbol = IMAGEHLP_SYMBOL64;
using DebugLine = IMAGEHLP_LINE64;
#define DEBUG_SYM_GET_MODULE_BASE gDbg._SymGetModuleBase64
#define DEBUG_SYM_GET_SYMBOL gDbg._SymGetSymFromAddr64
#define DEBUG_SYM_GET_LINE gDbg._SymGetLineFromAddr64
#else
using DebugDwordAddress = DWORD;
using DebugDisplacement = DWORD;
using DebugSymbol = IMAGEHLP_SYMBOL;
using DebugLine = IMAGEHLP_LINE;
#define DEBUG_SYM_GET_MODULE_BASE gDbg._SymGetModuleBase
#define DEBUG_SYM_GET_SYMBOL gDbg._SymGetSymFromAddr
#define DEBUG_SYM_GET_LINE gDbg._SymGetLineFromAddr
#endif

// local dbghelp.dll module handle
static HMODULE g_dbghelp;

// local flag that is true if we're using an old dbghelp.dll version
static bool g_oldDbghelp;

// DbgHelp maintains process-wide symbol state and its APIs are not safe to
// call concurrently. Keep initialization one-time and serialize all calls
// that use the loaded entry points. A recursive mutex also prevents an
// exception path from deadlocking if it re-enters stack reporting on the
// same thread while a symbol operation is in progress.
static std::once_flag g_dbghelpInitOnce;
static std::recursive_mutex g_dbghelpMutex;

static void InitDbghelpOnce()
{
  // already called?
  if (g_dbghelp)
    return;

	// firstly check for dbghelp.dll in the EXE directory
	char dbgHelpPath[256];
	if (GetModuleFileName(nullptr,dbgHelpPath,sizeof(dbgHelpPath)))
	{
		char *slash=strrchr(dbgHelpPath,'\\');
		if (slash)
		{
			strcpy(slash+1,"DBGHELP.DLL");
			g_dbghelp=::LoadLibrary(dbgHelpPath);
		}
	}
	if (!g_dbghelp)
		// load any version we can
		g_dbghelp=::LoadLibrary("DBGHELP.DLL");

  if (!g_dbghelp)
    return;

  // Get function addresses
  uintptr_t *funcptr=gDbg.funcPtr;
  unsigned k=0;
  for (;DebughelpFunctionNames[k];++k,++funcptr)
  {
    *funcptr=reinterpret_cast<uintptr_t>(GetProcAddress(g_dbghelp,DebughelpFunctionNames[k]));
    if (!*funcptr)
      break;
  }
  if (DebughelpFunctionNames[k])
  {
    // not all functions found -> clear them all
    while (funcptr!=gDbg.funcPtr)
      *--funcptr=0;
  }
  else
  {
    // Set options
    gDbg._SymSetOptions(gDbg._SymGetOptions()|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);

    // Init module
    gDbg._SymInitialize(GetCurrentProcess(),nullptr,TRUE);

    // Check: are we using a newer version of dbghelp.dll?
    // (older versions have some serious issues.. err... bugs)
    if (!GetProcAddress(g_dbghelp,"SymEnumSymbolsForAddr"))
      g_oldDbghelp=true;
  }
}

static void InitDbghelp()
{
  std::call_once(g_dbghelpInitOnce, InitDbghelpOnce);
}

//////////////////////////////////////////////////////////////////////////////

DebugStackwalk::Signature::Signature(const Signature &src)
{
  *this=src;
}

DebugStackwalk::Signature& DebugStackwalk::Signature::operator=(const Signature& src)
{
  if (&src!=this)
  {
    m_numAddr=src.m_numAddr;
    memcpy(m_addr,src.m_addr,m_numAddr*sizeof(*m_addr));
  }
  return *this;
}

DebugStackwalk::Signature::Address DebugStackwalk::Signature::GetAddress(int n) const
{
  DFAIL_IF_MSG(n<0||n>=MAX_ADDR,n << "/" << MAX_ADDR) return 0;
  return m_addr[n];
}

void DebugStackwalk::Signature::GetSymbol(Address addr, char *buf, unsigned bufSize)
{
  DFAIL_IF(!buf) return;
  DFAIL_IF(bufSize<64||bufSize>=0x80000000) return;

  InitDbghelp();
  std::lock_guard<std::recursive_mutex> dbghelpLock(g_dbghelpMutex);

  if (!g_dbghelp || !DEBUG_SYM_GET_MODULE_BASE || !DEBUG_SYM_GET_SYMBOL || !DEBUG_SYM_GET_LINE)
    return;

  char *bufEnd=buf+bufSize;
  *buf=0;
#if defined(_WIN64)
  buf+=wsprintf(buf,"%016I64x",static_cast<unsigned __int64>(addr));
#else
  buf+=wsprintf(buf,"%08x",static_cast<unsigned>(addr));
#endif

  // determine module
  DebugDwordAddress modBase=DEBUG_SYM_GET_MODULE_BASE(GetCurrentProcess(),static_cast<DebugDwordAddress>(addr));
  if (!modBase)
	{
		strcpy(buf," (unknown module)");
    return;
	}

  // illegal code ptr?
	if (IsBadReadPtr(reinterpret_cast<void *>(addr),4)||IsBadCodePtr(reinterpret_cast<FARPROC>(addr)))
	{
		strcpy(buf," (invalid code addr)");
		return;
	}

  char symbolBuffer[512];
  GetModuleFileName(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(modBase)),symbolBuffer,sizeof(symbolBuffer));

  char *p=strrchr(symbolBuffer,'\\'); // use filename only, strip off path
  p=p?p+1:symbolBuffer;
  *buf++=' ';
  strcpy(buf,p);
  buf+=strlen(buf);
  if (bufEnd-buf<32)
    return;
 #if defined(_WIN64)
  buf+=wsprintf(buf,"+0x%I64x",static_cast<unsigned __int64>(static_cast<DebugDwordAddress>(addr)-modBase));
 #else
  buf+=wsprintf(buf,"+0x%x",static_cast<unsigned>(addr)-modBase);
 #endif

  // determine symbol
  DebugSymbol *symPtr=reinterpret_cast<DebugSymbol *>(symbolBuffer);
  memset(symPtr,0,sizeof(symbolBuffer));
  symPtr->SizeOfStruct=sizeof(DebugSymbol);
  symPtr->MaxNameLength=sizeof(symbolBuffer)-sizeof(DebugSymbol);
  DebugDisplacement displacement;
  if (!DEBUG_SYM_GET_SYMBOL(GetCurrentProcess(),static_cast<DebugDwordAddress>(addr),&displacement,symPtr))
    return;
  if ((unsigned int)(bufEnd-buf)<strlen(symPtr->Name)+16)
    return;
 #if defined(_WIN64)
  buf+=wsprintf(buf,", %s+0x%I64x",symPtr->Name,static_cast<unsigned __int64>(displacement));
 #else
  buf+=wsprintf(buf,", %s+0x%x",symPtr->Name,displacement);
 #endif

  // and line number
  DebugLine line;
  memset(&line,0,sizeof(line));
  line.SizeOfStruct=sizeof(line);
  DWORD lineDisplacement;
  if (!DEBUG_SYM_GET_LINE(GetCurrentProcess(),static_cast<DebugDwordAddress>(addr),&lineDisplacement,&line))
    return;

  p=strrchr(line.FileName,'\\'); // use filename only, strip off path
  p=p?p+1:line.FileName;

  if ((unsigned int)(bufEnd-buf)<strlen(p)+16)
    return;
  buf+=wsprintf(buf,", %s:%i+0x%x",p,line.LineNumber,lineDisplacement);
}

void DebugStackwalk::Signature::GetSymbol(Address addr,
                                          char *bufMod, unsigned sizeMod, unsigned *relMod,
                                          char *bufSym, unsigned sizeSym, unsigned *relSym,
                                          char *bufFile, unsigned sizeFile, unsigned *linePtr, unsigned *relLine)
{
  InitDbghelp();
  std::lock_guard<std::recursive_mutex> dbghelpLock(g_dbghelpMutex);

  if (bufMod) *bufMod=0;
  if (relMod) *relMod=0;
  if (bufSym) *bufSym=0;
  if (relSym) *relSym=0;

  if (bufFile) *bufFile=0;
  if (linePtr) *linePtr=0;
  if (relLine) *relLine=0;

  DFAIL_IF(bufMod&&sizeMod<16) return;
  DFAIL_IF(bufSym&&sizeSym<16) return;
  DFAIL_IF(bufFile&&sizeFile<16) return;

  // determine module
  if (!g_dbghelp || !DEBUG_SYM_GET_MODULE_BASE)
    return;
  DebugDwordAddress modBase=DEBUG_SYM_GET_MODULE_BASE(GetCurrentProcess(),static_cast<DebugDwordAddress>(addr));
  if (!modBase)
	{
    if (bufMod)
		  strcpy(bufMod,"(unknown mod)");
    if (bufSym)
      strcpy(bufSym,"(unknown)");
    return;
	}

  // illegal code ptr?
	if (IsBadReadPtr(reinterpret_cast<void *>(addr),4)||IsBadCodePtr(reinterpret_cast<FARPROC>(addr)))
	{
    if (bufMod)
		  strcpy(bufMod,"(inv code addr)");
    if (bufSym)
      strcpy(bufSym,"(unknown)");
		return;
	}

  char symbolBuffer[512];
  if (bufMod)
  {
    GetModuleFileName(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(modBase)),symbolBuffer,sizeof(symbolBuffer));

    char *p=strrchr(symbolBuffer,'\\'); // use filename only, strip off path
    p=p?p+1:symbolBuffer;
    strlcpy(bufMod,p,sizeMod);
  }
  if (relMod)
    *relMod=static_cast<unsigned>(static_cast<DebugDwordAddress>(addr)-modBase);

  // determine symbol
  if (bufSym)
  {
    DebugSymbol *symPtr=reinterpret_cast<DebugSymbol *>(symbolBuffer);
    memset(symPtr,0,sizeof(symbolBuffer));
    symPtr->SizeOfStruct=sizeof(DebugSymbol);
    symPtr->MaxNameLength=sizeof(symbolBuffer)-sizeof(DebugSymbol);
    DebugDisplacement displacement;
    if (DEBUG_SYM_GET_SYMBOL(GetCurrentProcess(),static_cast<DebugDwordAddress>(addr),&displacement,symPtr))
    {
      strlcpy(bufSym,symPtr->Name,sizeSym);
      if (relSym)
        *relSym=static_cast<unsigned>(displacement);
    }
    else
      strcpy(bufSym,"(unknown)");
  }

  // and line number
  if (bufFile)
  {
    DebugLine line;
    memset(&line,0,sizeof(line));
    line.SizeOfStruct=sizeof(line);
    DWORD lineDisplacement;
    if (!DEBUG_SYM_GET_LINE(GetCurrentProcess(),static_cast<DebugDwordAddress>(addr),&lineDisplacement,&line))
      strcpy(bufFile,"(unknown)");
    else
    {
      char *p=strrchr(line.FileName,'\\'); // use filename only, strip off path
      p=p?p+1:line.FileName;
      strlcpy(bufFile,p,sizeFile);
      if (linePtr)
        *linePtr=line.LineNumber;
      if (relLine)
        *relLine=lineDisplacement;
    }
  }
}

Debug& operator<<(Debug &dbg, const DebugStackwalk::Signature &sig)
{
  dbg << sig.Size() << " addresses:\n";

  for (unsigned k=0;k<sig.Size();k++)
  {
    char buf[512];
    sig.GetSymbol(sig.GetAddress(k),buf,sizeof(buf));
    dbg << buf << "\n";
  }

  return dbg;
}

//////////////////////////////////////////////////////////////////////////////

DebugStackwalk::DebugStackwalk()
{
  // it doesn't harm to do this here
  InitDbghelp();
}

DebugStackwalk::~DebugStackwalk()
{
}

void *DebugStackwalk::GetDbghelpHandle()
{
  return g_dbghelp;
}

bool DebugStackwalk::IsOldDbghelp()
{
  return g_oldDbghelp;
}

int DebugStackwalk::StackWalk(Signature &sig, struct _CONTEXT *ctx)
{
  InitDbghelp();
  std::lock_guard<std::recursive_mutex> dbghelpLock(g_dbghelpMutex);

  sig.m_numAddr=0;

  // bail out if no stack walk available
#if defined(_WIN64)
  if (!gDbg._StackWalk64 || !gDbg._SymFunctionTableAccess64 || !gDbg._SymGetModuleBase64)
#else
  if (!gDbg._StackWalk || !gDbg._SymFunctionTableAccess || !gDbg._SymGetModuleBase)
#endif
    return 0;

	// Set up the stack frame structure for the start point of the stack walk (i.e. here).
#if defined(_WIN64)
	STACKFRAME64 stackFrame;
	CONTEXT stackContext;
	LPVOID contextRecord=&stackContext;
#else
	STACKFRAME stackFrame;
	LPVOID contextRecord=ctx;
#endif
	memset(&stackFrame,0,sizeof(stackFrame));

	stackFrame.AddrPC.Mode = AddrModeFlat;
	stackFrame.AddrStack.Mode = AddrModeFlat;
	stackFrame.AddrFrame.Mode = AddrModeFlat;

	// Use the context struct if it was provided.
	if (ctx)
  {
#if defined(_WIN64)
		// StackWalk64 may consume/update the context while unwinding. Work on
		// a copy so exception records supplied by callers remain untouched.
		stackContext=*ctx;
		stackContext.ContextFlags|=CONTEXT_CONTROL;
		stackFrame.AddrPC.Offset = stackContext.Rip;
		stackFrame.AddrStack.Offset = stackContext.Rsp;
		stackFrame.AddrFrame.Offset = stackContext.Rbp;
#else
		stackFrame.AddrPC.Offset = ctx->Eip;
		stackFrame.AddrStack.Offset = ctx->Esp;
		stackFrame.AddrFrame.Offset = ctx->Ebp;
#endif
	}
  else
  {
    // walk stack back using current call chain
#if defined(_WIN64)
	  CONTEXT currentContext;
	  RtlCaptureContext(&currentContext);
	  currentContext.ContextFlags|=CONTEXT_CONTROL;
	  stackContext=currentContext;
	  stackFrame.AddrPC.Offset = currentContext.Rip;
	  stackFrame.AddrStack.Offset = currentContext.Rsp;
	  stackFrame.AddrFrame.Offset = currentContext.Rbp;
#else
	  unsigned long reg_eip, reg_ebp, reg_esp;
#if defined(_MSC_VER)
	  __asm
    {
    here:
		  lea	eax,here
		  mov	reg_eip,eax
		  mov	reg_ebp,ebp
		  mov	reg_esp,esp
	  };
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(_M_IX86))
	  __asm__ __volatile__ (
		  "call 1f\n\t"
		  "1: pop %0\n\t"
		  "mov %%ebp, %1\n\t"
		  "mov %%esp, %2"
		  : "=r" (reg_eip), "=r" (reg_ebp), "=r" (reg_esp)
	  );
#else
#error "Unsupported compiler or architecture for register capture"
#endif
	  stackFrame.AddrPC.Offset = reg_eip;
	  stackFrame.AddrStack.Offset = reg_esp;
	  stackFrame.AddrFrame.Offset = reg_ebp;
#endif
  }

	// Walk the stack by the requested number of return address iterations.
  bool skipFirst=!ctx;
#if defined(_WIN64)
  while (sig.m_numAddr<Signature::MAX_ADDR&&
		     gDbg._StackWalk64(IMAGE_FILE_MACHINE_AMD64,GetCurrentProcess(),GetCurrentThread(),
                         &stackFrame,contextRecord,nullptr,gDbg._SymFunctionTableAccess64,gDbg._SymGetModuleBase64,nullptr))
#else
  while (sig.m_numAddr<Signature::MAX_ADDR&&
		     gDbg._StackWalk(IMAGE_FILE_MACHINE_I386,GetCurrentProcess(),GetCurrentThread(),
                         &stackFrame,contextRecord,nullptr,gDbg._SymFunctionTableAccess,gDbg._SymGetModuleBase,nullptr))
#endif
  {
    if (skipFirst)
      skipFirst=false;
    else
      sig.m_addr[sig.m_numAddr++]=static_cast<Signature::Address>(stackFrame.AddrPC.Offset);
  }

	return sig.m_numAddr;
}
