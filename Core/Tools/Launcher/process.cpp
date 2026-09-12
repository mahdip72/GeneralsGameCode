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

#include "process.h"

#include <string.h>

Process::Process()
{
  directory[0]=0;
  command[0]=0;
  args[0]=0;
  hProcess=nullptr;
  dwProcessID=0;
  hThread=nullptr;
  dwThreadID=0;
}

bit8 Append_Process_Arguments(Process &process, int argc, char *argv[])
{
    const size_t argsCapacity = sizeof(process.args);
    size_t currentLength = 0;

    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        return FALSE;
    }

    while (currentLength < argsCapacity && process.args[currentLength] != 0) {
        ++currentLength;
    }
    if (currentLength >= argsCapacity) {
        return FALSE;
    }

    // Build into a bounded temporary so an overflow cannot leave a partially
    // appended command line in the Process object.
    char composedArgs[sizeof(process.args)];
    memcpy(composedArgs, process.args, currentLength + 1);
    size_t composedLength = currentLength;
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == nullptr) {
            return FALSE;
        }

        size_t argumentLength = 0;
        while (argumentLength < argsCapacity && argv[i][argumentLength] != 0) {
            ++argumentLength;
        }
        if (argumentLength >= argsCapacity) {
            return FALSE;
        }

        const size_t availableLength = (argsCapacity - 1) - composedLength;
        if (availableLength < 1 || argumentLength > availableLength - 1) {
            return FALSE;
        }

        composedArgs[composedLength++] = ' ';
        memcpy(composedArgs + composedLength, argv[i], argumentLength);
        composedLength += argumentLength;
    }
    composedArgs[composedLength] = 0;
    memcpy(process.args, composedArgs, composedLength + 1);
    return TRUE;
}

bit8 Close_Process(Process &process)
{
    bit8 result = TRUE;
    if (process.hThread != nullptr) {
        if (CloseHandle(process.hThread) == 0) {
            result = FALSE;
        }
        process.hThread = nullptr;
    }
    if (process.hProcess != nullptr) {
        if (CloseHandle(process.hProcess) == 0) {
            result = FALSE;
        }
        process.hProcess = nullptr;
    }
    process.dwProcessID = 0;
    process.dwThreadID = 0;
    return result;
}

// Create a process
bit8 Create_Process(Process &process)
{
    int                      retval;
    STARTUPINFO              si;
    PROCESS_INFORMATION      piProcess;
    Close_Process(process);
    ZeroMemory(&si,sizeof(si));
    ZeroMemory(&piProcess,sizeof(piProcess));
    si.cb=sizeof(si);

    char cmdargs[513];
    memset(cmdargs,0,513);
    strcpy(cmdargs,process.command);
    strcat(cmdargs,process.args);

    DBGMSG("PROCESS CMD="<<cmdargs<<"  DIR="<<process.directory);

    retval=CreateProcess(nullptr,cmdargs,nullptr,nullptr,FALSE, 0  ,nullptr, nullptr/*process.directory*/,&si,&piProcess);

    DBGMSG("("<<retval<<") New process:  HANDLE " << (void *)piProcess.hProcess << "   ID "
      << (DWORD)piProcess.dwProcessId);
    DBGMSG("("<<retval<<") New thread:  HANDLE " << (void *)piProcess.hThread << "   ID "
      << (DWORD)piProcess.dwThreadId);
    if (retval==0)
    {
      char message_buffer[256];
	   FormatMessage( FORMAT_MESSAGE_FROM_SYSTEM, nullptr, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), &message_buffer[0], 256, nullptr );
      DBGMSG("ERR: "<<message_buffer);
      return(FALSE);
    }
    process.hProcess=piProcess.hProcess;
		process.dwProcessID = piProcess.dwProcessId;
		process.dwThreadID = piProcess.dwThreadId;
    if (CloseHandle(piProcess.hThread) == 0) {
      Close_Process(process);
      return(FALSE);
    }
    process.hThread=nullptr;
    return(TRUE);
}

//
// Wait for a process to complete, and fill in the exit code
//
bit8 Wait_Process(Process &process, DWORD *exit_code)
{
  if (exit_code != nullptr)
    *exit_code=-1;
  if (process.hProcess == nullptr)
    return(FALSE);

  DWORD retval=WaitForSingleObject(process.hProcess,INFINITE);
  bit8 result=FALSE;
  if (retval==WAIT_OBJECT_0)  // process exited
  {
		// MDC 1/10/2003 Inserting sleep here to let game exit before applying patch
		Sleep(3000);

    if (exit_code == nullptr || GetExitCodeProcess(process.hProcess,exit_code) != 0)
      result=TRUE;
  }
  if (Close_Process(process) == FALSE)
    result=FALSE;
  return(result);
}


//
// Get the process to run from the config object
//
#ifndef RTS_LAUNCHER_PROCESS_ONLY
bit8 Read_Process_Info(ConfigFile &config,OUT Process &info, IN const char *key)
{

 Wstring keyStr = "RUN";
 if (key)
	 keyStr = key;

 Wstring procinfo;
 if (config.getString(keyStr.get(), procinfo)==FALSE)
 {
   DBGMSG("Couldn't read the RUN line");
   return(FALSE);
 }
 int          offset=0;
 Wstring      dir;
 Wstring      executable;
 Wstring      args;
 offset=procinfo.getToken(offset," ",dir);
 offset=procinfo.getToken(offset," ",executable);
 args=procinfo;
 args.remove(0,offset);

 ///
 ///
 DBGMSG("RUN: EXE = "<<executable.get()<<"  DIR = "<<dir.get()<<
   "  ARGS = "<<args.get());
 strcpy(info.command,executable.get());
 strcpy(info.directory,dir.get());
 strcpy(info.args,args.get());
 return(TRUE);


/*********************************************************
  FILE     *in;
  char      string[256];
  bit8      found_space;
  int       i;

  if ((in=fopen(config,"r"))==nullptr)
    return(FALSE);

  while(fgets(string,256,in))
  {
    i=0;
    while ((isspace(string[i]))&&(i<int(strlen(string))))
      i++;

    // do nothing with empty line or commented line
    if ((string[i]=='#')||(i==int(strlen(string)-1)))
      continue;

    strcpy(info.directory,string);
    found_space=FALSE;
    for (; i<int(strlen(string)); i++)
    {
      if (isspace(info.directory[i]))
      {
        info.directory[i]=0;
        found_space=TRUE;
      }
      else if (found_space)
        break;
    }

    strcpy(info.command,string+i);
    for (i=0; i<int(strlen(info.command)); i++)
      if ((info.command[i]=='\n')||(info.command[i]=='\r'))
        info.command[i]=0;

    //printf("DIR = '%s'  CMD = '%s'\n",info.directory,info.command);

    // We only have 1 process for this
    break;
  }
  fclose(in);
  return(TRUE);
**********************************************/
}
#endif
