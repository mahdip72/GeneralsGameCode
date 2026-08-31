#pragma once

#include "Lib/BaseTypeCore.h"

/*
 * The projected-terrain kernel deliberately has no renderer or engine
 * dependency.  The owner captures height-map samples and triangle-flip bits
 * before admission; workers only read this immutable POD snapshot and write
 * disjoint vertex/index rows.
 */
enum ProjectedTerrainGridKind
{
	PROJECTED_TERRAIN_GRID_SHADOW = 0,
	PROJECTED_TERRAIN_GRID_DECAL = 1
};

struct ProjectedTerrainGridVertex
{
	Real x;
	Real y;
	Real z;
	unsigned diffuse;
	Real u;
	Real v;
};

struct ProjectedTerrainGridSnapshot
{
	unsigned width;
	unsigned height;
	unsigned cellWidth;
	unsigned cellHeight;
	const Real *heights;
	const UnsignedByte *flips;

	unsigned kind;
	Int firstMapX;
	Int firstMapY;
	Int coordinateBiasX;
	Int coordinateBiasY;
	Real mapXYFactor;
	Real mapHeightScale;

	UnsignedByte clampToLayerHeight;
	Real layerHeight;
	Real heightBias;
	unsigned diffuse;
	Real uAxisX;
	Real uAxisY;
	Real vAxisX;
	Real vAxisY;
	Real objectX;
	Real objectY;
	Real uOffset;
	Real vOffset;
};

enum
{
	PROJECTED_TERRAIN_GRID_MIN_PARALLEL_CELLS = 512,
	PROJECTED_TERRAIN_GRID_MAX_VERTICES = 65536,
	PROJECTED_TERRAIN_GRID_MAX_BYTES = 8u * 1024u * 1024u
};

/* Owner-side reusable bounded scratch.  No pointer returned by this object
 * may outlive the synchronous preparation call that borrowed it. */
class ProjectedTerrainGridScratch
{
public:
	ProjectedTerrainGridScratch();
	~ProjectedTerrainGridScratch();

	/* Allocate a complete vertex/cell snapshot transactionally. */
	bool ensure(unsigned width, unsigned height);
	void reset();

	bool isAllocated() const
	{
		return m_vertices != 0 && m_indices != 0 && m_heights != 0 &&
			m_width != 0 && m_height != 0;
	}
	unsigned width() const { return m_width; }
	unsigned height() const { return m_height; }
	Real *heights() { return m_heights; }
	UnsignedByte *flips() { return m_flips; }
	ProjectedTerrainGridVertex *vertices() { return m_vertices; }
	UnsignedShort *indices() { return m_indices; }

private:
	ProjectedTerrainGridScratch(const ProjectedTerrainGridScratch &);
	ProjectedTerrainGridScratch &operator=(const ProjectedTerrainGridScratch &);

	unsigned m_width;
	unsigned m_height;
	Real *m_heights;
	UnsignedByte *m_flips;
	ProjectedTerrainGridVertex *m_vertices;
	UnsignedShort *m_indices;
};

/* Structural and owner-capture validation.  This does not inspect any live
 * engine or renderer object. */
bool ValidateProjectedTerrainGridInput(
	const ProjectedTerrainGridSnapshot &snapshot,
	const ProjectedTerrainGridVertex *vertices,
	const UnsignedShort *indices);

/* Write vertex rows [rowBegin,rowEnd).  Cell indices for those rows are also
 * written, with each cell row owning one exclusive output range. */
bool PrepareProjectedTerrainGridRows(
	const ProjectedTerrainGridSnapshot &snapshot,
	ProjectedTerrainGridVertex *vertices,
	UnsignedShort *indices,
	unsigned rowBegin, unsigned rowEnd);

/* Validate all output before the owner copies bytes into a D3D lock. */
bool ValidatePreparedProjectedTerrainGridOutput(
	const ProjectedTerrainGridSnapshot &snapshot,
	const ProjectedTerrainGridVertex *vertices,
	const UnsignedShort *indices);
