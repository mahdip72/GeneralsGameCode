/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include <windows.h>
#include <stdio.h>
#include <string.h>

enum TestQpcMode
{
	TEST_QPC_NORMAL,
	TEST_QPC_FAIL_BEGIN,
	TEST_QPC_ZERO_BEGIN,
	TEST_QPC_FAIL_END,
	TEST_QPC_ZERO_END,
	TEST_QPC_END_BEFORE_BEGIN
};

static TestQpcMode testQpcMode=TEST_QPC_NORMAL;
static unsigned int testQpcCalls=0;
static BOOL WINAPI ShadowStreamReuseDiagnosticsTestQpc(LARGE_INTEGER *value);

// Keep the production helper intact while giving this fixture deterministic
// coverage of Win32 QPC failure and invalid-value paths.
#define QueryPerformanceCounter ShadowStreamReuseDiagnosticsTestQpc
#include "Lib/ShadowStreamReuseDiagnostics.h"
#undef QueryPerformanceCounter

static BOOL WINAPI ShadowStreamReuseDiagnosticsTestQpc(LARGE_INTEGER *value)
{
	const unsigned int call=testQpcCalls++;
	value->QuadPart=100000+call;
	if (testQpcMode==TEST_QPC_FAIL_BEGIN && call==0)
	{
		value->QuadPart=0;
		return FALSE;
	}
	if (testQpcMode==TEST_QPC_ZERO_BEGIN && call==0)
	{
		value->QuadPart=0;
		return TRUE;
	}
	if (testQpcMode==TEST_QPC_FAIL_END && call==1)
	{
		value->QuadPart=0;
		return FALSE;
	}
	if (testQpcMode==TEST_QPC_ZERO_END && call==1)
	{
		value->QuadPart=0;
		return TRUE;
	}
	if (testQpcMode==TEST_QPC_END_BEFORE_BEGIN)
		value->QuadPart=call==0 ? 200 : call==1 ? 199 : 100000+call;
	return TRUE;
}

static bool SelectMode(const char *name, bool *unclosed, bool *truncated, bool *empty)
{
	*unclosed=false;
	*truncated=false;
	*empty=false;
	if (strcmp(name,"empty")==0) { *empty=true; return true; }
	if (strcmp(name,"complete")==0) return true;
	if (strcmp(name,"truncated")==0) { *truncated=true; return true; }
	if (strcmp(name,"qpc_fail_begin_truncated")==0)
	{
		testQpcMode=TEST_QPC_FAIL_BEGIN; *truncated=true; return true;
	}
	if (strcmp(name,"qpc_fail_begin")==0) { testQpcMode=TEST_QPC_FAIL_BEGIN; return true; }
	if (strcmp(name,"qpc_zero_begin")==0) { testQpcMode=TEST_QPC_ZERO_BEGIN; return true; }
	if (strcmp(name,"qpc_fail_end")==0) { testQpcMode=TEST_QPC_FAIL_END; return true; }
	if (strcmp(name,"qpc_zero_end")==0) { testQpcMode=TEST_QPC_ZERO_END; return true; }
	if (strcmp(name,"end_before_begin")==0) { testQpcMode=TEST_QPC_END_BEFORE_BEGIN; return true; }
	if (strcmp(name,"unclosed")==0) { *unclosed=true; return true; }
	return false;
}

static void FillCounters(rts::render::ShadowStreamReuseRow *row)
{
	row->firstTasks=3; row->firstVertices=128; row->firstIndices=384;
	row->firstUploads=3; row->firstBytes=2304;
	row->secondTasks=3; row->eligible=2; row->hits=2; row->misses=1;
	row->secondUploads=1; row->secondBytes=768; row->reusedBytes=1536;
	row->missVertexGeneration=1;
}

int main(int argc, char **argv)
{
	bool unclosed, truncated, empty;
	if (argc!=3 || !SelectMode(argv[2],&unclosed,&truncated,&empty) ||
		!SetEnvironmentVariableA("RTS_SHADOW_STREAM_REUSE_DIAGNOSTICS_DIR",argv[1])) return 1;
	rts::render::ShadowStreamReuseDiagnostics &capture=rts::render::ShadowStreamReuseDiagnostics::instance();
	if (!empty && truncated)
	{
		for (unsigned int i=0; i<1024; ++i)
		{
			rts::render::ShadowStreamReuseRow *row=capture.begin(1000+i,16384,49152,true);
			if (!row) return 2;
			FillCounters(row);
			capture.end(row);
			if (!row->closed) return 3;
			if (i==0 && testQpcMode==TEST_QPC_FAIL_BEGIN)
			{
				if (!row->diagnosticFlags) return 4;
			}
			else if (row->diagnosticFlags) return 5;
		}
		if (capture.begin(2024,16384,49152,true)!=0) return 6;
		if (capture.begin(2025,16384,49152,true)!=0) return 7;
	}
	else if (!empty)
	{
		rts::render::ShadowStreamReuseRow *row=capture.begin(1000,16384,49152,true);
		if (!row) return 6;
		FillCounters(row);
		if (!unclosed) capture.end(row);
	}
	// The verifier reads the ACTUAL shutdown artifact from this process.
	printf("capture_pid=%lu\n",GetCurrentProcessId());
	return 0;
}
