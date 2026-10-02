#include "Utility/CppMacros.h"
#include "line3d.h"
#include "nativeLine3dTransform.h"
#include "nativew3dline.h"
#include "ww3d.h"

#include <new>

namespace
{
bool ReleaseNativeLineBuffers(void *&bufferStorage,
	void *&submitterStorage,
	rts::render::NativeLine3DSubmitter *publishedSubmitter)
{
	rts::render::NativeLine3DSubmitter *submitter =
		static_cast<rts::render::NativeLine3DSubmitter *>(submitterStorage);
	rts::render::NativeLine3DBufferSet *buffers =
		static_cast<rts::render::NativeLine3DBufferSet *>(bufferStorage);
	if (buffers != 0)
	{
		// Drop the Line3D reference before trying the owner release.  A worker
		// destructor must not delete the sidecar: a registered context retains it
		// and retries the exact handles on the render owner thread.
		if (buffers->lineOwned)
		{
			buffers->lineReferenceActive = false;
		}
		const bool contextOwns = buffers->contextReferenceActive;
		bool released = false;
		if (contextOwns)
		{
			// Never dereference a replaced or destroyed context.  The old context
			// remains the cleanup owner through its registry, even though this line
			// object no longer exists.
			released = submitter != 0 && submitter == publishedSubmitter &&
				submitter->ReleaseLine3D(buffers);
			bufferStorage = 0;
			submitterStorage = 0;
			return released;
		}
		// A context that already drained or reached terminal shutdown has either
		// invalidated the handles or explicitly transferred physical ownership to
		// the resource table.  Reclaim only this metadata locally in that case.
		if (buffers->resourceOwnerTerminal ||
			(!buffers->vertexBuffer.isValid() &&
				!buffers->indexBuffer.isValid()))
		{
			delete buffers;
			bufferStorage = 0;
			submitterStorage = 0;
			return true;
		}
		if (submitter != 0 && submitter == publishedSubmitter)
		{
			released = submitter->ReleaseLine3D(buffers);
			if (released && buffers->lineOwned)
			{
				delete buffers;
				bufferStorage = 0;
				submitterStorage = 0;
				return true;
			}
		}
		// Keep a non-terminal, still-valid sidecar attached when no owner can
		// release it.  This is only reachable for a custom submitter that did not
		// register the set; the production context always takes the branch above.
		return released;
	}
	else
	{
		bufferStorage = 0;
		submitterStorage = 0;
	}
	return true;
}
}

Line3DClass::Line3DClass(const Vector3 &start, const Vector3 &end,
	float width, float r, float g, float b, float opacity)
{
	NativeLine3DBuffers = 0;
	NativeLine3DSubmitterPtr = 0;
	Length = (end - start).Length();
	Width = width;

	const float halfWidth = Width * 0.5f;
	vert[0].X = 0.0f;
	vert[0].Y = -halfWidth;
	vert[0].Z = -halfWidth;
	vert[1].X = 0.0f;
	vert[1].Y = halfWidth;
	vert[1].Z = -halfWidth;
	vert[2].X = 0.0f;
	vert[2].Y = -halfWidth;
	vert[2].Z = halfWidth;
	vert[3].X = 0.0f;
	vert[3].Y = halfWidth;
	vert[3].Z = halfWidth;
	vert[4].X = Length;
	vert[4].Y = -halfWidth;
	vert[4].Z = -halfWidth;
	vert[5].X = Length;
	vert[5].Y = halfWidth;
	vert[5].Z = -halfWidth;
	vert[6].X = Length;
	vert[6].Y = -halfWidth;
	vert[6].Z = halfWidth;
	vert[7].X = Length;
	vert[7].Y = halfWidth;
	vert[7].Z = halfWidth;

	Color.X = r;
	Color.Y = g;
	Color.Z = b;
	Set_Opacity(opacity);

	Matrix3D transform(true);
	transform.Obj_Look_At(start, end, 0.0f);
	Set_Transform(transform);
}

Line3DClass::Line3DClass(const Line3DClass &src) :
	RenderObjClass(src),
	Length(src.Length),
	Width(src.Width),
	Shader(src.Shader),
	Color(src.Color),
	SortLevel(src.SortLevel),
	NativeLine3DBuffers(0),
	NativeLine3DSubmitterPtr(0)
{
	for (int i = 0; i < 8; ++i)
	{
		vert[i] = src.vert[i];
	}
}

Line3DClass &Line3DClass::operator=(const Line3DClass &that)
{
	WWASSERT(0);
	rts::render::NativeLine3DSubmitterScope submitterScope;
	(void)ReleaseNativeLineBuffers(NativeLine3DBuffers,
		NativeLine3DSubmitterPtr, submitterScope.Get());
	RenderObjClass::operator=(that);
	if (this != &that)
	{
		Length = that.Length;
		Width = that.Width;
		Shader = that.Shader;
		Color = that.Color;
		SortLevel = that.SortLevel;
		for (int i = 0; i < 8; ++i)
		{
			vert[i] = that.vert[i];
		}
	}
	return *this;
}

Line3DClass::~Line3DClass()
{
	rts::render::NativeLine3DSubmitterScope submitterScope;
	(void)ReleaseNativeLineBuffers(NativeLine3DBuffers,
		NativeLine3DSubmitterPtr, submitterScope.Get());
}

RenderObjClass *Line3DClass::Clone() const
{
	return NEW_REF(Line3DClass, (*this));
}

void Line3DClass::Render(RenderInfoClass &rinfo)
{
	(void)rinfo;
	if (!Is_Not_Hidden_At_All())
	{
		return;
	}
	const unsigned int sortLevel = (unsigned int)Get_Sort_Level();
	if (WW3D::Are_Static_Sort_Lists_Enabled() && sortLevel != 0)
	{
		WW3D::Add_To_Static_Sort_List(this, sortLevel);
		return;
	}
	rts::render::NativeLine3DSubmitterScope submitterScope;
	rts::render::NativeLine3DSubmitter *submitter =
		submitterScope.Get();
	if (submitter == 0)
	{
		return;
	}
	rts::render::NativeLine3DSubmitter *previousSubmitter =
		static_cast<rts::render::NativeLine3DSubmitter *>(
			NativeLine3DSubmitterPtr);
	rts::render::NativeLine3DBufferSet *buffers =
		static_cast<rts::render::NativeLine3DBufferSet *>(NativeLine3DBuffers);
	if (buffers != 0 && previousSubmitter != submitter)
	{
		// Do not discard cached handles when publication changes.  The old
		// owner may be in a terminal transition and must retain the exact cache
		// for deferred cleanup.  A new owner cannot safely reuse those handles.
		return;
	}
	if (buffers == 0)
	{
		buffers = new (std::nothrow) rts::render::NativeLine3DBufferSet;
		if (buffers == 0)
		{
			return;
		}
		buffers->lineOwned = true;
		buffers->lineReferenceActive = true;
		NativeLine3DBuffers = buffers;
	}
	NativeLine3DSubmitterPtr = submitter;
	rts::render::NativeLine3DGeometry geometry;
	if (!Build_Native_Geometry(&geometry))
	{
		return;
	}
	float worldMatrix[16];
	if (!rts::render::Build_Native_Line3D_World_Matrix(Transform,
		worldMatrix))
	{
		return;
	}
	rts::render::LegacyLogicalState baseState;
	const bool hasBaseState =
		rts::render::GetTrackedLegacyLogicalState(&baseState);
	rts::render::LegacyLogicalState state;
	if (!rts::render::Build_Native_Line3D_State(Shader.Get_Bits(),
		hasBaseState ? &baseState : 0, worldMatrix, &state))
	{
		return;
	}
	const rts::render::RenderResult submitResult =
		submitter->SubmitLine3D(geometry, state, buffers);
	if (submitResult != rts::render::RENDER_RESULT_OK)
	{
		// NativeW3DLineContext records this as a frame failure.  Keep the
		// caller-side branch explicit as well: a failed submission is never
		// treated as a successful draw or silently retried with stale state.
		return;
	}
}

bool Line3DClass::Build_Native_Geometry(
	rts::render::NativeLine3DGeometry *geometry) const
{
	float positions[rts::render::NATIVE_LINE3D_VERTEX_COUNT * 3];
	for (unsigned int vertex = 0;
		vertex < rts::render::NATIVE_LINE3D_VERTEX_COUNT; ++vertex)
	{
		const unsigned int position = vertex * 3;
		positions[position] = vert[vertex].X;
		positions[position + 1] = vert[vertex].Y;
		positions[position + 2] = vert[vertex].Z;
	}
	const rts::render::RenderFloat4 color(Color.X, Color.Y, Color.Z,
		Color.W);
	return rts::render::Build_Native_Line3D_Geometry(positions,
		rts::render::NATIVE_LINE3D_VERTEX_COUNT, color, geometry);
}

void Line3DClass::Scale(float scale)
{
	for (int i = 0; i < 8; ++i)
	{
		vert[i] *= scale;
	}
	Length *= scale;
	Width *= scale;
	Invalidate_Cached_Bounding_Volumes();
	RenderObjClass *container = Get_Container();
	if (container != 0)
	{
		container->Update_Obj_Space_Bounding_Volumes();
	}
}

void Line3DClass::Scale(float scalex, float scaley, float scalez)
{
	Vector3 scale(scalex, scaley, scalez);
	for (int i = 0; i < 8; ++i)
	{
		vert[i].Scale(scale);
	}
	Length *= scalex;
	Width *= scaley;
	Invalidate_Cached_Bounding_Volumes();
	RenderObjClass *container = Get_Container();
	if (container != 0)
	{
		container->Update_Obj_Space_Bounding_Volumes();
	}
}

void Line3DClass::Get_Obj_Space_Bounding_Sphere(SphereClass &sphere) const
{
	const float halfLength = Length * 0.5f;
	sphere.Center.Set(halfLength, 0.0f, 0.0f);
	sphere.Radius = halfLength;
}

void Line3DClass::Get_Obj_Space_Bounding_Box(AABoxClass &box) const
{
	const float halfLength = Length * 0.5f;
	box.Center.Set(halfLength, 0.0f, 0.0f);
	box.Extent.Set(halfLength, 0.0f, 0.0f);
}

void Line3DClass::Reset(const Vector3 &newStart, const Vector3 &newEnd)
{
	float newLength = (newEnd - newStart).Length();
	if (newLength == 0.0f)
	{
		newLength = 0.001f;
	}
	Scale(newLength / Length, 1.0f, 1.0f);
	Length = newLength;
	Matrix3D transform(true);
	transform.Obj_Look_At(newStart, newEnd, 0.0f);
	Set_Transform(transform);
	Invalidate_Cached_Bounding_Volumes();
	RenderObjClass *container = Get_Container();
	if (container != 0)
	{
		container->Update_Obj_Space_Bounding_Volumes();
	}
}

void Line3DClass::Reset(const Vector3 &newStart, const Vector3 &newEnd,
	float newWidth)
{
	float newLength = (newEnd - newStart).Length();
	if (newLength == 0.0f)
	{
		newLength = 0.001f;
	}
	const float widthScale = newWidth / Width;
	Scale(newLength / Length, widthScale, widthScale);
	Length = newLength;
	Width = newWidth;
	Matrix3D transform(true);
	transform.Obj_Look_At(newStart, newEnd, 0.0f);
	Set_Transform(transform);
	Invalidate_Cached_Bounding_Volumes();
	RenderObjClass *container = Get_Container();
	if (container != 0)
	{
		container->Update_Obj_Space_Bounding_Volumes();
	}
}

void Line3DClass::Re_Color(float r, float g, float b)
{
	Color = Vector4(r, g, b, Color.W);
}

void Line3DClass::Set_Opacity(float opacity)
{
	Shader.Reset();
	if (opacity < 1.0f)
	{
		Shader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);
		Shader.Set_Src_Blend_Func(ShaderClass::SRCBLEND_SRC_ALPHA);
		Shader.Set_Dst_Blend_Func(ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}
	Set_Sort_Level(opacity < 1.0f ? 1 : 0);
	Color.W = opacity;
}

int Line3DClass::Get_Num_Polys() const
{
	return 12;
}
