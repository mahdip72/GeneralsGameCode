/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

// Native-only experiment telemetry. Nothing is written during shadow submission.
// An orderly process shutdown publishes at most 1024 aggregate render rows.
#if defined(_WIN64)
#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <string.h>

namespace rts { namespace render {
struct ShadowStreamReuseRow
{
	unsigned int epoch, vertexCapacity, indexCapacity, reuseEnabled;
	__int64 beginQpc, endQpc;
	unsigned __int64 firstTasks, firstVertices, firstIndices, firstUploads, firstBytes;
	unsigned __int64 firstVertexDiscards, firstIndexDiscards;
	unsigned __int64 secondTasks, eligible, hits, misses, secondUploads, secondBytes;
	unsigned __int64 secondVertexDiscards, secondIndexDiscards, reusedBytes, failures, oversized;
	unsigned __int64 missDisabled, missProof, missGeometry, missTransform, missVertexGeneration, missIndexGeneration, missRange;
	unsigned int diagnosticFlags, closed;
};

class ShadowStreamReuseDiagnostics
{
public:
	static ShadowStreamReuseDiagnostics &instance()
	{
		static ShadowStreamReuseDiagnostics diagnostics;
		return diagnostics;
	}
	ShadowStreamReuseRow *begin(unsigned int epoch, unsigned int vertices,
		unsigned int indices, bool reuse)
	{
		if (!m_enabled) return 0;
		if (m_rows == RowLimit) { ++m_dropped; return 0; }
		ShadowStreamReuseRow *row=&m_data[m_rows++];
		memset(row, 0, sizeof(*row));
		row->epoch=epoch; row->vertexCapacity=vertices; row->indexCapacity=indices;
		row->reuseEnabled=reuse ? 1 : 0;
		LARGE_INTEGER now; now.QuadPart=0;
		if (!QueryPerformanceCounter(&now) || now.QuadPart<=0)
			row->diagnosticFlags|=InvalidQpc;
		else
			row->beginQpc=now.QuadPart;
		return row;
	}
	static void end(ShadowStreamReuseRow *row)
	{
		if (!row) return;
		if (row->closed) { row->diagnosticFlags|=InvalidInterval; return; }
		LARGE_INTEGER now; now.QuadPart=0;
		if (!QueryPerformanceCounter(&now) || now.QuadPart<=0)
			row->diagnosticFlags|=InvalidQpc;
		else
		{
			row->endQpc=now.QuadPart;
			if (row->beginQpc<=0 || row->endQpc<=row->beginQpc)
				row->diagnosticFlags|=InvalidInterval;
		}
		row->closed=1;
	}
private:
	enum { RowLimit=1024 };
	enum { InvalidQpc=1, InvalidInterval=2 };
	ShadowStreamReuseRow m_data[RowLimit];
	unsigned int m_rows, m_dropped;
	bool m_enabled;
	char m_directory[MAX_PATH];
	__int64 m_frequency;
	ShadowStreamReuseDiagnostics() : m_rows(0), m_dropped(0), m_enabled(false), m_frequency(0)
	{
		DWORD length=GetEnvironmentVariableA("RTS_SHADOW_STREAM_REUSE_DIAGNOSTICS_DIR",
			m_directory, sizeof(m_directory));
		if (!length || length>=sizeof(m_directory)) return;
		DWORD attributes=GetFileAttributesA(m_directory);
		LARGE_INTEGER frequency;
		if (attributes==INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
			!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0) return;
		m_frequency=frequency.QuadPart; m_enabled=true;
	}
	~ShadowStreamReuseDiagnostics()
	{
		if (!m_enabled) return;
		bool invalidQpc=false, invalidInterval=false, unclosedRow=false;
		for (unsigned int i=0; i<m_rows; ++i)
		{
			if (m_data[i].diagnosticFlags & InvalidQpc) invalidQpc=true;
			if (m_data[i].diagnosticFlags & InvalidInterval) invalidInterval=true;
			if (!m_data[i].closed) unclosedRow=true;
		}
		const bool publishable=m_rows!=0 && !invalidQpc && !invalidInterval && !unclosedRow;
		const char *captureStatus="complete";
		if (m_rows==0) captureStatus="empty_capture";
		else if (invalidQpc) captureStatus="invalid_qpc";
		else if (invalidInterval) captureStatus="invalid_interval";
		else if (unclosedRow) captureStatus="unclosed_row";
		else if (m_dropped) captureStatus="truncated";
		char path[MAX_PATH+100], publishedPath[MAX_PATH+100];
		_snprintf(path, sizeof(path), "%s\\shadow-stream-reuse-%lu-%lu.csv",
			m_directory, GetCurrentProcessId(), GetTickCount());
		path[sizeof(path)-1]=0;
		strcpy(publishedPath,path);
		strcat(path,".pending");
		FILE *file=_fsopen(path, "w+x", _SH_DENYWR);
		if (!file) return; // No artifact means capture unavailable, never a passing result.
		setvbuf(file, 0, _IOFBF, 16384);
		bool success=fprintf(file, "epoch,qpc_begin,qpc_end,qpc_frequency,vertex_capacity,index_capacity,reuse_enabled,activity_key,first_tasks,first_vertices,first_indices,first_uploads,first_bytes,first_vb_discards,first_ib_discards,second_tasks,eligible,hits,misses,second_uploads,second_bytes,second_vb_discards,second_ib_discards,reused_bytes,failures,oversized,miss_disabled,miss_proof,miss_geometry,miss_transform,miss_vb_generation,miss_ib_generation,miss_range\n")>=0;
		for (unsigned int i=0; i<m_rows && success; ++i)
		{
			const ShadowStreamReuseRow &r=m_data[i];
			success=fprintf(file, "%u,%lld,%lld,%lld,%u,%u,%u,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
				r.epoch,r.beginQpc,r.endQpc,m_frequency,r.vertexCapacity,r.indexCapacity,r.reuseEnabled,
				r.firstTasks ? "dynamic_stream" : "no_dynamic_stream_work",
				r.firstTasks,r.firstVertices,r.firstIndices,r.firstUploads,r.firstBytes,r.firstVertexDiscards,r.firstIndexDiscards,
				r.secondTasks,r.eligible,r.hits,r.misses,r.secondUploads,r.secondBytes,r.secondVertexDiscards,r.secondIndexDiscards,
				r.reusedBytes,r.failures,r.oversized,r.missDisabled,r.missProof,r.missGeometry,r.missTransform,
				r.missVertexGeneration,r.missIndexGeneration,r.missRange)>=0;
		}
		if (fflush(file)!=0) success=false;
		if (fprintf(file, "# end,rows=%u,dropped=%u,status=%s\n",m_rows,m_dropped,
			!success ? "write_failed" : captureStatus)<0) success=false;
		if (fflush(file)!=0) success=false;
		if (fclose(file)!=0) success=false;
		// A final .csv exists only after every write/flush/close succeeded.
		// Invalid rows also stay pending, so no incomplete capture is published as valid.
		if (success && publishable) (void)MoveFileA(path,publishedPath);
	}
	ShadowStreamReuseDiagnostics(const ShadowStreamReuseDiagnostics &);
	ShadowStreamReuseDiagnostics &operator=(const ShadowStreamReuseDiagnostics &);
};

class ShadowStreamReuseDiagnosticsFrame
{
public:
	ShadowStreamReuseDiagnosticsFrame(ShadowStreamReuseRow *&active,
		unsigned int epoch, unsigned int vertices, unsigned int indices, bool reuse)
		: m_active(active)
	{
		m_active=ShadowStreamReuseDiagnostics::instance().begin(epoch,vertices,indices,reuse);
	}
	~ShadowStreamReuseDiagnosticsFrame()
	{
		ShadowStreamReuseDiagnostics::end(m_active); m_active=0;
	}
private:
	ShadowStreamReuseRow *&m_active;
	ShadowStreamReuseDiagnosticsFrame(const ShadowStreamReuseDiagnosticsFrame &);
	ShadowStreamReuseDiagnosticsFrame &operator=(const ShadowStreamReuseDiagnosticsFrame &);
};
} }
#endif

