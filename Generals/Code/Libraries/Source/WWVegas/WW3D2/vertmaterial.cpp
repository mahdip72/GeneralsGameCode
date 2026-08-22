/*
**	Command & Conquer Generals(tm)
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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/vertmaterial.cpp                       $*
 *                                                                                             *
 *                       Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                     $Modtime:: 8/22/01 11:06a                                              $*
 *                                                                                             *
 *                    $Revision:: 42                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 *   Init -- init code                                                                         *
 *   Shutdown -- shutdown code                                                                 *
 *   Get_Preset -- retrieve presets                                                            *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "vertmaterial.h"
#include "WWLib/realcrc.h"
#include "WWDebug/wwdebug.h"
#include "w3d_util.h"
#include "WWLib/chunkio.h"
#include "w3derr.h"
#include "WWLib/INI.h"
#include "WWLib/XSTRAW.h"
#include "dx8wrapper.h"


static unsigned int unique=1;

VertexMaterialClass* VertexMaterialClass::Presets[VertexMaterialClass::PRESET_COUNT];

/*
** VertexMaterialClass Implementation
*/
VertexMaterialClass::VertexMaterialClass():
	Flags(0),
	AmbientColorSource(rts::render::RENDER_MATERIAL_SOURCE_MATERIAL),
	EmissiveColorSource(rts::render::RENDER_MATERIAL_SOURCE_MATERIAL),
	DiffuseColorSource(rts::render::RENDER_MATERIAL_SOURCE_MATERIAL),
	UseLighting(false),
	UniqueID(0),
	CRCDirty(true)
{
	int i;

	for (i=0; i<MeshBuilderClass::MAX_STAGES; i++)
	{
		Mapper[i]=nullptr;
		UVSource[i] = i;
	}
	// The historical descriptor was zeroed before the default setters, which
	// leaves diffuse and ambient alpha at zero. Preserve that detail in the
	// neutral state before the default opacity setter below.
	Material.diffuse.w = 0.0f;
	Material.ambient.w = 0.0f;
	Set_Ambient(1.0f,1.0f,1.0f);
	Set_Diffuse(1.0f,1.0f,1.0f);

	Set_Opacity(1.0f);
}

VertexMaterialClass::VertexMaterialClass(const VertexMaterialClass & src) :
	Material(src.Material),
	Flags(src.Flags),
	AmbientColorSource(src.AmbientColorSource),
	EmissiveColorSource(src.EmissiveColorSource),
	DiffuseColorSource(src.DiffuseColorSource),
	UseLighting(src.UseLighting),
	Name(src.Name),
	UniqueID(src.UniqueID),
	CRCDirty(true)
{
	int i;
	for (i=0; i<MeshBuilderClass::MAX_STAGES; i++)
	{
		Mapper[i]=nullptr;
		if (src.Mapper[i])
		{
			TextureMapperClass *mapper=src.Mapper[i]->Clone();
			Set_Mapper(mapper,i);
			mapper->Release_Ref();
		}

		UVSource[i] = src.UVSource[i];
	}
}

void VertexMaterialClass::Make_Unique()
{
	CRCDirty=true;
	UniqueID=unique;
	unique++;
}

VertexMaterialClass::~VertexMaterialClass()
{
	int i;

	for (i=0; i<MeshBuilderClass::MAX_STAGES; i++)
	{
		if (Mapper[i])
		{
			REF_PTR_RELEASE(Mapper[i]);
			Mapper[i]=nullptr;
		}
	}
}

VertexMaterialClass & VertexMaterialClass::operator = (const VertexMaterialClass &src)
{

	if (this != &src) {
		Name=src.Name;
		Flags = src.Flags;
		AmbientColorSource = src.AmbientColorSource;
		EmissiveColorSource = src.EmissiveColorSource;
		DiffuseColorSource = src.DiffuseColorSource;
		UseLighting=src.UseLighting;
		UniqueID=src.UniqueID;
		CRCDirty=src.CRCDirty;
		int stage;
		for (stage=0;stage<MeshBuilderClass::MAX_STAGES;++stage) {
			if (Mapper[stage] != nullptr) {
				Mapper[stage]->Release_Ref();
				Mapper[stage] = nullptr;
			}
		}
		for (stage=0;stage<MeshBuilderClass::MAX_STAGES;++stage) {
			if (src.Mapper[stage]) {
				TextureMapperClass *mapper = src.Mapper[stage]->Clone();
				Set_Mapper(mapper,stage);
				mapper->Release_Ref();
			}
			UVSource[stage] = src.UVSource[stage];
		}

		Material = src.Material;
	}
	return *this;
}

unsigned long VertexMaterialClass::Compute_CRC() const
{
	unsigned long crc = 0;

// don't include the name when determining whether two vertex materials match
//	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(Name.Peek_Buffer()),sizeof(char)*strlen(Name),crc);

	const float material_values[] = {
		Material.diffuse.x, Material.diffuse.y, Material.diffuse.z, Material.diffuse.w,
		Material.ambient.x, Material.ambient.y, Material.ambient.z, Material.ambient.w,
		Material.specular.x, Material.specular.y, Material.specular.z, Material.specular.w,
		Material.emissive.x, Material.emissive.y, Material.emissive.z, Material.emissive.w,
		Material.specularPower
	};
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(material_values),
		sizeof(material_values),crc);
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&Flags),sizeof(Flags),crc);
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&DiffuseColorSource),sizeof(DiffuseColorSource),crc);
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&AmbientColorSource),sizeof(AmbientColorSource),crc);
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&EmissiveColorSource),sizeof(EmissiveColorSource),crc);
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&UVSource),sizeof(UVSource),crc);
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&UseLighting),sizeof(UseLighting),crc);
	crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&UniqueID),sizeof(UniqueID),crc);

	int i;
	for (i=0; i<MeshBuilderClass::MAX_STAGES; i++)
	{
		if (Mapper[i]) crc = CRC_Memory(reinterpret_cast<const unsigned char *>(&(Mapper[i])),sizeof(TextureMapperClass*),crc);
	}

	return crc;
}

// Ambient Get and Sets

void VertexMaterialClass::Get_Ambient(Vector3 * set) const
{
	assert(set);
	*set=Vector3(Material.ambient.x,Material.ambient.y,Material.ambient.z);
}

void VertexMaterialClass::Set_Ambient(const Vector3 & color)
{
	CRCDirty=true;
	Material.ambient.x=color.X;
	Material.ambient.y=color.Y;
	Material.ambient.z=color.Z;
}

void VertexMaterialClass::Set_Ambient(float r,float g,float b)
{
	CRCDirty=true;
	Material.ambient.x=r;
	Material.ambient.y=g;
	Material.ambient.z=b;
}

// Diffuse Get and Sets

void VertexMaterialClass::Get_Diffuse(Vector3 * set) const
{
	assert(set);
	*set=Vector3(Material.diffuse.x,Material.diffuse.y,Material.diffuse.z);
}

void VertexMaterialClass::Set_Diffuse(const Vector3 & color)
{
	CRCDirty=true;
	Material.diffuse.x=color.X;
	Material.diffuse.y=color.Y;
	Material.diffuse.z=color.Z;
}

void VertexMaterialClass::Set_Diffuse(float r,float g,float b)
{
	CRCDirty=true;
	Material.diffuse.x=r;
	Material.diffuse.y=g;
	Material.diffuse.z=b;
}

// Specular Get and Sets

void VertexMaterialClass::Get_Specular(Vector3 * set) const
{
	assert(set);
	*set=Vector3(Material.specular.x,Material.specular.y,Material.specular.z);
}

void VertexMaterialClass::Set_Specular(const Vector3 & color)
{
	CRCDirty=true;
	Material.specular.x=color.X;
	Material.specular.y=color.Y;
	Material.specular.z=color.Z;
}

void VertexMaterialClass::Set_Specular(float r,float g,float b)
{
	CRCDirty=true;
	Material.specular.x=r;
	Material.specular.y=g;
	Material.specular.z=b;
}

// Emissive Get and Sets

void VertexMaterialClass::Get_Emissive(Vector3 * set) const
{
	assert(set);
	*set=Vector3(Material.emissive.x,Material.emissive.y,Material.emissive.z);
}

void VertexMaterialClass::Set_Emissive(const Vector3 & color)
{
	CRCDirty=true;
	Material.emissive.x=color.X;
	Material.emissive.y=color.Y;
	Material.emissive.z=color.Z;
}

void VertexMaterialClass::Set_Emissive(float r,float g,float b)
{
	CRCDirty=true;
	Material.emissive.x=r;
	Material.emissive.y=g;
	Material.emissive.z=b;
}


float	VertexMaterialClass::Get_Shininess() const
{
	return Material.specularPower;
}

void	VertexMaterialClass::Set_Shininess(float shin)
{
	CRCDirty=true;
	Material.specularPower=shin;
}

float	VertexMaterialClass::Get_Opacity() const
{
	return Material.diffuse.w;
}

void	VertexMaterialClass::Set_Opacity(float o)
{
	CRCDirty=true;
	Material.diffuse.w=o;
}

void	VertexMaterialClass::Set_Ambient_Color_Source(ColorSourceType src)
{
	CRCDirty=true;
	switch (src)
	{
	case	COLOR1:		AmbientColorSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR1; break;
	case	COLOR2:		AmbientColorSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR2; break;
	default:				AmbientColorSource = rts::render::RENDER_MATERIAL_SOURCE_MATERIAL; break;
	}
}

void	VertexMaterialClass::Set_Emissive_Color_Source(ColorSourceType src)
{
	CRCDirty=true;
	switch (src)
	{
	case	COLOR1:		EmissiveColorSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR1; break;
	case	COLOR2:		EmissiveColorSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR2; break;
	default:				EmissiveColorSource = rts::render::RENDER_MATERIAL_SOURCE_MATERIAL; break;
	}
}

void	VertexMaterialClass::Set_Diffuse_Color_Source(ColorSourceType src)
{
	CRCDirty=true;
	switch (src)
	{
	case	COLOR1:		DiffuseColorSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR1; break;
	case	COLOR2:		DiffuseColorSource = rts::render::RENDER_MATERIAL_SOURCE_COLOR2; break;
	default:				DiffuseColorSource = rts::render::RENDER_MATERIAL_SOURCE_MATERIAL; break;
	}
}

VertexMaterialClass::ColorSourceType
VertexMaterialClass::Get_Ambient_Color_Source()
{
	switch(AmbientColorSource)
	{
	case rts::render::RENDER_MATERIAL_SOURCE_COLOR1:	return COLOR1;
	case rts::render::RENDER_MATERIAL_SOURCE_COLOR2:	return COLOR2;
	default:					return MATERIAL;
	}
}

VertexMaterialClass::ColorSourceType
VertexMaterialClass::Get_Emissive_Color_Source()
{
	switch(EmissiveColorSource)
	{
	case rts::render::RENDER_MATERIAL_SOURCE_COLOR1:	return COLOR1;
	case rts::render::RENDER_MATERIAL_SOURCE_COLOR2:	return COLOR2;
	default:					return MATERIAL;
	}
}

VertexMaterialClass::ColorSourceType
VertexMaterialClass::Get_Diffuse_Color_Source()
{
	switch(DiffuseColorSource)
	{
	case rts::render::RENDER_MATERIAL_SOURCE_COLOR1:	return COLOR1;
	case rts::render::RENDER_MATERIAL_SOURCE_COLOR2:	return COLOR2;
	default:					return MATERIAL;
	}
}

void VertexMaterialClass::Set_UV_Source(int stage,int array_index)
{
	WWASSERT(stage >= 0);
	WWASSERT(stage < MeshBuilderClass::MAX_STAGES);
	WWASSERT(array_index >= 0);
	WWASSERT(array_index < 8);
	CRCDirty=true;
	UVSource[stage] = array_index;
}

int VertexMaterialClass::Get_UV_Source(int stage)
{
	WWASSERT(stage >= 0);
	WWASSERT(stage < MeshBuilderClass::MAX_STAGES);
	return UVSource[stage];
}


void VertexMaterialClass::Init_From_Material3(const W3dMaterial3Struct & mat3)
{
	Vector3 tmp0,tmp1,tmp2;

	W3dUtilityClass::Convert_Color(mat3.DiffuseColor,&tmp0);
	W3dUtilityClass::Convert_Color(mat3.DiffuseCoefficients,&tmp1);
	tmp2.X = tmp0.X * tmp1.X;
	tmp2.Y = tmp0.Y * tmp1.Y;
	tmp2.Z = tmp0.Z * tmp1.Z;
	Set_Diffuse(tmp2);

	W3dUtilityClass::Convert_Color(mat3.SpecularColor,&tmp0);
	W3dUtilityClass::Convert_Color(mat3.SpecularCoefficients,&tmp1);
	tmp2.X = tmp0.X * tmp1.X;
	tmp2.Y = tmp0.Y * tmp1.Y;
	tmp2.Z = tmp0.Z * tmp1.Z;
	Set_Specular(tmp2);

	W3dUtilityClass::Convert_Color(mat3.EmissiveCoefficients,&tmp0);
	Set_Emissive(tmp0);

	W3dUtilityClass::Convert_Color(mat3.AmbientCoefficients,&tmp0);
	Set_Ambient(tmp0);

	Set_Shininess(mat3.Shininess);
	Set_Opacity(mat3.Opacity);
}

WW3DErrorType VertexMaterialClass::Load_W3D(ChunkLoadClass & cload)
{
	char name[256];

	W3dVertexMaterialStruct vmat;
	bool hasname = false;

	char *mapping0_arg_buffer = nullptr;
	char *mapping1_arg_buffer = nullptr;
	unsigned int mapping0_arg_len = 0U;
	unsigned int mapping1_arg_len = 0U;

	while (cload.Open_Chunk()) {
		switch (cload.Cur_Chunk_ID()) {
			case W3D_CHUNK_VERTEX_MATERIAL_NAME:
				cload.Read(&name,cload.Cur_Chunk_Length());
				hasname = true;
				break;

			case W3D_CHUNK_VERTEX_MATERIAL_INFO:
				if (cload.Read(&vmat,sizeof(vmat)) != sizeof(vmat)) {
					return WW3D_ERROR_LOAD_FAILED;
				}
				break;

			case W3D_CHUNK_VERTEX_MAPPER_ARGS0:
				mapping0_arg_len = cload.Cur_Chunk_Length();
				mapping0_arg_buffer = MSGW3DNEWARRAY("VertexMaterialClassTemp") char[mapping0_arg_len];
				if (cload.Read(mapping0_arg_buffer, mapping0_arg_len) != mapping0_arg_len) {
					return WW3D_ERROR_LOAD_FAILED;
				}
				break;

			case W3D_CHUNK_VERTEX_MAPPER_ARGS1:
				mapping1_arg_len = cload.Cur_Chunk_Length();
				mapping1_arg_buffer = MSGW3DNEWARRAY("VertexMaterialClassTemp") char[mapping1_arg_len];
				if (cload.Read(mapping1_arg_buffer, mapping1_arg_len) != mapping1_arg_len) {
					return WW3D_ERROR_LOAD_FAILED;
				}
				break;
		};
		cload.Close_Chunk();
	}

	if (hasname) {
		Set_Name(name);
	}

	// Read an INIClass from the mapping argument buffer - this will be used
	// to initialize any special mappers used.
	INIClass mapping0_arg_ini;
	if (mapping0_arg_buffer) {

		char *extended_arg_buffer = MSGW3DNEWARRAY("VertexMaterialClassTemp") char[mapping0_arg_len + 10];
		snprintf(extended_arg_buffer, mapping0_arg_len + 10, "[Args]\n%s", mapping0_arg_buffer);
		mapping0_arg_len = strlen(extended_arg_buffer) + 1;

		delete [] mapping0_arg_buffer;
		mapping0_arg_buffer = nullptr;

		BufferStraw map_arg_buf_straw((void *)extended_arg_buffer, mapping0_arg_len);

		mapping0_arg_ini.Load(map_arg_buf_straw);

		delete [] extended_arg_buffer;
		extended_arg_buffer = nullptr;
	}
	INIClass mapping1_arg_ini;
	if (mapping1_arg_buffer) {

		char *extended_arg_buffer = MSGW3DNEWARRAY("VertexMaterialClassTemp") char[mapping1_arg_len + 20];
		snprintf(extended_arg_buffer, mapping1_arg_len + 20, "[Args]\n%s", mapping1_arg_buffer);
		mapping1_arg_len = strlen(extended_arg_buffer) + 1;

		delete [] mapping1_arg_buffer;
		mapping1_arg_buffer = nullptr;

		BufferStraw map_arg_buf_straw((void *)extended_arg_buffer, mapping1_arg_len);

		mapping1_arg_ini.Load(map_arg_buf_straw);

		delete [] extended_arg_buffer;
		extended_arg_buffer = nullptr;
	}

	if (vmat.Attributes & W3DVERTMAT_USE_DEPTH_CUE) {
		Set_Flag(VertexMaterialClass::DEPTH_CUE,true);
	}

	if (vmat.Attributes & W3DVERTMAT_COPY_SPECULAR_TO_DIFFUSE) {
		Set_Flag(VertexMaterialClass::COPY_SPECULAR_TO_DIFFUSE,true);
	}

	// Set up the vertex mapper.  If it is one of the simple
	// ones, set the pointer to one of the global instances.
	int mapping = vmat.Attributes & W3DVERTMAT_STAGE0_MAPPING_MASK;

	switch(mapping) {

		case W3DVERTMAT_STAGE0_MAPPING_UV:
			break;

		case W3DVERTMAT_STAGE0_MAPPING_ENVIRONMENT:
			{
				EnvironmentMapperClass *mapper = NEW_REF(EnvironmentMapperClass,(0));
				Set_Mapper(mapper);
				mapper->Release_Ref();
			}
			break;
		case W3DVERTMAT_STAGE0_MAPPING_CHEAP_ENVIRONMENT:
			{
				ClassicEnvironmentMapperClass *mapper = NEW_REF(ClassicEnvironmentMapperClass,(0));
				Set_Mapper(mapper);
				mapper->Release_Ref();
			}
			break;
		case W3DVERTMAT_STAGE0_MAPPING_LINEAR_OFFSET:
			{
				LinearOffsetTextureMapperClass *mapper =
					NEW_REF(LinearOffsetTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_SCREEN:
			{
				ScreenMapperClass *mapper =
					NEW_REF(ScreenMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_SCALE:
			{
				ScaleTextureMapperClass *mapper =
					NEW_REF(ScaleTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_GRID:
			{
				GridTextureMapperClass *mapper =
					NEW_REF(GridTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_ROTATE:
			{
				RotateTextureMapperClass *mapper =
					NEW_REF(RotateTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_SINE_LINEAR_OFFSET:
			{
				SineLinearOffsetTextureMapperClass *mapper =
					NEW_REF(SineLinearOffsetTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_STEP_LINEAR_OFFSET:
			{
				StepLinearOffsetTextureMapperClass *mapper =
					NEW_REF(StepLinearOffsetTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_ZIGZAG_LINEAR_OFFSET:
			{
				ZigZagLinearOffsetTextureMapperClass *mapper =
					NEW_REF(ZigZagLinearOffsetTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_WS_CLASSIC_ENV:
			{
				WSClassicEnvironmentMapperClass *mapper = NEW_REF(WSClassicEnvironmentMapperClass,(0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_WS_ENVIRONMENT:
			{
				WSEnvironmentMapperClass *mapper = NEW_REF(WSEnvironmentMapperClass,(0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_GRID_CLASSIC_ENV:
			{
				GridClassicEnvironmentMapperClass *mapper =
					NEW_REF(GridClassicEnvironmentMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_GRID_ENVIRONMENT:
			{
				GridEnvironmentMapperClass *mapper =
					NEW_REF(GridEnvironmentMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_RANDOM:
			{
				RandomTextureMapperClass *mapper =
					NEW_REF(RandomTextureMapperClass,(mapping0_arg_ini, "Args", 0));
				Set_Mapper(mapper,0);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_EDGE:
		{
			EdgeMapperClass *mapper =
				NEW_REF(EdgeMapperClass,(mapping0_arg_ini, "Args", 0));
			Set_Mapper(mapper,0);
			mapper->Release_Ref();
		}
		break;

		case W3DVERTMAT_STAGE0_MAPPING_BUMPENV:
		{
			BumpEnvTextureMapperClass *mapper =
				NEW_REF(BumpEnvTextureMapperClass,(mapping0_arg_ini, "Args", 0));
			Set_Mapper(mapper,0);
			mapper->Release_Ref();
		}
		break;

		default:
				WWDEBUG_SAY(("Unsupported mapper in %s",name));
			break;
	}

	// Same setup for stage 1's mapper.
	mapping = vmat.Attributes & W3DVERTMAT_STAGE1_MAPPING_MASK;
	switch(mapping) {

		case W3DVERTMAT_STAGE1_MAPPING_UV:
			break;

		case W3DVERTMAT_STAGE1_MAPPING_ENVIRONMENT:
		{
			EnvironmentMapperClass *mapper = W3DNEW EnvironmentMapperClass(1);
			Set_Mapper(mapper, 1);
			mapper->Release_Ref();
		}
		break;
		case W3DVERTMAT_STAGE1_MAPPING_CHEAP_ENVIRONMENT:
		{
			ClassicEnvironmentMapperClass *mapper = W3DNEW ClassicEnvironmentMapperClass(1);
			Set_Mapper(mapper, 1);
			mapper->Release_Ref();
		}
		break;

		case W3DVERTMAT_STAGE1_MAPPING_LINEAR_OFFSET:
		{
			LinearOffsetTextureMapperClass *mapper =
				W3DNEW LinearOffsetTextureMapperClass(mapping1_arg_ini, "Args", 1);
			Set_Mapper(mapper, 1);
			mapper->Release_Ref();
		}
		break;

		case W3DVERTMAT_STAGE1_MAPPING_SCREEN:
		{
			ScreenMapperClass *mapper =
				W3DNEW ScreenMapperClass(mapping1_arg_ini, "Args", 1);
			Set_Mapper(mapper, 1);
			mapper->Release_Ref();
		}
		break;

		case W3DVERTMAT_STAGE1_MAPPING_SCALE:
			{
				ScaleTextureMapperClass *mapper =
					NEW_REF(ScaleTextureMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_GRID:
			{
				GridTextureMapperClass *mapper =
					NEW_REF(GridTextureMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_ROTATE:
			{
				RotateTextureMapperClass *mapper =
					NEW_REF(RotateTextureMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_SINE_LINEAR_OFFSET:
			{
				SineLinearOffsetTextureMapperClass *mapper =
					NEW_REF(SineLinearOffsetTextureMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_STEP_LINEAR_OFFSET:
			{
				StepLinearOffsetTextureMapperClass *mapper =
					NEW_REF(StepLinearOffsetTextureMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_ZIGZAG_LINEAR_OFFSET:
			{
				ZigZagLinearOffsetTextureMapperClass *mapper =
					NEW_REF(ZigZagLinearOffsetTextureMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_WS_CLASSIC_ENV:
			{
				WSClassicEnvironmentMapperClass *mapper = NEW_REF(WSClassicEnvironmentMapperClass,(1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_WS_ENVIRONMENT:
			{
				WSEnvironmentMapperClass *mapper = NEW_REF(WSEnvironmentMapperClass,(1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_GRID_CLASSIC_ENV:
			{
				GridClassicEnvironmentMapperClass *mapper =
					NEW_REF(GridClassicEnvironmentMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_GRID_ENVIRONMENT:
			{
				GridEnvironmentMapperClass *mapper =
					NEW_REF(GridEnvironmentMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_RANDOM:
			{
				RandomTextureMapperClass *mapper =
					NEW_REF(RandomTextureMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE1_MAPPING_EDGE:
			{
				EdgeMapperClass *mapper =
					NEW_REF(EdgeMapperClass,(mapping1_arg_ini, "Args", 1));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		case W3DVERTMAT_STAGE0_MAPPING_BUMPENV:
			{
				BumpEnvTextureMapperClass *mapper =
					NEW_REF(BumpEnvTextureMapperClass,(mapping1_arg_ini, "Args", 0));
				Set_Mapper(mapper,1);
				mapper->Release_Ref();
			}
			break;

		default:
			WWDEBUG_SAY(("Unsupported mapper in %s",name));
			break;
	}

	Vector3 tmp;
	W3dUtilityClass::Convert_Color(vmat.Ambient,&tmp);
	Set_Ambient(tmp);

	W3dUtilityClass::Convert_Color(vmat.Diffuse,&tmp);
	Set_Diffuse(tmp);

	W3dUtilityClass::Convert_Color(vmat.Specular,&tmp);
	Set_Specular(tmp);

	W3dUtilityClass::Convert_Color(vmat.Emissive,&tmp);
	Set_Emissive(tmp);

	Set_Shininess(vmat.Shininess);
	Set_Opacity(vmat.Opacity);

	return WW3D_ERROR_OK;
}


WW3DErrorType VertexMaterialClass::Save_W3D(ChunkSaveClass & csave)
{
	WWASSERT(0);
	return WW3D_ERROR_OK;
}

void VertexMaterialClass::Apply() const
{
	rts::render::LegacyVertexMaterialState state;
	state.material = Material;
	state.lightingEnable = WW3D::Is_Coloring_Enabled() ? false : UseLighting;
	state.ambientMaterialSource = AmbientColorSource;
	state.diffuseMaterialSource = DiffuseColorSource;
	state.emissiveMaterialSource = EmissiveColorSource;

	// set to default values if no mappers
	for (unsigned int i=0; i<MeshBuilderClass::MAX_STAGES; i++) {
		if (Mapper[i]) {
			state.textureCoordinateIndex[i] = UVSource[i];
		} else {
			state.textureCoordinateIndex[i] = UVSource[i];
			state.textureStageResetMask |= 1U << i;
		}
	}
	DX8Wrapper::Set_Legacy_Vertex_Material(state);
	for (unsigned int i=0; i<MeshBuilderClass::MAX_STAGES; i++) {
		if (Mapper[i]) Mapper[i]->Apply(UVSource[i]);
	}
}

void VertexMaterialClass::Apply_Null()
{
	DX8Wrapper::Set_Legacy_Vertex_Material_Null();
}


/***********************************************************************************************
 * Init -- init code                                                                           *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/14/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void VertexMaterialClass::Init()
{
	int i;
	for (i=0; i<PRESET_COUNT;i++)
		Presets[i]=NEW_REF(VertexMaterialClass,());

	// Set up presets
	Presets[PRELIT_DIFFUSE]->Set_Diffuse_Color_Source(VertexMaterialClass::COLOR1);
	Presets[PRELIT_DIFFUSE]->Set_Lighting(false);
	Presets[PRELIT_NODIFFUSE]->Set_Lighting(false);
}


/***********************************************************************************************
 * Shutdown -- shutdown code                                                                   *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/14/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void VertexMaterialClass::Shutdown()
{
	int i;
	for (i=0; i<PRESET_COUNT;i++)
		REF_PTR_RELEASE(Presets[i]);
}


/***********************************************************************************************
 * Get_Preset -- retrieve presets                                                              *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   2/14/2001  hy : Created.                                                                  *
 *=============================================================================================*/
VertexMaterialClass * VertexMaterialClass::Get_Preset(PresetType type)
{
	WWASSERT(type<PRESET_COUNT);
	Presets[type]->Add_Ref();
	return Presets[type];
}
