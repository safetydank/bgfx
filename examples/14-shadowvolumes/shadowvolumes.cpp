/*
 * Copyright 2013 Dario Manesku. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

namespace std { namespace tr1 {} }
using namespace std::tr1;

#include "common.h"

#include <bgfx.h>
#include <bx/timer.h>
#include <bx/readerwriter.h>
#include "entry/entry.h"
#include "fpumath.h"
#include "imgui/imgui.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <map>

uint32_t packUint32(uint8_t _x, uint8_t _y, uint8_t _z, uint8_t _w)
{
	union
	{
		uint32_t ui32;
		uint8_t arr[4];
	} un;

	un.arr[0] = _x;
	un.arr[1] = _y;
	un.arr[2] = _z;
	un.arr[3] = _w;

	return un.ui32;
}

uint32_t packF4u(float _x, float _y = 0.0f, float _z = 0.0f, float _w = 0.0f)
{
	const uint8_t xx = uint8_t(_x*127.0f + 128.0f);
	const uint8_t yy = uint8_t(_y*127.0f + 128.0f);
	const uint8_t zz = uint8_t(_z*127.0f + 128.0f);
	const uint8_t ww = uint8_t(_w*127.0f + 128.0f);
	return packUint32(xx, yy, zz, ww);
}

struct PosNormalTexcoordVertex
{
	float    m_x;
	float    m_y;
	float    m_z;
	uint32_t m_normal;
	float    m_u;
	float    m_v;
};

static const float s_texcoord = 50.0f;
static const uint32_t s_numHPlaneVertices = 4;
static PosNormalTexcoordVertex s_hplaneVertices[s_numHPlaneVertices] =
{
	{ -1.0f, 0.0f,  1.0f, packF4u(0.0f, 1.0f, 0.0f), s_texcoord, s_texcoord },
	{  1.0f, 0.0f,  1.0f, packF4u(0.0f, 1.0f, 0.0f), s_texcoord, 0.0f       },
	{ -1.0f, 0.0f, -1.0f, packF4u(0.0f, 1.0f, 0.0f), 0.0f,       s_texcoord },
	{  1.0f, 0.0f, -1.0f, packF4u(0.0f, 1.0f, 0.0f), 0.0f,       0.0f       },
};

static const uint32_t s_numVPlaneVertices = 4;
static PosNormalTexcoordVertex s_vplaneVertices[s_numVPlaneVertices] =
{
	{ -1.0f,  1.0f, 0.0f, packF4u(0.0f, 0.0f, -1.0f), 1.0f, 1.0f },
	{  1.0f,  1.0f, 0.0f, packF4u(0.0f, 0.0f, -1.0f), 1.0f, 0.0f },
	{ -1.0f, -1.0f, 0.0f, packF4u(0.0f, 0.0f, -1.0f), 0.0f, 1.0f },
	{  1.0f, -1.0f, 0.0f, packF4u(0.0f, 0.0f, -1.0f), 0.0f, 0.0f },
};

static const uint32_t s_numPlaneIndices = 6;
static const uint16_t s_planeIndices[s_numPlaneIndices] =
{
	0, 1, 2,
	1, 3, 2,
};

//-------------------------------------------------
// Helper functions
//-------------------------------------------------

static const char* s_shaderPath = NULL;
static bool s_flipV = false;

static uint32_t s_clearMask = 0;
static uint32_t s_viewMask = 0;
static uint32_t s_rtMask = 0;

static bgfx::UniformHandle u_texColor;
static bgfx::UniformHandle u_texStencil;
static bgfx::RenderTargetHandle s_stencilRt;

inline uint32_t uint32_max(uint32_t _a, uint32_t _b)
{
	return _a > _b ? _a : _b;
}

static void shaderFilePath(char* _out, const char* _name)
{
	strcpy(_out, s_shaderPath);
	strcat(_out, _name);
	strcat(_out, ".bin");
}

long int fsize(FILE* _file)
{
	long int pos = ftell(_file);
	fseek(_file, 0L, SEEK_END);
	long int size = ftell(_file);
	fseek(_file, pos, SEEK_SET);
	return size;
}

static const bgfx::Memory* load(const char* _filePath)
{
	FILE* file = fopen(_filePath, "rb");
	if (NULL != file)
	{
		uint32_t size = (uint32_t)fsize(file);
		const bgfx::Memory* mem = bgfx::alloc(size+1);
		size_t ignore = fread(mem->data, 1, size, file);
		BX_UNUSED(ignore);
		fclose(file);
		mem->data[mem->size-1] = '\0';
		return mem;
	}

	return NULL;
}

static const bgfx::Memory* loadShader(const char* _name)
{
	char filePath[512];
	shaderFilePath(filePath, _name);
	return load(filePath);
}

static const bgfx::Memory* loadTexture(const char* _name)
{
	char filePath[512];
	strcpy(filePath, "textures/");
	strcat(filePath, _name);
	return load(filePath);
}

static bgfx::ProgramHandle loadProgram(const char* _vsName, const char* _fsName)
{
	const bgfx::Memory* mem;

	// Load vertex shader.
	mem = loadShader(_vsName);
	bgfx::VertexShaderHandle vsh = bgfx::createVertexShader(mem);

	// Load fragment shader.
	mem = loadShader(_fsName);
	bgfx::FragmentShaderHandle fsh = bgfx::createFragmentShader(mem);

	// Create program from shaders.
	bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh);

	// We can destroy vertex and fragment shader here since
	// their reference is kept inside bgfx after calling createProgram.
	// Vertex and fragment shader will be destroyed once program is
	// destroyed.
	bgfx::destroyVertexShader(vsh);
	bgfx::destroyFragmentShader(fsh);

	return program;
}

//-------------------------------------------------
// Math
//-------------------------------------------------

void mtxScaleRotateTranslate(float* _result
						   , const float _scaleX
						   , const float _scaleY
						   , const float _scaleZ
						   , const float _rotX
						   , const float _rotY
						   , const float _rotZ
						   , const float _translateX
						   , const float _translateY
						   , const float _translateZ
						   )
{
	float mtxRotateTranslate[16];
	float mtxScale[16];

	mtxRotateXYZ(mtxRotateTranslate, _rotX, _rotY, _rotZ);
	mtxRotateTranslate[12] = _translateX;
	mtxRotateTranslate[13] = _translateY;
	mtxRotateTranslate[14] = _translateZ;

	memset(mtxScale, 0, 16*sizeof(float));
	mtxScale[0]  = _scaleX;
	mtxScale[5]  = _scaleY;
	mtxScale[10] = _scaleZ;
	mtxScale[15] = 1.0f;

	mtxMul(_result, mtxScale, mtxRotateTranslate);
}

void mtxReflected(float*__restrict _result
				, const float* __restrict _p /* plane */
				, const float* __restrict _n /* normal */
				)
{
	float dot = vec3Dot(_p, _n);

	_result[ 0] =  1.0f -  2.0f * _n[0] * _n[0]; //1-2Nx^2
	_result[ 1] = -2.0f * _n[0] * _n[1];         //-2*Nx*Ny
	_result[ 2] = -2.0f * _n[0] * _n[2];         //-2*NxNz
	_result[ 3] = 0.0f;                          //0

	_result[ 4] = -2.0f * _n[0] * _n[1];         //-2*NxNy
	_result[ 5] =  1.0f -  2.0f * _n[1] * _n[1]; //1-2*Ny^2
	_result[ 6] = -2.0f * _n[1] * _n[2];         //-2*NyNz
	_result[ 7] = 0.0f;                          //0

	_result[ 8] = -2.0f * _n[0] * _n[2];         //-2*NxNz
	_result[ 9] = -2.0f * _n[1] * _n[2];         //-2NyNz
	_result[10] =  1.0f -  2.0f * _n[2] * _n[2]; //1-2*Nz^2
	_result[11] = 0.0f;                          //0

	_result[12] = 2.0f * dot * _n[0];            //2*dot*Nx
	_result[13] = 2.0f * dot * _n[1];            //2*dot*Ny
	_result[14] = 2.0f * dot * _n[2];            //2*dot*Nz
	_result[15] = 1.0f;                          //1
}

void mtxShadow(float* __restrict _result
			 , const float* __restrict _ground
			 , const float* __restrict _light
			 )
{
	float dot = _ground[0] * _light[0] +
		_ground[1] * _light[1] +
		_ground[2] * _light[2] +
		_ground[3] * _light[3];

	_result[ 0] =  dot - _light[0] * _ground[0];
	_result[ 1] = 0.0f - _light[1] * _ground[0];
	_result[ 2] = 0.0f - _light[2] * _ground[0];
	_result[ 3] = 0.0f - _light[3] * _ground[0];

	_result[ 4] = 0.0f - _light[0] * _ground[1];
	_result[ 5] =  dot - _light[1] * _ground[1];
	_result[ 6] = 0.0f - _light[2] * _ground[1];
	_result[ 7] = 0.0f - _light[3] * _ground[1];

	_result[ 8] = 0.0f - _light[0] * _ground[2];
	_result[ 9] = 0.0f - _light[1] * _ground[2];
	_result[10] =  dot - _light[2] * _ground[2];
	_result[11] = 0.0f - _light[3] * _ground[2];

	_result[12] = 0.0f - _light[0] * _ground[3];
	_result[13] = 0.0f - _light[1] * _ground[3];
	_result[14] = 0.0f - _light[2] * _ground[3];
	_result[15] =  dot - _light[3] * _ground[3];
}

void mtxBillboard(float* __restrict _result
				, const float* __restrict _view
				, const float* __restrict _pos
				, const float* __restrict _scale
				)
{
	_result[ 0] = _view[0]  * _scale[0];
	_result[ 1] = _view[4]  * _scale[0];
	_result[ 2] = _view[8]  * _scale[0];
	_result[ 3] = 0.0f;
	_result[ 4] = _view[1]  * _scale[1];
	_result[ 5] = _view[5]  * _scale[1];
	_result[ 6] = _view[9]  * _scale[1];
	_result[ 7] = 0.0f;
	_result[ 8] = _view[2]  * _scale[2];
	_result[ 9] = _view[6]  * _scale[2];
	_result[10] = _view[10] * _scale[2];
	_result[11] = 0.0f;
	_result[12] = _pos[0];
	_result[13] = _pos[1];
	_result[14] = _pos[2];
	_result[15] = 1.0f;
}

void planeNormal(float* __restrict _result
		, const float* __restrict _v0
		, const float* __restrict _v1
		, const float* __restrict _v2
		)
{
	float vec0[3], vec1[3];
	float cross[3];

	vec0[0] = _v1[0] - _v0[0];
	vec0[1] = _v1[1] - _v0[1];
	vec0[2] = _v1[2] - _v0[2];

	vec1[0] = _v2[0] - _v1[0];
	vec1[1] = _v2[1] - _v1[1];
	vec1[2] = _v2[2] - _v1[2];

	vec3Cross(cross, vec0, vec1);
	vec3Norm(_result, cross);

	_result[3] = -vec3Dot(_result, _v0);
}

//-------------------------------------------------
// Uniforms
//-------------------------------------------------

struct Uniforms
{
	void init()
	{
		m_params.m_ambientPass   = 1.0f;
		m_params.m_lightningPass = 1.0f;
		m_params.m_lightCount    = 4.0f;
		m_params.m_alpha         = 1.0f;

		m_ambient[0] = 0.05f;
		m_ambient[1] = 0.05f;
		m_ambient[2] = 0.05f;
		m_ambient[3] = 0.0f; //unused

		m_diffuse[0] = 0.8f;
		m_diffuse[1] = 0.8f;
		m_diffuse[2] = 0.8f;
		m_diffuse[3] = 0.0f; //unused

		m_specular_shininess[0] = 1.0f;
		m_specular_shininess[1] = 1.0f;
		m_specular_shininess[2] = 1.0f;
		m_specular_shininess[3] = 25.0f; //shininess

		m_fog[0] = 0.0f;   //color
		m_fog[1] = 0.0f;   //color
		m_fog[2] = 0.0f;   //color
		m_fog[3] = 0.0055f; //density

		m_color[0] = 1.0f;
		m_color[1] = 1.0f;
		m_color[2] = 1.0f;
		m_color[3] = 1.0f;

		m_time = 0.0f;

		m_flipV = float(s_flipV) * 2.0f - 1.0f;

		m_lightPosRadius[0] = 0.0f;
		m_lightPosRadius[1] = 0.0f;
		m_lightPosRadius[2] = 0.0f;
		m_lightPosRadius[3] = 1.0f;

		m_lightRgbInnerR[0] = 0.0f;
		m_lightRgbInnerR[1] = 0.0f;
		m_lightRgbInnerR[2] = 0.0f;
		m_lightRgbInnerR[3] = 1.0f;

		m_virtualLightPos_extrusionDist[0] = 0.0f;
		m_virtualLightPos_extrusionDist[1] = 0.0f;
		m_virtualLightPos_extrusionDist[2] = 0.0f;
		m_virtualLightPos_extrusionDist[3] = 100.0f;

		u_params                        = bgfx::createUniform("u_params",                        bgfx::UniformType::Uniform4fv);
		u_svparams                      = bgfx::createUniform("u_svparams",                      bgfx::UniformType::Uniform4fv);
		u_ambient                       = bgfx::createUniform("u_ambient",                       bgfx::UniformType::Uniform4fv);
		u_diffuse                       = bgfx::createUniform("u_diffuse",                       bgfx::UniformType::Uniform4fv);
		u_specular_shininess            = bgfx::createUniform("u_specular_shininess",            bgfx::UniformType::Uniform4fv);
		u_fog                           = bgfx::createUniform("u_fog",                           bgfx::UniformType::Uniform4fv);
		u_color                         = bgfx::createUniform("u_color",                         bgfx::UniformType::Uniform4fv);
		u_time                          = bgfx::createUniform("u_time",                          bgfx::UniformType::Uniform1f );
		u_flipV                         = bgfx::createUniform("u_flipV",                         bgfx::UniformType::Uniform1f );
		u_lightPosRadius                = bgfx::createUniform("u_lightPosRadius",                bgfx::UniformType::Uniform4fv);
		u_lightRgbInnerR                = bgfx::createUniform("u_lightRgbInnerR",                bgfx::UniformType::Uniform4fv);
		u_virtualLightPos_extrusionDist = bgfx::createUniform("u_virtualLightPos_extrusionDist", bgfx::UniformType::Uniform4fv);
	}

	//call this once at initialization
	void submitConstUniforms()
	{
		bgfx::setUniform(u_ambient,            &m_ambient);
		bgfx::setUniform(u_diffuse,            &m_diffuse);
		bgfx::setUniform(u_specular_shininess, &m_specular_shininess);
		bgfx::setUniform(u_fog,                &m_fog);
		bgfx::setUniform(u_flipV,              &m_flipV);
	}

	//call this once per frame
	void submitPerFrameUniforms()
	{
		bgfx::setUniform(u_time, &m_time);
	}

	//call this before each draw call
	void submitPerDrawUniforms()
	{
		bgfx::setUniform(u_params,                        &m_params);
		bgfx::setUniform(u_svparams,                      &m_svparams);
		bgfx::setUniform(u_color,                         &m_color);
		bgfx::setUniform(u_lightPosRadius,                &m_lightPosRadius);
		bgfx::setUniform(u_lightRgbInnerR,                &m_lightRgbInnerR);
		bgfx::setUniform(u_virtualLightPos_extrusionDist, &m_virtualLightPos_extrusionDist);
	}

	void destroy()
	{
		bgfx::destroyUniform(u_params);
		bgfx::destroyUniform(u_svparams);
		bgfx::destroyUniform(u_ambient);
		bgfx::destroyUniform(u_diffuse);
		bgfx::destroyUniform(u_specular_shininess);
		bgfx::destroyUniform(u_fog);
		bgfx::destroyUniform(u_color);
		bgfx::destroyUniform(u_time);
		bgfx::destroyUniform(u_flipV);
		bgfx::destroyUniform(u_lightPosRadius);
		bgfx::destroyUniform(u_lightRgbInnerR);
		bgfx::destroyUniform(u_virtualLightPos_extrusionDist);
	}

	struct
	{
		float m_ambientPass;
		float m_lightningPass;
		float m_alpha;
		float m_lightCount;
	} m_params;
	struct
	{
		float m_useStencilTex;
		float m_dfail;
		float m_unused0;
		float m_unused1;
	} m_svparams;
	float m_ambient[4];
	float m_diffuse[4];
	float m_specular_shininess[4];
	float m_fog[4];
	float m_color[4];
	float m_time;
	float m_flipV;
	float m_lightPosRadius[4];
	float m_lightRgbInnerR[4];
	float m_virtualLightPos_extrusionDist[4];

	/**
	 * u_params.x - u_ambientPass
	 * u_params.y - u_lightningPass
	 * u_params.z - u_alpha
	 * u_params.w - u_lightCount

	 * u_svparams.x - u_useStencilTex
	 * u_svparams.y - u_dfail
	 * u_svparams.z - unused
	 * u_svparams.w - unused
	 */

	bgfx::UniformHandle u_params;
	bgfx::UniformHandle u_svparams;
	bgfx::UniformHandle u_ambient;
	bgfx::UniformHandle u_diffuse;
	bgfx::UniformHandle u_specular_shininess;
	bgfx::UniformHandle u_fog;
	bgfx::UniformHandle u_color;
	bgfx::UniformHandle u_time;
	bgfx::UniformHandle u_flipV;
	bgfx::UniformHandle u_lightPosRadius;
	bgfx::UniformHandle u_lightRgbInnerR;
	bgfx::UniformHandle u_virtualLightPos_extrusionDist;
};
static Uniforms s_uniforms;

//-------------------------------------------------
// Render state
//-------------------------------------------------

struct RenderState
{
	enum Enum
	{
		ShadowVolume_UsingStencilTexture_DrawAmbient = 0,
		ShadowVolume_UsingStencilTexture_BuildDepth,
		ShadowVolume_UsingStencilTexture_CraftStencil_DepthPass,
		ShadowVolume_UsingStencilTexture_CraftStencil_DepthFail,
		ShadowVolume_UsingStencilTexture_DrawDiffuse,

		ShadowVolume_UsingStencilBuffer_DrawAmbient,
		ShadowVolume_UsingStencilBuffer_CraftStencil_DepthPass,
		ShadowVolume_UsingStencilBuffer_CraftStencil_DepthFail,
		ShadowVolume_UsingStencilBuffer_DrawDiffuse,

		Custom_Default,
		Custom_BlendLightTexture,
		Custom_DrawPlaneBottom,
		Custom_DrawShadowVolume_Lines,

		Count
	};

	uint64_t m_state;
	uint32_t m_blendFactorRgba;
	uint32_t m_fstencil;
	uint32_t m_bstencil;
};

static void setRenderState(const RenderState& _renderState)
{
	bgfx::setStencil(_renderState.m_fstencil, _renderState.m_bstencil);
	bgfx::setState(_renderState.m_state, _renderState.m_blendFactorRgba);
}

static RenderState s_renderStates[RenderState::Count]  =
{
	{ //ShadowVolume_UsingStencilTexture_DrawAmbient
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_DEPTH_WRITE
			| BGFX_STATE_DEPTH_TEST_LESS
			| BGFX_STATE_CULL_CCW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //ShadowVolume_UsingStencilTexture_BuildDepth
		BGFX_STATE_DEPTH_WRITE
			| BGFX_STATE_DEPTH_TEST_LESS
			| BGFX_STATE_CULL_CCW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //ShadowVolume_UsingStencilTexture_CraftStencil_DepthPass
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE)
			| BGFX_STATE_DEPTH_TEST_LEQUAL
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //ShadowVolume_UsingStencilTexture_CraftStencil_DepthFail
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE)
			| BGFX_STATE_DEPTH_TEST_GEQUAL
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //ShadowVolume_UsingStencilTexture_DrawDiffuse
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE)
			| BGFX_STATE_DEPTH_WRITE
			| BGFX_STATE_DEPTH_TEST_EQUAL
			| BGFX_STATE_CULL_CCW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //ShadowVolume_UsingStencilBuffer_DrawAmbient
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_DEPTH_WRITE
			| BGFX_STATE_DEPTH_TEST_LESS
			| BGFX_STATE_CULL_CCW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //ShadowVolume_UsingStencilBuffer_CraftStencil_DepthPass
		BGFX_STATE_DEPTH_TEST_LEQUAL
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_TEST_ALWAYS
			| BGFX_STENCIL_FUNC_REF(1)        
			| BGFX_STENCIL_FUNC_RMASK(0xff)
			| BGFX_STENCIL_OP_FAIL_S_KEEP
			| BGFX_STENCIL_OP_FAIL_Z_KEEP
			| BGFX_STENCIL_OP_PASS_Z_DECR
			, BGFX_STENCIL_TEST_ALWAYS
			| BGFX_STENCIL_FUNC_REF(1)        
			| BGFX_STENCIL_FUNC_RMASK(0xff)
			| BGFX_STENCIL_OP_FAIL_S_KEEP
			| BGFX_STENCIL_OP_FAIL_Z_KEEP
			| BGFX_STENCIL_OP_PASS_Z_INCR
	},
	{ //ShadowVolume_UsingStencilBuffer_CraftStencil_DepthFail
		BGFX_STATE_DEPTH_TEST_LEQUAL
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_TEST_ALWAYS
			| BGFX_STENCIL_FUNC_REF(1)        
			| BGFX_STENCIL_FUNC_RMASK(0xff)
			| BGFX_STENCIL_OP_FAIL_S_KEEP
			| BGFX_STENCIL_OP_FAIL_Z_INCR
			| BGFX_STENCIL_OP_PASS_Z_KEEP
			, BGFX_STENCIL_TEST_ALWAYS
			| BGFX_STENCIL_FUNC_REF(1)        
			| BGFX_STENCIL_FUNC_RMASK(0xff)
			| BGFX_STENCIL_OP_FAIL_S_KEEP
			| BGFX_STENCIL_OP_FAIL_Z_DECR
			| BGFX_STENCIL_OP_PASS_Z_KEEP
	},
	{ //ShadowVolume_UsingStencilBuffer_DrawDiffuse
			BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE)
			| BGFX_STATE_DEPTH_TEST_EQUAL
			| BGFX_STATE_CULL_CCW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_TEST_EQUAL
			| BGFX_STENCIL_FUNC_REF(0)
			| BGFX_STENCIL_FUNC_RMASK(0xff)
			| BGFX_STENCIL_OP_FAIL_S_KEEP
			| BGFX_STENCIL_OP_FAIL_Z_KEEP
			| BGFX_STENCIL_OP_PASS_Z_KEEP
			, BGFX_STENCIL_NONE
	},
	{ //Custom_Default
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_DEPTH_WRITE
			| BGFX_STATE_DEPTH_TEST_LESS
			| BGFX_STATE_CULL_CCW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //Custom_BlendLightTexture
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_DEPTH_WRITE
			| BGFX_STATE_DEPTH_TEST_LESS
			| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_COLOR, BGFX_STATE_BLEND_INV_SRC_COLOR)
			| BGFX_STATE_CULL_CCW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //Custom_DrawPlaneBottom
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_ALPHA_WRITE
			| BGFX_STATE_DEPTH_WRITE
			| BGFX_STATE_CULL_CW
			| BGFX_STATE_MSAA
			, UINT32_MAX
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	},
	{ //Custom_DrawShadowVolume_Lines
		BGFX_STATE_RGB_WRITE
			| BGFX_STATE_DEPTH_TEST_LESS
			| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_SRC_ALPHA)
			| BGFX_STATE_PT_LINES
			| BGFX_STATE_MSAA
			, 0x0f0f0fff
			, BGFX_STENCIL_NONE
			, BGFX_STENCIL_NONE
	}
};

struct ViewState
{
	ViewState(uint32_t _width  = 1280
			, uint32_t _height = 720
			)
		: m_width(_width)
		, m_height(_height)
	{ }

	uint32_t m_width;
	uint32_t m_height;

	float m_view[16], m_proj[16];
};

void setViewRectTransform(uint8_t _view, const ViewState& _viewState)
{
	bgfx::setViewRect(_view, 0, 0, _viewState.m_width, _viewState.m_height);
	bgfx::setViewTransform(_view, _viewState.m_view, _viewState.m_proj);
}

void setViewRectTransformMask(uint32_t _viewMask, const ViewState& _viewState)
{
	bgfx::setViewRectMask(_viewMask, 0, 0, _viewState.m_width, _viewState.m_height);
	bgfx::setViewTransformMask(_viewMask, _viewState.m_view, _viewState.m_proj);
}

struct ClearValues
{
	uint32_t m_clearRgba;
	float    m_clearDepth;
	uint8_t  m_clearStencil;
};

void clearView(uint8_t _id, uint8_t _flags, const ClearValues& _clearValues)
{
	bgfx::setViewClear(_id
			, _flags
			, _clearValues.m_clearRgba
			, _clearValues.m_clearDepth
			, _clearValues.m_clearStencil
			);

	// Keep track of cleared views
	s_clearMask |= 1 << _id;
}

void clearViewMask(uint32_t _viewMask, uint8_t _flags, const ClearValues& _clearValues)
{
	bgfx::setViewClearMask(_viewMask
			, _flags
			, _clearValues.m_clearRgba
			, _clearValues.m_clearDepth
			, _clearValues.m_clearStencil
			);

	// Keep track of cleared views
	s_clearMask |= _viewMask;
}

void submit(uint8_t _id, int32_t _depth = 0)
{
	// Submit
	bgfx::submit(_id, _depth);

	// Keep track of submited view ids
	s_viewMask |= 1 << _id;
}

void submitMask(uint32_t _viewMask, int32_t _depth = 0)
{
	// Submit
	bgfx::submitMask(_viewMask, _depth);

	// Keep track of submited view ids
	s_viewMask |= _viewMask;
}

void setViewRenderTarget(uint8_t _id, bgfx::RenderTargetHandle _handle)
{
	bgfx::setViewRenderTarget(_id, _handle);

	// Keep track of render target view ids
	s_rtMask |= 1 << _id;
}

//-------------------------------------------------
// Mesh
//-------------------------------------------------

struct Aabb
{
	float m_min[3];
	float m_max[3];
};

struct Obb
{
	float m_mtx[16];
};

struct Sphere
{
	float m_center[3];
	float m_radius;
};

struct Primitive
{
	uint32_t m_startIndex;
	uint32_t m_numIndices;
	uint32_t m_startVertex;
	uint32_t m_numVertices;

	Sphere m_sphere;
	Aabb m_aabb;
	Obb m_obb;
};

typedef std::vector<Primitive> PrimitiveArray;

struct Face
{
	uint16_t m_i[3];
	float m_plane[4];
};
typedef std::vector<Face> FaceArray;

struct Edge
{
	struct Plane
	{
		float m_plane[4];
		bool m_reverseVertexOrder;
	};

	Edge(const float* _v0, const float* _v1)
		: m_faceIndex(0)
	{
		memcpy(m_v0, _v0, 3*sizeof(float));
		memcpy(m_v1, _v1, 3*sizeof(float));
	}               

	Plane& nextFace()
	{
		BX_CHECK(m_faceIndex < FACE_NUM, "Error! 2-manifold meshes must be used!");
		return m_faces[(m_faceIndex++)%FACE_NUM];
	}

	float m_v0[3], m_v1[3];    
	static const uint8_t FACE_NUM = 2;
	Plane m_faces[FACE_NUM];
	uint8_t m_faceIndex;
};
typedef std::vector<Edge> EdgeArray;

struct HalfEdge
{
#define INVALID_EDGE_INDEX UINT16_MAX
	uint16_t m_secondIndex;
	bool m_marked;
};

struct HalfEdges
{
	HalfEdges()
		: m_data()
		, m_offsets()
		, m_endPtr()
	{ }

	void init(uint16_t* _indices, uint32_t _numIndices)
	{
		m_data = (HalfEdge*)malloc(2 * _numIndices * sizeof(HalfEdge));

		std::unordered_map<uint16_t, std::vector<uint16_t> > edges;
		for (uint32_t ii = 0; ii < _numIndices; ii+=3)
		{
			uint16_t idx0 = _indices[ii];
			uint16_t idx1 = _indices[ii+1];
			uint16_t idx2 = _indices[ii+2];

			edges[idx0].push_back(idx1);
			edges[idx1].push_back(idx2);
			edges[idx2].push_back(idx0);
		}

		uint32_t numRows = edges.size();
		m_offsets = (uint32_t*)malloc(numRows * sizeof(uint32_t));

		HalfEdge* he = m_data;
		for (uint32_t ii = 0; ii < numRows; ++ii)
		{
			m_offsets[ii] = uint32_t(he - m_data);

			std::vector<uint16_t>& row = edges[ii];
			for (uint32_t jj = 0, end = row.size(); jj < end; ++jj)
			{
				he->m_secondIndex = row[jj];
				he->m_marked = false;
				++he;
			}
			he->m_secondIndex = INVALID_EDGE_INDEX;
			++he;
		}
		he->m_secondIndex = 0;
		m_endPtr = he;
	}
	
	void destroy()
	{
		free(m_data);
		m_data = NULL;
		free(m_offsets);
		m_offsets = NULL;
	}

	void mark(uint16_t _firstIndex, uint16_t _secondIndex)
	{
		HalfEdge* ptr = &m_data[m_offsets[_firstIndex]];
		while (INVALID_EDGE_INDEX != ptr->m_secondIndex)
		{
			if (ptr->m_secondIndex == _secondIndex)
			{
				ptr->m_marked = true;
				break;
			}
			++ptr;
		}
	}

	bool unmark(uint16_t _firstIndex, uint16_t _secondIndex)
	{
		bool ret = false;
		HalfEdge* ptr = &m_data[m_offsets[_firstIndex]];
		while (INVALID_EDGE_INDEX != ptr->m_secondIndex)
		{
			if (ptr->m_secondIndex == _secondIndex && ptr->m_marked)
			{
				ptr->m_marked = false;
				ret = true;
				break;
			}
			++ptr;
		}
		return ret;
	}

	inline HalfEdge* begin() const { return m_data; }
	inline HalfEdge* end() const { return m_endPtr; }

	HalfEdge* m_data;
	uint32_t* m_offsets;
	HalfEdge* m_endPtr;
};

struct Group
{
	Group()
	{
		reset();
	}

	void reset()
	{
		m_vbh.idx = bgfx::invalidHandle;
		m_ibh.idx = bgfx::invalidHandle;
		m_numVertices = 0;
		m_vertices = NULL;
		m_numIndices = 0;
		m_indices = NULL;
		m_prims.clear();
	}

	void fillStructures(uint16_t _stride)
	{
		m_faces.clear();
		m_edges.clear();
		m_halfEdges.destroy();

		//init halfedges
		m_halfEdges.init(m_indices, m_numIndices);

		//init faces and edges
		m_faces.reserve(m_numIndices/3); //1 face = 3 indices 
		m_edges.reserve(m_numIndices);   //1 triangle = 3 indices = 3 edges.

		typedef std::map<std::pair<uint16_t, uint16_t>, uint32_t> EdgeIndexMap;
		EdgeIndexMap edgeIndexMap;

		for (uint32_t ii = 0, size = m_numIndices/3; ii < size; ++ii)
		{
			const uint16_t* indices = &m_indices[ii*3];
			const uint16_t i0 = indices[0];
			const uint16_t i1 = indices[1];
			const uint16_t i2 = indices[2];
			const float* v0 = (float*)&m_vertices[i0*_stride];
			const float* v1 = (float*)&m_vertices[i1*_stride];
			const float* v2 = (float*)&m_vertices[i2*_stride];

			float plane[4];
			planeNormal(plane, v0, v2, v1);

			Face face;
			face.m_i[0] = i0;
			face.m_i[1] = i1;
			face.m_i[2] = i2;
			memcpy(face.m_plane, plane, 4*sizeof(float));
			m_faces.push_back(face);

			uint16_t triangleI[3][2] =
			{
				{i0, i1},
				{i1, i2},
				{i2, i0},
			};

			const float* triangleV[3][2] =
			{
				{v0, v1},
				{v1, v2},
				{v2, v0},
			};

			typedef std::vector<uint8_t> TriangleIndex;
			TriangleIndex triangleIndex;

			for (uint8_t jj = 0; jj < 3; ++jj)
			{   
				EdgeIndexMap::iterator iter = edgeIndexMap.find(std::make_pair(triangleI[jj][1], triangleI[jj][0]));
				if (edgeIndexMap.end() != iter)
				{
					const uint32_t index = iter->second;
					Edge* edge = &m_edges[index];

					Edge::Plane& face = edge->nextFace();
					memcpy(face.m_plane, plane, 4*sizeof(float));
					face.m_reverseVertexOrder = true;
				}
				else
				{
					triangleIndex.push_back(jj);
				}
			}

			for (TriangleIndex::const_iterator iter = triangleIndex.begin(), end = triangleIndex.end(); iter != end; ++iter)
			{
				const uint8_t index = *iter;
				const uint16_t i0 = triangleI[index][0];
				const uint16_t i1 = triangleI[index][1];
				const float* v0 = triangleV[index][0];
				const float* v1 = triangleV[index][1];

				Edge edge(v0, v1);
				Edge::Plane& face = edge.nextFace();
				memcpy(face.m_plane, plane, 4*sizeof(float));
				face.m_reverseVertexOrder = false;
				m_edges.push_back(edge);

				edgeIndexMap.insert(std::make_pair(std::make_pair(i0, i1), m_edges.size()-1));
			}
		}
	}

	void unload()
	{
		bgfx::destroyVertexBuffer(m_vbh);
		if (bgfx::invalidHandle != m_ibh.idx)
		{
			bgfx::destroyIndexBuffer(m_ibh);
		}
		free(m_vertices);
		m_vertices = NULL;
		free(m_indices);
		m_indices = NULL;
		m_halfEdges.destroy();
	}

	bgfx::VertexBufferHandle m_vbh;
	bgfx::IndexBufferHandle m_ibh;
	uint16_t m_numVertices;
	uint8_t* m_vertices;
	uint32_t m_numIndices;
	uint16_t* m_indices;
	Sphere m_sphere;
	Aabb m_aabb;
	Obb m_obb;
	PrimitiveArray m_prims;
	EdgeArray m_edges;
	FaceArray m_faces;
	HalfEdges m_halfEdges;
};
typedef std::vector<Group> GroupArray;

struct Mesh
{
	void load(const void* _vertices, uint32_t _numVertices, const bgfx::VertexDecl _decl, const uint16_t* _indices, uint32_t _numIndices)
	{
		Group group;
		const bgfx::Memory* mem;
		uint32_t size;

		//vertices
		group.m_numVertices = _numVertices;
		size = _numVertices*_decl.getStride();

		group.m_vertices = (uint8_t*)malloc(size);
		memcpy(group.m_vertices, _vertices, size);

		mem = bgfx::makeRef(group.m_vertices, size);
		group.m_vbh = bgfx::createVertexBuffer(mem, _decl);

		//indices
		group.m_numIndices = _numIndices;
		size = _numIndices*2;

		group.m_indices = (uint16_t*)malloc(size);
		memcpy(group.m_indices, _indices, size);

		mem = bgfx::makeRef(group.m_indices, size);
		group.m_ibh = bgfx::createIndexBuffer(mem);
	
		//TODO:
		// group.m_sphere = ...
		// group.m_aabb = ...
		// group.m_obb = ...
		// group.m_prims = ...

		m_groups.push_back(group);
	}

	void load(const char* _filePath)
	{
#define BGFX_CHUNK_MAGIC_VB BX_MAKEFOURCC('V', 'B', ' ', 0x0)
#define BGFX_CHUNK_MAGIC_IB BX_MAKEFOURCC('I', 'B', ' ', 0x0)
#define BGFX_CHUNK_MAGIC_PRI BX_MAKEFOURCC('P', 'R', 'I', 0x0)

		bx::CrtFileReader reader;
		reader.open(_filePath);

		Group group;

		uint32_t chunk;
		while (4 == bx::read(&reader, chunk) )
		{
			switch (chunk)
			{
				case BGFX_CHUNK_MAGIC_VB:
					{
						bx::read(&reader, group.m_sphere);
						bx::read(&reader, group.m_aabb);
						bx::read(&reader, group.m_obb);

						bx::read(&reader, m_decl);
						uint16_t stride = m_decl.getStride();

						bx::read(&reader, group.m_numVertices);
						const uint32_t size = group.m_numVertices*stride;
						group.m_vertices = (uint8_t*)malloc(size);
						bx::read(&reader, group.m_vertices, size);

						const bgfx::Memory* mem = bgfx::makeRef(group.m_vertices, size);
						group.m_vbh = bgfx::createVertexBuffer(mem, m_decl);
					}
					break;

				case BGFX_CHUNK_MAGIC_IB:
					{
						bx::read(&reader, group.m_numIndices);
						const uint32_t size = group.m_numIndices*2;
						group.m_indices = (uint16_t*)malloc(size);
						bx::read(&reader, group.m_indices, size);

						const bgfx::Memory* mem = bgfx::makeRef(group.m_indices, size);
						group.m_ibh = bgfx::createIndexBuffer(mem);
					}
					break;

				case BGFX_CHUNK_MAGIC_PRI:
					{
						uint16_t len;
						bx::read(&reader, len);

						std::string material;
						material.resize(len);
						bx::read(&reader, const_cast<char*>(material.c_str() ), len);

						uint16_t num;
						bx::read(&reader, num);

						for (uint32_t ii = 0; ii < num; ++ii)
						{
							bx::read(&reader, len);

							std::string name;
							name.resize(len);
							bx::read(&reader, const_cast<char*>(name.c_str() ), len);

							Primitive prim;
							bx::read(&reader, prim.m_startIndex);
							bx::read(&reader, prim.m_numIndices);
							bx::read(&reader, prim.m_startVertex);
							bx::read(&reader, prim.m_numVertices);
							bx::read(&reader, prim.m_sphere);
							bx::read(&reader, prim.m_aabb);
							bx::read(&reader, prim.m_obb);

							group.m_prims.push_back(prim);
						}

						m_groups.push_back(group);
						group.reset();
					}
					break;

				default:
					DBG("%08x at %d", chunk, reader.seek() );
					break;
			}
		}

		reader.close();

		uint16_t stride = m_decl.getStride();
		for (GroupArray::iterator it = m_groups.begin(), itEnd = m_groups.end(); it != itEnd; ++it)
		{
			it->fillStructures(stride);
		}
	}

	void unload()
	{
		for (GroupArray::iterator it = m_groups.begin(), itEnd = m_groups.end(); it != itEnd; ++it)
		{
			it->unload();
		}
		m_groups.clear();
	}

	bgfx::VertexDecl m_decl;
	GroupArray m_groups;
};

struct Model
{
	Model()
	{ 
		m_program.idx = bgfx::invalidHandle;
		m_texture.idx = bgfx::invalidHandle;
	}

	void load(const void* _vertices, uint32_t _numVertices, const bgfx::VertexDecl _decl, const uint16_t* _indices, uint32_t _numIndices)
	{
		m_mesh.load(_vertices, _numVertices, _decl, _indices, _numIndices);
	}

	void load(const char* _meshFilePath)
	{
		m_mesh.load(_meshFilePath);
	}

	void unload()
	{
		m_mesh.unload();
	}

	void submit(uint8_t _viewId, float* _mtx, const RenderState& _renderState)
	{
		for (GroupArray::const_iterator it = m_mesh.m_groups.begin(), itEnd = m_mesh.m_groups.end(); it != itEnd; ++it)
		{
			const Group& group = *it;

			// Set uniforms
			s_uniforms.submitPerDrawUniforms();

			// Set program
			BX_CHECK(bgfx::invalidHandle != m_program, "Error, program is not set.");
			bgfx::setProgram(m_program);

			// Set transform
			bgfx::setTransform(_mtx);

			// Set buffers
			bgfx::setIndexBuffer(group.m_ibh);
			bgfx::setVertexBuffer(group.m_vbh);

			// Set textures
			if (bgfx::invalidHandle != m_texture.idx)
			{
				bgfx::setTexture(0, u_texColor, m_texture);
			}
			bgfx::setTexture(7, u_texStencil, s_stencilRt);

			// Apply render state
			::setRenderState(_renderState);

			// Submit
			::submit(_viewId);
		}
	}

	Mesh m_mesh;
	bgfx::ProgramHandle m_program;
	bgfx::TextureHandle m_texture;
};

struct Instance
{
	Instance()
		: m_svExtrusionDistance(150.0f)
	{ 
		m_color[0] = 1.0f;
		m_color[1] = 1.0f;
		m_color[2] = 1.0f;
	}

	void submit(uint8_t _viewId, const RenderState& _renderState)
	{
		memcpy(s_uniforms.m_color, m_color, 3*sizeof(float));

		float mtx[16];
		mtxScaleRotateTranslate(mtx
				, m_scale[0]
				, m_scale[1]
				, m_scale[2]
				, m_rotation[0]
				, m_rotation[1]
				, m_rotation[2]
				, m_pos[0]
				, m_pos[1]
				, m_pos[2]
				);

		BX_CHECK(NULL != m_model, "Instance model cannot be NULL!");
		m_model->submit(_viewId, mtx, _renderState);
	}

	float m_scale[3];
	float m_rotation[3];
	float m_pos[3];

	float m_color[3];
	float m_svExtrusionDistance;

	Model* m_model;
};

//-------------------------------------------------
// Shadow volume
//-------------------------------------------------

struct ShadowVolumeImpl
{
	enum Enum
	{
		DepthPass,
		DepthFail,
	};
};

struct ShadowVolumeAlgorithm
{
	enum Enum
	{
		FaceBased,
		EdgeBased,
	};
};

struct ShadowVolume
{
	bgfx::VertexBufferHandle m_vbSides;
	bgfx::IndexBufferHandle m_ibSides;
	bgfx::IndexBufferHandle m_ibFrontCap;
	bgfx::IndexBufferHandle m_ibBackCap;
	
	uint32_t m_numVertices;
	uint32_t m_numIndices;

	const float* m_mtx;
	const float* m_lightPos;

	bool m_cap;
};

void shadowVolumeTransform(float* __restrict _outMtx
		, float* __restrict _outLightPos
		, const float* __restrict _scale
		, const float* __restrict _rotate
		, const float* __restrict _translate
		, const float* __restrict _lightPos // world pos
		)
{
	/*
	 * Instead of transforming all the vertices, transform light instead:
	 * mtx = pivotTranslate -> rotateZYX -> invScale
	 * light = mtx * origin
	 * _outMtx = scale -> rotateXYZ -> translate
	 */

	float origin[3] = { 0.0f, 0.0f, 0.0f };
	float light[3];
	float mtx[16];
	float mtxPivotTranslate[16];
	float mtxRotateZYX[16];
	float mtxInvScale[16];
	float mtxScale[16];
	float mtxRotateXYZ[16];
	float mtxTranslate[16];
	float mtxtmp0[16];

	::mtxTranslate(mtxPivotTranslate
			, _lightPos[0] - _translate[0]
			, _lightPos[1] - _translate[1]
			, _lightPos[2] - _translate[2]
			);

	::mtxRotateZYX(mtxRotateZYX
			, -_rotate[0]
			, -_rotate[1]
			, -_rotate[2]
			);

	::mtxScale(mtxInvScale
			, 1.0f / _scale[0]
			, 1.0f / _scale[1]
			, 1.0f / _scale[2]
			);

	mtxMul(mtxtmp0, mtxPivotTranslate, mtxRotateZYX);
	mtxMul(mtx, mtxtmp0, mtxInvScale);

	vec3MulMtx(light, origin, mtx);
	memcpy(_outLightPos, light, 3*sizeof(float));

	::mtxScale(mtxScale
			, _scale[0]
			, _scale[1]
			, _scale[2]
			);

	::mtxRotateXYZ(mtxRotateXYZ
			, _rotate[0]
			, _rotate[1]
			, _rotate[2]
			);

	::mtxTranslate(mtxTranslate
			, _translate[0]
			, _translate[1]
			, _translate[2]
			);

	mtxMul(mtxtmp0, mtxScale, mtxRotateXYZ);
	mtxMul(_outMtx, mtxtmp0, mtxTranslate);
}

void shadowVolumeCreate(ShadowVolume& _shadowVolume
		, Group& _group
		, uint16_t _stride
		, const float* _mtx
		, const float* _light // in model space
		, ShadowVolumeImpl::Enum _impl = ShadowVolumeImpl::DepthPass
		, ShadowVolumeAlgorithm::Enum _algo = ShadowVolumeAlgorithm::FaceBased
		, bool _textureAsStencil = false
		)
{
	const uint8_t*    vertices  = _group.m_vertices;
	const FaceArray&  faces     = _group.m_faces;
	const EdgeArray&  edges     = _group.m_edges;
	HalfEdges&        halfEdges = _group.m_halfEdges;

	struct VertexData
	{
		VertexData() { }

		VertexData(const float* _v3, float _extrude = 0.0f, float _k = 1.0f)
		{
			memcpy(m_v, _v3, 3*sizeof(float));
			m_extrude = _extrude;
			m_k = _k;
		}

		float m_v[3];
		float m_extrude, m_k;
	};

	struct Index3us
	{
		Index3us() { }

		Index3us(uint16_t _i0, uint16_t _i1, uint16_t _i2)
			: m_i0(_i0)
			, m_i1(_i1)
			, m_i2(_i2)
		{ }

		uint16_t m_i0, m_i1, m_i2;
	};

	VertexData* verticesSide;
	Index3us*   indicesSide;
	Index3us*   indicesFrontCap;
	Index3us*   indicesBackCap;

	verticesSide    = (VertexData*) malloc (100000 * sizeof(VertexData));
	indicesSide     = (Index3us*)   malloc (100000 * sizeof(Index3us));
	indicesFrontCap = (Index3us*)   malloc (100000 * sizeof(Index3us));
	indicesBackCap  = (Index3us*)   malloc (100000 * sizeof(Index3us));

	uint16_t vsideI    = 0;
	uint16_t sideI     = 0;
	uint16_t frontCapI = 0;
	uint16_t backCapI  = 0;

	bool cap = (ShadowVolumeImpl::DepthFail == _impl);
	uint16_t indexSide = 0;
	
	switch (_algo)
	{
		case ShadowVolumeAlgorithm::FaceBased:
			{
				for (FaceArray::const_iterator iter = faces.begin(), end = faces.end(); iter != end; ++iter)
				{
					const Face& face = *iter;
					const uint16_t* indices = face.m_i;

					bool frontFacing = false;
					float f = vec3Dot(face.m_plane, _light) + face.m_plane[3];
					if (f > 0.0f)
					{
						frontFacing = true;
						uint16_t triangleEdges[3][2] = 
						{
							{ indices[0], indices[1] },
							{ indices[1], indices[2] },
							{ indices[2], indices[0] },
						};

						for (uint8_t ii = 0; ii < 3; ++ii)
						{
							uint16_t first  = triangleEdges[ii][0];
							uint16_t second = triangleEdges[ii][1];

							if (!halfEdges.unmark(second, first))
							{
								halfEdges.mark(first, second);
							}
						}
					}

					if (cap)
					{
						if (frontFacing)
						{
							indicesFrontCap[frontCapI++] = *(Index3us*)face.m_i;
						}
						else
						{
							indicesBackCap[backCapI++] = *(Index3us*)face.m_i; 
						}
						
						/**
						 * TODO: if '_useFrontFacingFacesAsBackCap' is needed, implement it as such:
						 *
						 * bool condition0 = frontFacing && _useFrontFacingFacesAsBackCap;
						 * bool condition1 = !frontFacing && !_useFrontFacingFacesAsBackCap;
						 * if (condition0 || condition1)
						 * {
						 *		const Index3us tmp = { indices[0], indices[1+condition0], indices[2-condition0] }; //winding regarding condition0 
						 *		indicesBackCap.push_back(tmp);
						 * }
						 */
					}
				}

				//fill side arrays
				uint16_t firstIndex = 0;
				HalfEdge* he = halfEdges.begin();
				while (halfEdges.end() != he)
				{
					if (he->m_marked)
					{
						he->m_marked = false;

						const float* v0 = (float*)&vertices[firstIndex*_stride];
						const float* v1 = (float*)&vertices[he->m_secondIndex*_stride];

						verticesSide[vsideI++] = VertexData(v0, 0.0f);
						verticesSide[vsideI++] = VertexData(v0, 1.0f);
						verticesSide[vsideI++] = VertexData(v1, 0.0f);
						verticesSide[vsideI++] = VertexData(v1, 1.0f);

						//sides
						indicesSide[sideI++] = Index3us(indexSide+0, indexSide+1, indexSide+2); 
						indicesSide[sideI++] = Index3us(indexSide+2, indexSide+1, indexSide+3);

						indexSide += 4;
					}

					++he;
					if (INVALID_EDGE_INDEX == he->m_secondIndex)
					{
						++he;
						++firstIndex;
					}
				}
			}
			break;
		case ShadowVolumeAlgorithm::EdgeBased:
			{
				for (EdgeArray::const_iterator iter = edges.begin(), end = edges.end(); iter != end; ++iter)
				{
					const Edge& edge = *iter;
					const float* v0 = edge.m_v0;
					const float* v1 = edge.m_v1;

					int16_t k = 0;
					float s;
					for (uint8_t ii = 0; ii < edge.m_faceIndex; ++ii)
					{
						const Edge::Plane& face = edge.m_faces[ii];
						s = fsign(vec3Dot(face.m_plane, _light) + face.m_plane[3]);
						if (face.m_reverseVertexOrder)
						{
							s = -s;
						}
						k += uint16_t(s);
					}

					if (k == 0)
					{
						continue;
					}

					verticesSide[vsideI++] = VertexData(v0, 0.0f, k);
					verticesSide[vsideI++] = VertexData(v0, 1.0f, k);
					verticesSide[vsideI++] = VertexData(v1, 0.0f, k);
					verticesSide[vsideI++] = VertexData(v1, 1.0f, k);

					k = _textureAsStencil ? 1 : k;
					uint16_t winding = uint16_t(k > 0);
					for (uint8_t ii = 0, end = abs(k); ii < end; ++ii)
					{
						indicesSide[sideI++] =
						Index3us(uint16_t(indexSide)
							, uint16_t(indexSide + 2 - winding)
							, uint16_t(indexSide + 1 + winding)
							);
						indicesSide[sideI++] =
						Index3us(uint16_t(indexSide + 2)
							, uint16_t(indexSide + 3 - winding*2)
							, uint16_t(indexSide + 1 + winding*2) 
							);
					}

					indexSide += 4;
				}

				if (cap)
				{
					//this could/should be done on GPU !
					for (FaceArray::const_iterator iter = faces.begin(), end = faces.end(); iter != end; ++iter)
					{
						const Face& face = *iter;

						float f = vec3Dot(face.m_plane, _light) + face.m_plane[3];
						bool frontFacing = (f > 0.0f); 

						for (uint8_t ii = 0, end = 1 + uint8_t(!_textureAsStencil); ii < end; ++ii)
						{
							if (frontFacing)
							{
								indicesFrontCap[frontCapI++] = *(Index3us*)face.m_i; 
							}
							else
							{
								indicesBackCap[backCapI++] = *(Index3us*)face.m_i; 
							}
						}
					}
				}
			}
			break;
	}

	bgfx::VertexDecl decl;
	decl.begin();
	decl.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float);
	decl.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float);
	decl.end();

	//fill the structure
	_shadowVolume.m_numVertices = vsideI;
	_shadowVolume.m_numIndices  = sideI + frontCapI + backCapI;
	_shadowVolume.m_mtx         = _mtx;
	_shadowVolume.m_lightPos    = _light;
	_shadowVolume.m_cap         = cap;

	const bgfx::Memory* mem;

	//sides
	uint32_t vsize = vsideI * 5*sizeof(float);
	uint32_t isize = sideI * 3*sizeof(uint16_t);

	mem = bgfx::alloc(vsize);
	memcpy(mem->data, verticesSide, vsize);
	_shadowVolume.m_vbSides = bgfx::createVertexBuffer(mem, decl);

	mem = bgfx::alloc(isize);
	memcpy(mem->data, indicesSide, isize);
	_shadowVolume.m_ibSides = bgfx::createIndexBuffer(mem);

	// bgfx::destroy*Buffer doesn't actually destroy buffers now.
	// Instead, these bgfx::destroy*Buffer commands get queued to be executed after the end of the next frame.
	bgfx::destroyVertexBuffer(_shadowVolume.m_vbSides);
	bgfx::destroyIndexBuffer(_shadowVolume.m_ibSides);

	if (cap)
	{
		//front cap
		isize = frontCapI * 3*sizeof(uint16_t); 
		mem = bgfx::alloc(isize);
		memcpy(mem->data, indicesFrontCap, isize);
		_shadowVolume.m_ibFrontCap = bgfx::createIndexBuffer(mem);

		//gets destroyed after the end of the next frame
		bgfx::destroyIndexBuffer(_shadowVolume.m_ibFrontCap);

		//back cap
		isize = backCapI * 3*sizeof(uint16_t); 
		mem = bgfx::alloc(isize);
		memcpy(mem->data, indicesBackCap, isize);
		_shadowVolume.m_ibBackCap = bgfx::createIndexBuffer(mem);

		//gets destroyed after the end of the next frame
		bgfx::destroyIndexBuffer(_shadowVolume.m_ibBackCap);
	}

	//release resources
	free(verticesSide);
	free(indicesSide);
	free(indicesFrontCap);
	free(indicesBackCap);
}

void createNearClipVolume(float* __restrict _outPlanes24f
		, float* __restrict _lightPos
		, float* __restrict _view
		, float _fovy
		, float _aspect
		, float _near
		)
{
	float (*volumePlanes)[4] = (float(*)[4])_outPlanes24f;

	float mtxViewInv[16];
	float mtxViewTrans[16];
	mtxInverse(mtxViewInv, _view);
	mtxTranspose(mtxViewTrans, _view);

	float lightPosV[4];
	vec4MulMtx(lightPosV, _lightPos, _view);

	const float delta = 0.1f;

	float nearNormal[4] = { 0.0f, 0.0f, 1.0f, _near };
	float d = vec3Dot(lightPosV, nearNormal) + lightPosV[3] * nearNormal[3];

	// Light is:
	//  1.0f - in front of near plane
	//  0.0f - on the near plane
	// -1.0f - behind near plane
	float lightSide = float((d > delta) - (d < -delta));

	if (lightSide == 0.0f)
	{
		//TODO: implement.. for now this doesn't seem to cause problems.
	}
	
	float t = tanf(_fovy*( (float)M_PI/180.0f)*0.5f) * _near;
	float b = -t;
	float r = t * _aspect;
	float l = -r;

	float cornersV[4][3] =
	{
		{ r, t, _near },
		{ l, t, _near },
		{ l, b, _near },
		{ r, b, _near },
	};

	float corners[4][3];
	vec3MulMtx(corners[0], cornersV[0], mtxViewInv);
	vec3MulMtx(corners[1], cornersV[1], mtxViewInv);
	vec3MulMtx(corners[2], cornersV[2], mtxViewInv);
	vec3MulMtx(corners[3], cornersV[3], mtxViewInv);

	float planeNormals[4][3];
	for (uint8_t ii = 0; ii < 4; ++ii)
	{
		float* normal = planeNormals[ii];
		float* plane = volumePlanes[ii];

		float planeVec[3];
		vec3Sub(planeVec, corners[ii], corners[(ii-1)%4]);

		float light[3];
		float tmp[3];
		vec3Mul(tmp, corners[ii], _lightPos[3]);
		vec3Sub(light, _lightPos, tmp);

		vec3Cross(normal, planeVec, light);
		
		normal[0] *= lightSide;
		normal[1] *= lightSide;
		normal[2] *= lightSide;

		float lenInv = 1.0f / sqrtf(vec3Dot(normal, normal));

		plane[0] = normal[0] * lenInv;
		plane[1] = normal[1] * lenInv;
		plane[2] = normal[2] * lenInv;
		plane[3] = -vec3Dot(normal, corners[ii]) * lenInv;
	}

	float nearPlaneV[4] =
	{
		0.0f * lightSide,  
		0.0f * lightSide,  
		1.0f * lightSide,  
		_near * lightSide, 
	};
	vec4MulMtx(volumePlanes[4], nearPlaneV, mtxViewTrans);

	float* lightPlane = volumePlanes[5];
	float lightPlaneNormal[3] = { 0.0f, 0.0f, -_near * lightSide };
	float tmp[3];
	vec3MulMtx(tmp, lightPlaneNormal, mtxViewInv);
	vec3Sub(lightPlaneNormal, tmp, _lightPos);

	float lenInv = 1.0f / sqrtf(vec3Dot(lightPlaneNormal, lightPlaneNormal));

	lightPlane[0] = lightPlaneNormal[0] * lenInv;
	lightPlane[1] = lightPlaneNormal[1] * lenInv;
	lightPlane[2] = lightPlaneNormal[2] * lenInv;
	lightPlane[3] = -vec3Dot(lightPlaneNormal, _lightPos) * lenInv;
}

bool clipTest(const float* _planes, uint8_t _planeNum, const Mesh& _mesh, const float* _scale, const float* _translate)
{
	float (*volumePlanes)[4] = (float(*)[4])_planes;
	float scale = fmax(fmax(_scale[0], _scale[1]), _scale[2]);

	const GroupArray& groups = _mesh.m_groups;
	for (GroupArray::const_iterator it = groups.begin(), itEnd = groups.end(); it != itEnd; ++it)
	{
		const Group& group = *it;

		Sphere sphere = group.m_sphere;
		sphere.m_center[0] = sphere.m_center[0] * scale + _translate[0];
		sphere.m_center[1] = sphere.m_center[1] * scale + _translate[1];
		sphere.m_center[2] = sphere.m_center[2] * scale + _translate[2];
		sphere.m_radius *= (scale+0.4f);

		bool isInside = true;
		for (uint8_t ii = 0; ii < _planeNum; ++ii)
		{
			const float* plane = volumePlanes[ii];

			float positiveSide = vec3Dot(plane, sphere.m_center) + plane[3] + sphere.m_radius;

			if (positiveSide < 0.0f)
			{
				isInside = false;
				break;
			}
		}

		if (isInside) 
		{
			return true; 
		}
	}

	return false;
}

int _main_(int /*_argc*/, char** /*_argv*/)
{
	ViewState viewState(1280, 720);
	ClearValues clearValues = {0x00000000, 1.0f, 0};

	uint32_t debug = BGFX_DEBUG_TEXT;
	uint32_t reset = BGFX_RESET_VSYNC;

	bgfx::init();
	bgfx::reset(viewState.m_width, viewState.m_height, reset);

	// Enable debug text.
	bgfx::setDebug(debug);

	// Setup root path for binary shaders. Shader binaries are different
	// for each renderer.
	switch (bgfx::getRendererType() )
	{
		default:
		case bgfx::RendererType::Direct3D9:
			s_shaderPath = "shaders/dx9/";
			s_flipV = true;
			break;

		case bgfx::RendererType::Direct3D11:
			s_shaderPath = "shaders/dx11/";
			s_flipV = true;
			break;

		case bgfx::RendererType::OpenGL:
			s_shaderPath = "shaders/glsl/";
			s_flipV = false;
			break;

		case bgfx::RendererType::OpenGLES2:
		case bgfx::RendererType::OpenGLES3:
			s_shaderPath = "shaders/gles/";
			s_flipV = false;
			break;
	}

	// Imgui
	FILE* file = fopen("font/droidsans.ttf", "rb");
	uint32_t size = (uint32_t)fsize(file);
	void* data = malloc(size);
	size_t ignore = fread(data, 1, size, file);
	BX_UNUSED(ignore);
	fclose(file);
	imguiCreate(data, size);

	bgfx::VertexDecl PosNormalTexcoordDecl;
	PosNormalTexcoordDecl.begin();
	PosNormalTexcoordDecl.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float);
	PosNormalTexcoordDecl.add(bgfx::Attrib::Normal,    4, bgfx::AttribType::Uint8, true, true);
	PosNormalTexcoordDecl.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float);
	PosNormalTexcoordDecl.end();

	s_uniforms.init();
	s_uniforms.submitConstUniforms();

	const bgfx::Memory* mem;

	mem = loadTexture("figure-rgba.dds");
	bgfx::TextureHandle figureTex = bgfx::createTexture(mem);

	mem = loadTexture("flare.dds");
	bgfx::TextureHandle flareTex = bgfx::createTexture(mem);

	mem = loadTexture("fieldstone-rgba.dds");
	bgfx::TextureHandle fieldstoneTex = bgfx::createTexture(mem);

	s_stencilRt  = bgfx::createRenderTarget(viewState.m_width, viewState.m_height, BGFX_RENDER_TARGET_COLOR_RGBA8 | BGFX_RENDER_TARGET_DEPTH);

	u_texColor   = bgfx::createUniform("u_texColor",            bgfx::UniformType::Uniform1iv);
	u_texStencil = bgfx::createUniform("u_texStencil",          bgfx::UniformType::Uniform1iv);

	bgfx::ProgramHandle programTextureLightning = loadProgram("vs_shadowvolume_texture_lightning", "fs_shadowvolume_texture_lightning");
	bgfx::ProgramHandle programColorLightning   = loadProgram("vs_shadowvolume_color_lightning",   "fs_shadowvolume_color_lightning"  );
	bgfx::ProgramHandle programColorTexture     = loadProgram("vs_shadowvolume_color_texture",     "fs_shadowvolume_color_texture"    );
	bgfx::ProgramHandle programTexture          = loadProgram("vs_shadowvolume_texture",           "fs_shadowvolume_texture"          );

	bgfx::ProgramHandle programBackBlank        = loadProgram("vs_shadowvolume_svback",  "fs_shadowvolume_svbackblank" ); 
	bgfx::ProgramHandle programSideBlank        = loadProgram("vs_shadowvolume_svside",  "fs_shadowvolume_svsideblank" ); 
	bgfx::ProgramHandle programFrontBlank       = loadProgram("vs_shadowvolume_svfront", "fs_shadowvolume_svfrontblank"); 

	bgfx::ProgramHandle programBackColor        = loadProgram("vs_shadowvolume_svback",  "fs_shadowvolume_svbackcolor" ); 
	bgfx::ProgramHandle programSideColor        = loadProgram("vs_shadowvolume_svside",  "fs_shadowvolume_svsidecolor" ); 
	bgfx::ProgramHandle programFrontColor       = loadProgram("vs_shadowvolume_svfront", "fs_shadowvolume_svfrontcolor"); 

	bgfx::ProgramHandle programSideTex          = loadProgram("vs_shadowvolume_svside",  "fs_shadowvolume_svsidetex"   ); 
	bgfx::ProgramHandle programBackTex1         = loadProgram("vs_shadowvolume_svback",  "fs_shadowvolume_svbacktex1"  ); 
	bgfx::ProgramHandle programBackTex2         = loadProgram("vs_shadowvolume_svback",  "fs_shadowvolume_svbacktex2"  ); 
	bgfx::ProgramHandle programFrontTex1        = loadProgram("vs_shadowvolume_svfront", "fs_shadowvolume_svfronttex1" ); 
	bgfx::ProgramHandle programFrontTex2        = loadProgram("vs_shadowvolume_svfront", "fs_shadowvolume_svfronttex2" ); 

	struct ShadowVolumeProgramType
	{
		enum Enum
		{
			Blank = 0,
			Color,
			Tex1,
			Tex2,

			Count
		};
	};

	struct ShadowVolumePart
	{
		enum Enum
		{
			Back = 0,
			Side,
			Front,

			Count
		};
	};

	bgfx::ProgramHandle svProgs[ShadowVolumeProgramType::Count][ShadowVolumePart::Count] =
	{ 
		 { programBackBlank, programSideBlank, programFrontBlank } // Blank
		,{ programBackColor, programSideColor, programFrontColor } // Color
		,{ programBackTex1,  programSideTex,   programFrontTex1  } // Tex1
		,{ programBackTex2,  programSideTex,   programFrontTex2  } // Tex2
	};

	Model bunnyLowPolyModel;
	Model bunnyHighPolyModel;
	Model columnModel;
	Model platformModel;
	Model cubeModel;
	Model hplaneFieldModel;
	Model hplaneFigureModel;
	Model vplaneModel;

	bunnyHighPolyModel.load("meshes/bunny_patched.bin");
	bunnyHighPolyModel.m_program = programColorLightning;

	bunnyLowPolyModel.load("meshes/bunny_decimated.bin");
	bunnyLowPolyModel.m_program = programColorLightning;

	columnModel.load("meshes/column.bin");
	columnModel.m_program = programColorLightning;

	platformModel.load("meshes/platform.bin");
	platformModel.m_program = programTextureLightning;
	platformModel.m_texture = figureTex;

	cubeModel.load("meshes/cube.bin");
	cubeModel.m_program = programTextureLightning;
	cubeModel.m_texture = figureTex;

	hplaneFieldModel.load(s_hplaneVertices, s_numHPlaneVertices, PosNormalTexcoordDecl, s_planeIndices, s_numPlaneIndices);
	hplaneFieldModel.m_program = programTextureLightning;
	hplaneFieldModel.m_texture = fieldstoneTex;

	hplaneFigureModel.load(s_hplaneVertices, s_numHPlaneVertices, PosNormalTexcoordDecl, s_planeIndices, s_numPlaneIndices);
	hplaneFigureModel.m_program = programTextureLightning;
	hplaneFigureModel.m_texture = figureTex;

	vplaneModel.load(s_vplaneVertices, s_numVPlaneVertices, PosNormalTexcoordDecl, s_planeIndices, s_numPlaneIndices);
	vplaneModel.m_program = programColorTexture;
	vplaneModel.m_texture = flareTex;

	//setup lights
	const uint8_t MAX_NUM_LIGHTS = 5;
	const float rgbInnerR[MAX_NUM_LIGHTS][4] =
	{
		{ 1.0f, 0.7f, 0.2f, 0.0f }, //yellow
		{ 0.7f, 0.2f, 1.0f, 0.0f }, //purple
		{ 0.2f, 1.0f, 0.7f, 0.0f }, //cyan
		{ 1.0f, 0.4f, 0.2f, 0.0f }, //orange
		{ 0.7f, 0.7f, 0.7f, 0.0f }, //white
	};

	float lightRgbInnerR[MAX_NUM_LIGHTS][4];
	for (uint8_t ii = 0, jj = 0; ii < MAX_NUM_LIGHTS; ++ii, ++jj)
	{
		const uint8_t index = jj%MAX_NUM_LIGHTS;
		lightRgbInnerR[ii][0] = rgbInnerR[index][0];
		lightRgbInnerR[ii][1] = rgbInnerR[index][1];
		lightRgbInnerR[ii][2] = rgbInnerR[index][2];
		lightRgbInnerR[ii][3] = rgbInnerR[index][3];
	}

	int64_t profTime = 0;
	int64_t timeOffset = bx::getHPCounter();

	uint32_t numShadowVolumeVertices = 0; 
	uint32_t numShadowVolumeIndices  = 0;

	uint32_t oldWidth = 0;
	uint32_t oldHeight = 0;

	entry::MouseState mouseState;
	while (!entry::processEvents(viewState.m_width, viewState.m_height, debug, reset, &mouseState) )
	{
		//respond properly on resize
		if (oldWidth != viewState.m_width
		||  oldHeight != viewState.m_height)
		{
			oldWidth = viewState.m_width;
			oldHeight = viewState.m_height;

			bgfx::destroyRenderTarget(s_stencilRt);

			s_stencilRt = bgfx::createRenderTarget(viewState.m_width, viewState.m_height, BGFX_RENDER_TARGET_COLOR_RGBA8 | BGFX_RENDER_TARGET_DEPTH);
		}

		//set view and projection matrices
		const float aspect = float(viewState.m_width)/float(viewState.m_height);
		mtxProj(viewState.m_proj, 60.0f, aspect, 1.0f, 1000.0f);
		float at[3] = { 3.0f, 5.0f, 0.0f };
		float eye[3] = { 3.0f, 20.0f, -58.0f };
		mtxLookAt(viewState.m_view, eye, at);

		//time
		int64_t now = bx::getHPCounter();
		static int64_t last = now;
		const int64_t frameTime = now - last;
		last = now;
		const double freq = double(bx::getHPFrequency() );
		const double toMs = 1000.0/freq;
		float time = (float)( (now - timeOffset)/double(bx::getHPFrequency() ) );
		const float deltaTime = float(frameTime/freq);
		s_uniforms.m_time = time;

		//imgui
		static bool settings_showHelp           = false;
		static bool settings_updateLights       = true;
		static bool settings_updateScene        = true;
		static bool settings_mixedSvImpl        = true;
		static bool settings_useStencilTexture  = false;
		static bool settings_drawShadowVolumes  = false;
		static float settings_numLights         = 1.0f;
		static float settings_instanceCount     = 9.0f;
		static ShadowVolumeImpl::Enum      settings_shadowVolumeImpl      = ShadowVolumeImpl::DepthFail; 
		static ShadowVolumeAlgorithm::Enum settings_shadowVolumeAlgorithm = ShadowVolumeAlgorithm::FaceBased;

		static enum LightPattern
		{
			LightPattern0 = 0,
			LightPattern1
		} lightPattern = LightPattern0;

		static enum MeshChoice
		{
			BunnyHighPoly = 0,
			BunnyLowPoly
		} currentMesh = BunnyLowPoly;

		static enum Scene
		{
			Scene0 = 0,
			Scene1,

			SceneCount
		} currentScene = Scene0;

		static std::string titles[2] =
		{
			"Scene 0",
			"Scene 1",
		};

		imguiBeginFrame(mouseState.m_mx
				, mouseState.m_my
				, (mouseState.m_buttons[entry::MouseButton::Left  ] ? IMGUI_MBUT_LEFT  : 0)
				| (mouseState.m_buttons[entry::MouseButton::Right ] ? IMGUI_MBUT_RIGHT : 0)
				, 0
				, viewState.m_width
				, viewState.m_height
				);

		static int32_t scrollAreaRight = 0;
		imguiBeginScrollArea("Settings", viewState.m_width - 256 - 10, 10, 256, 700, &scrollAreaRight);

		if (imguiCheck(titles[Scene0].c_str(), Scene0 == currentScene)) { currentScene = Scene0; } 
		if (imguiCheck(titles[Scene1].c_str(), Scene1 == currentScene)) { currentScene = Scene1; }

		imguiSlider("Lights", &settings_numLights, 1.0f, 5.0f, 1.0f);
		if (imguiCheck("Update lights", settings_updateLights)) { settings_updateLights = !settings_updateLights; }
		imguiIndent();
		if (imguiCheck("Light pattern 0", LightPattern0 == lightPattern, settings_updateLights)) { lightPattern = LightPattern0; }
		if (imguiCheck("Light pattern 1", LightPattern1 == lightPattern, settings_updateLights)) { lightPattern = LightPattern1; }
		imguiUnindent();
		if (imguiCheck("Update scene", settings_updateScene, Scene0 == currentScene)) { settings_updateScene  = !settings_updateScene;  }

		imguiSeparatorLine();
		imguiLabel("Stencil buffer implementation:");
		settings_shadowVolumeImpl = (imguiCheck("Depth fail", ShadowVolumeImpl::DepthFail == settings_shadowVolumeImpl, !settings_mixedSvImpl) ? ShadowVolumeImpl::DepthFail : settings_shadowVolumeImpl);
		settings_shadowVolumeImpl = (imguiCheck("Depth pass", ShadowVolumeImpl::DepthPass == settings_shadowVolumeImpl, !settings_mixedSvImpl) ? ShadowVolumeImpl::DepthPass : settings_shadowVolumeImpl);
		settings_mixedSvImpl = (imguiCheck("Mixed", settings_mixedSvImpl) ? !settings_mixedSvImpl : settings_mixedSvImpl);

		imguiLabel("Shadow volume implementation:");
		settings_shadowVolumeAlgorithm = (imguiCheck("Face based impl.", ShadowVolumeAlgorithm::FaceBased == settings_shadowVolumeAlgorithm) ? ShadowVolumeAlgorithm::FaceBased : settings_shadowVolumeAlgorithm);
		settings_shadowVolumeAlgorithm = (imguiCheck("Edge based impl.", ShadowVolumeAlgorithm::EdgeBased == settings_shadowVolumeAlgorithm) ? ShadowVolumeAlgorithm::EdgeBased : settings_shadowVolumeAlgorithm);

		imguiLabel("Stencil:");
		if (imguiCheck("Use stencil buffer", !settings_useStencilTexture))
		{
			if (settings_useStencilTexture) { settings_useStencilTexture = false; }
		}
		if (imguiCheck("Use texture as stencil", settings_useStencilTexture))
		{
			if (!settings_useStencilTexture) { settings_useStencilTexture = true; }
		}

		imguiSeparatorLine();
		imguiLabel("Mesh:");
		if (imguiCheck("Bunny - high poly", BunnyHighPoly == currentMesh)) { currentMesh = BunnyHighPoly; }
		if (imguiCheck("Bunny - low poly",  BunnyLowPoly  == currentMesh)) { currentMesh = BunnyLowPoly;  }
		if (Scene1 == currentScene) { imguiSlider("Instance count", &settings_instanceCount, 1.0f, 49.0f, 1.0f); }

		imguiLabel("CPU Time: %7.1f [ms]", double(profTime)*toMs);
		imguiLabel("Volume Vertices: %5.uk", numShadowVolumeVertices/1000);
		imguiLabel("Volume Indices: %6.uk", numShadowVolumeIndices/1000);
		numShadowVolumeVertices = 0; 
		numShadowVolumeIndices = 0;  

		imguiSeparatorLine();
		settings_drawShadowVolumes = (imguiCheck("Draw Shadow Volumes", settings_drawShadowVolumes) ? !settings_drawShadowVolumes : settings_drawShadowVolumes);
		imguiIndent();
		imguiUnindent();

		imguiEndScrollArea();

		static int32_t scrollAreaLeft = 0;
		imguiBeginScrollArea("Show help:", 10, viewState.m_height - 77 - 10, 120, 77, &scrollAreaLeft);
		settings_showHelp = (imguiButton(settings_showHelp ? "ON" : "OFF") ? !settings_showHelp : settings_showHelp);
		imguiEndScrollArea();

		imguiEndFrame();

		//update settings
		s_uniforms.m_params.m_ambientPass     = 1.0f;
		s_uniforms.m_params.m_lightningPass   = 1.0f;
		s_uniforms.m_params.m_lightCount      = settings_numLights;
		s_uniforms.m_svparams.m_useStencilTex = float(settings_useStencilTexture);
		s_uniforms.submitPerFrameUniforms();
		
		//set picked bunny model
		Model* bunnyModel = BunnyLowPoly == currentMesh ? &bunnyLowPolyModel : &bunnyHighPolyModel;

		//update time accumulators
		static float sceneTimeAccumulator = 0.0f;
		if (settings_updateScene)
		{
			sceneTimeAccumulator += deltaTime;
		}

		static float lightTimeAccumulator = 0.0f;
		if (settings_updateLights)
		{
			lightTimeAccumulator += deltaTime;
		}

		//setup light positions
		float lightPosRadius[MAX_NUM_LIGHTS][4];
		switch (lightPattern)
		{
			case LightPattern0:
				for (uint8_t ii = 0; ii < settings_numLights; ++ii)
				{
					lightPosRadius[ii][0] = cos(2.0f*float(M_PI)/settings_numLights * float(ii) + lightTimeAccumulator * 1.1f + 3.0f) * 20.0f;
					lightPosRadius[ii][1] = 20.0f;
					lightPosRadius[ii][2] = sin(2.0f*float(M_PI)/settings_numLights * float(ii) + lightTimeAccumulator * 1.1f + 3.0f) * 20.0f;
					lightPosRadius[ii][3] = 20.0f;
				}
				break;
			case LightPattern1:
				for (uint8_t ii = 0; ii < settings_numLights; ++ii)
				{
					lightPosRadius[ii][0] = cos(float(ii) * 2.0f/settings_numLights + lightTimeAccumulator * 1.3f + float(M_PI)) * 40.0f;
					lightPosRadius[ii][1] = 20.0f;
					lightPosRadius[ii][2] = sin(float(ii) * 2.0f/settings_numLights + lightTimeAccumulator * 1.3f + float(M_PI)) * 40.0f;
					lightPosRadius[ii][3] = 20.0f;
				}
				break;
		}

		//use debug font to print information about this example.
		bgfx::dbgTextClear();
		bgfx::dbgTextPrintf(0, 1, 0x4f, "bgfx/examples/14-shadowvolumes");
		bgfx::dbgTextPrintf(0, 2, 0x6f, "Description: Shadow volumes.");
		bgfx::dbgTextPrintf(0, 3, 0x0f, "Frame: % 7.3f[ms]", double(frameTime)*toMs);
		
		if (settings_showHelp)
		{
			uint8_t row = 5;
			bgfx::dbgTextPrintf(3, row++, 0x0f, "Stencil buffer implementation:");
			bgfx::dbgTextPrintf(8, row++, 0x0f, "Depth fail - Robust, but slower than 'Depth pass'. Requires computing and drawing of shadow volume caps.");
			bgfx::dbgTextPrintf(8, row++, 0x0f, "Depth pass - Faster, but not stable. Shadows are wrong when camera is in the shadow.");
			bgfx::dbgTextPrintf(8, row++, 0x0f, "Mixed      - 'Depth pass' where possible, 'Depth fail' where necessary. Best of both words.");

			row++;
			bgfx::dbgTextPrintf(3, row++, 0x0f, "Shadow volume implementation:");
			bgfx::dbgTextPrintf(8, row++, 0x0f, "Face Based - Slower. Works fine with either stencil buffer or texture as stencil.");
			bgfx::dbgTextPrintf(8, row++, 0x0f, "Edge Based - Faster, but requires +2 incr/decr on stencil buffer. To avoid massive redraw, use RGBA texture as stencil.");

			row++;
			bgfx::dbgTextPrintf(3, row++, 0x0f, "Stencil:");
			bgfx::dbgTextPrintf(8, row++, 0x0f, "Stencil buffer     - Faster, but capable only of +1 incr.");
			bgfx::dbgTextPrintf(8, row++, 0x0f, "Texture as stencil - Slower, but capable of +2 incr.");
		}                       
								
		//-------------------------------------------------
		// Setup instances
		//-------------------------------------------------

		Instance shadowCasters[SceneCount][60];
		uint16_t shadowCastersCount[SceneCount];
		for (uint8_t ii = 0; ii < SceneCount; ++ii)
		{
			shadowCastersCount[ii] = 0;
		}

		Instance shadowReceivers[SceneCount][10];
		uint16_t shadowReceiversCount[SceneCount];
		for (uint8_t ii = 0; ii < SceneCount; ++ii)
		{
			shadowReceiversCount[ii] = 0;
		}

		/**
		 * Scene 0 - shadow casters
		 */
		
		//bunny
		{
			Instance& inst = shadowCasters[Scene0][shadowCastersCount[Scene0]++];
			inst.m_scale[0]    = 5.0f;
			inst.m_scale[1]    = 5.0f;
			inst.m_scale[2]    = 5.0f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = float(4.0f - sceneTimeAccumulator * 0.7f);
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = 0.0f;
			inst.m_pos[1]      = 10.0f;
			inst.m_pos[2]      = 0.0f;
			inst.m_color[0]    = 0.68f;
			inst.m_color[1]    = 0.65f;
			inst.m_color[2]    = 0.60f;
			inst.m_model       = bunnyModel;
		}

		//cubes top
		const uint8_t numCubesTop = 9;
		for (uint16_t ii = 0; ii < numCubesTop; ++ii)
		{
			Instance& inst = shadowCasters[Scene0][shadowCastersCount[Scene0]++];
			inst.m_scale[0]    = 1.0f;
			inst.m_scale[1]    = 1.0f;
			inst.m_scale[2]    = 1.0f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = 0.0f;
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = sin(ii * 2.0f + 13.0f + sceneTimeAccumulator * 1.1f) * 13.0f;
			inst.m_pos[1]      = 6.0f;
			inst.m_pos[2]      = cos(ii * 2.0f + 13.0f + sceneTimeAccumulator * 1.1f) * 13.0f;
			inst.m_model       = &cubeModel;
		}

		//cubes bottom
		const uint8_t numCubesBottom = 9;
		for (uint16_t ii = 0; ii < numCubesBottom; ++ii)
		{
			Instance& inst = shadowCasters[Scene0][shadowCastersCount[Scene0]++];
			inst.m_scale[0]    = 1.0f;
			inst.m_scale[1]    = 1.0f;
			inst.m_scale[2]    = 1.0f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = 0.0f;
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = sin(ii * 2.0f + 13.0f + sceneTimeAccumulator * 1.1f) * 13.0f;
			inst.m_pos[1]      = 22.0f;
			inst.m_pos[2]      = cos(ii * 2.0f + 13.0f + sceneTimeAccumulator * 1.1f) * 13.0f;
			inst.m_model       = &cubeModel;
		}

		//columns
		const float dist = 16.0f;
		const float columnPositions[4][3] =
		{
			{  dist, 3.3f,  dist },
			{ -dist, 3.3f,  dist },
			{  dist, 3.3f, -dist },
			{ -dist, 3.3f, -dist },
		};

		for (uint8_t ii = 0; ii < 4; ++ii)
		{
			Instance& inst = shadowCasters[Scene0][shadowCastersCount[Scene0]++];
			inst.m_scale[0]    = 1.5f;
			inst.m_scale[1]    = 1.5f;
			inst.m_scale[2]    = 1.5f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = 1.57f;
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = columnPositions[ii][0];
			inst.m_pos[1]      = columnPositions[ii][1];
			inst.m_pos[2]      = columnPositions[ii][2];
			inst.m_color[0]    = 0.25f;
			inst.m_color[1]    = 0.25f;
			inst.m_color[2]    = 0.25f;
			inst.m_model       = &columnModel;
		}

		//ceiling
		{
			Instance& inst = shadowCasters[Scene0][shadowCastersCount[Scene0]++];
			inst.m_scale[0]    = 21.0f;
			inst.m_scale[1]    = 21.0f;
			inst.m_scale[2]    = 21.0f;
			inst.m_rotation[0] = float(M_PI);
			inst.m_rotation[1] = 0.0f;
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = 0.0f;
			inst.m_pos[1]      = 28.2f;
			inst.m_pos[2]      = 0.0f;
			inst.m_model       = &platformModel;
			inst.m_svExtrusionDistance = 2.0f; //prevent culling on tight view frustum
		}
		
		//platform
		{
			Instance& inst = shadowCasters[Scene0][shadowCastersCount[Scene0]++];
			inst.m_scale[0]    = 24.0f;
			inst.m_scale[1]    = 24.0f;
			inst.m_scale[2]    = 24.0f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = 0.0f;
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = 0.0f;
			inst.m_pos[1]      = 0.0f;
			inst.m_pos[2]      = 0.0f;
			inst.m_model       = &platformModel;
			inst.m_svExtrusionDistance = 2.0f; //prevent culling on tight view frustum
		}

		/**
		 * Scene 0 - shadow receivers
		 */

		//floor
		{
			Instance& inst = shadowReceivers[Scene0][shadowReceiversCount[Scene0]++];
			inst.m_scale[0]    = 500.0f;
			inst.m_scale[1]    = 500.0f;
			inst.m_scale[2]    = 500.0f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = 0.0f;
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = 0.0f;
			inst.m_pos[1]      = 0.0f;
			inst.m_pos[2]      = 0.0f;
			inst.m_model       = &hplaneFieldModel;
		}

		/**
		 * Scene 1 - shadow casters
		 */

		//bunny instances
		enum Direction
		{
			Left = 0,
			Down,
			Right,
			Up,

			DirectionCount,
		};
		uint8_t currentDirection = Left;
		float currX = 0.0f;
		float currY = 0.0f;
		const float stepX = 20.0f;
		const float stepY = 20.0f;
		uint8_t stateStep = 0;
		float stateChange = 1.0f;

		for (uint8_t ii = 0; ii < settings_instanceCount; ++ii)
		{
			Instance& inst = shadowCasters[Scene1][shadowCastersCount[Scene1]++];
			inst.m_scale[0]    = 5.0f;
			inst.m_scale[1]    = 5.0f;
			inst.m_scale[2]    = 5.0f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = float(M_PI);
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = currX;
			inst.m_pos[1]      = 0.0f;
			inst.m_pos[2]      = currY;
			inst.m_model       = bunnyModel;

			stateStep++;
			if (stateStep >= floor(stateChange/2.0f))
			{
				currentDirection = (currentDirection+1)%DirectionCount;
				stateStep = 0;
				stateChange += 1.0f;
			}

			switch (currentDirection)
			{
				case Left:  currX -= stepX; break;
				case Down:  currY -= stepY; break;
				case Right: currX += stepX; break;
				case Up:    currY += stepY; break;
				default: break;
			}
		}

		/**
		 * Scene 1 - shadow receivers
		 */

		//floor
		{
			Instance& inst = shadowReceivers[Scene1][shadowReceiversCount[Scene1]++];
			inst.m_scale[0]    = 500.0f;
			inst.m_scale[1]    = 500.0f;
			inst.m_scale[2]    = 500.0f;
			inst.m_rotation[0] = 0.0f;
			inst.m_rotation[1] = 0.0f;
			inst.m_rotation[2] = 0.0f;
			inst.m_pos[0]      = 0.0f;
			inst.m_pos[1]      = 0.0f;
			inst.m_pos[2]      = 0.0f;
			inst.m_model       = &hplaneFigureModel;
		}

		//-------------------------------------------------
		// Render
		//-------------------------------------------------

#define VIEWID_RANGE1_PASS0     1 
#define VIEWID_RANGE1_RT_PASS1  2
#define VIEWID_RANGE15_PASS2    3
#define VIEWID_RANGE1_PASS3    20

		//make sure at the beginning everything gets cleared
		::clearView(0, BGFX_CLEAR_COLOR_BIT | BGFX_CLEAR_DEPTH_BIT | BGFX_CLEAR_STENCIL_BIT, clearValues);
		::submit(0);

		/**
		 * Draw ambient
		 */

		//draw ambient only
		s_uniforms.m_params.m_ambientPass = 1.0f;
		s_uniforms.m_params.m_lightningPass = 0.0f;

		//white bunny and columns
		s_uniforms.m_color[0] = 1.0f;
		s_uniforms.m_color[1] = 1.0f;
		s_uniforms.m_color[2] = 1.0f;

		const RenderState& drawAmbient = (settings_useStencilTexture ?
				s_renderStates[RenderState::ShadowVolume_UsingStencilTexture_DrawAmbient]:
				s_renderStates[RenderState::ShadowVolume_UsingStencilBuffer_DrawAmbient]);

		//draw shadow casters
		for (uint8_t ii = 0; ii < shadowCastersCount[currentScene]; ++ii)
		{
			shadowCasters[currentScene][ii].submit(VIEWID_RANGE1_PASS0, drawAmbient);
		}

		//draw shadow receivers
		for (uint8_t ii = 0; ii < shadowReceiversCount[currentScene]; ++ii)
		{
			shadowReceivers[currentScene][ii].submit(VIEWID_RANGE1_PASS0, drawAmbient);
		}

		//using stencil texture requires rendering to separate render target. first pass is building depth buffer
		if (settings_useStencilTexture)
		{
			ClearValues cv = { 0x00000000, 1.0f, 0 };
			::clearView(VIEWID_RANGE1_RT_PASS1, BGFX_CLEAR_DEPTH_BIT, cv);
			::setViewRenderTarget(VIEWID_RANGE1_RT_PASS1, s_stencilRt);

			const RenderState& renderState = s_renderStates[RenderState::ShadowVolume_UsingStencilTexture_BuildDepth]; 

			for (uint8_t ii = 0; ii < shadowCastersCount[currentScene]; ++ii)
			{
				shadowCasters[currentScene][ii].submit(VIEWID_RANGE1_RT_PASS1, renderState);
			}

			for (uint8_t ii = 0; ii < shadowReceiversCount[currentScene]; ++ii)
			{
				shadowReceivers[currentScene][ii].submit(VIEWID_RANGE1_RT_PASS1, renderState);
			}
		}

		//start performance timer
		profTime = bx::getHPCounter();

		/**
		 * For each light: 
		 * 1. Compute and draw shadow volume to stencil buffer
		 * 2. Draw diffuse with stencil test
		 */
		for (uint8_t ii = 0, viewId = VIEWID_RANGE15_PASS2; ii < settings_numLights; ++ii, ++viewId)
		{
			const float* lightPos = lightPosRadius[ii];

			//set uniform data
			memcpy(s_uniforms.m_lightPosRadius, lightPosRadius[ii], 4*sizeof(float));
			memcpy(s_uniforms.m_lightRgbInnerR, lightRgbInnerR[ii], 3*sizeof(float));
			memcpy(s_uniforms.m_color,          lightRgbInnerR[ii], 3*sizeof(float));

			if (settings_useStencilTexture)
			{
				ClearValues cv = { 0x00000000, 1.0f, 0 };
				::clearView(viewId, BGFX_CLEAR_COLOR_BIT, cv);
				::setViewRenderTarget(viewId, s_stencilRt);
			}
			else
			{
				::clearView(viewId, BGFX_CLEAR_STENCIL_BIT, clearValues);
			}

			//create near clip volume for current light
			float nearClipVolume[6 * 4];
			float pointLight[4];
			if (settings_mixedSvImpl)
			{
				pointLight[0] = lightPos[0];
				pointLight[1] = lightPos[1];
				pointLight[2] = lightPos[2];
				pointLight[3] = 1.0f;
				createNearClipVolume(nearClipVolume, pointLight, viewState.m_view, 60.0f, 16.0f/9.0f, 0.1f);
			}

			for (uint8_t jj = 0; jj < shadowCastersCount[currentScene]; ++jj)
			{
				const Instance& instance = shadowCasters[currentScene][jj];
				Model* model = instance.m_model;

				ShadowVolumeImpl::Enum shadowVolumeImpl = settings_shadowVolumeImpl; 
				if (settings_mixedSvImpl)
				{                                                     
					//if instance is inside near clip volume, depth fail must be used, else depth pass is fine.
					bool isInsideVolume = clipTest(nearClipVolume, 6, model->m_mesh, instance.m_scale, instance.m_pos);
					shadowVolumeImpl = (isInsideVolume ? ShadowVolumeImpl::DepthFail : ShadowVolumeImpl::DepthPass);
				}
				s_uniforms.m_svparams.m_dfail = float(ShadowVolumeImpl::DepthFail == shadowVolumeImpl); 

				//compute transform for shadow volume
				float shadowVolumeMtx[16];
				float transformedLightPos[3];
				shadowVolumeTransform(shadowVolumeMtx
						, transformedLightPos
						, instance.m_scale
						, instance.m_rotation
						, instance.m_pos
						, lightPos
						);

				//set virtual light pos
				memcpy(s_uniforms.m_virtualLightPos_extrusionDist, transformedLightPos, 3*sizeof(float));
				s_uniforms.m_virtualLightPos_extrusionDist[3] = instance.m_svExtrusionDistance;

				GroupArray& groups = model->m_mesh.m_groups;
				const uint16_t stride = model->m_mesh.m_decl.getStride();
				for (GroupArray::iterator it = groups.begin(), itEnd = groups.end(); it != itEnd; ++it)
				{
					Group& group = *it;

					//create shadow volume
					ShadowVolume shadowVolume;
					shadowVolumeCreate(shadowVolume
							, group
							, stride
							, shadowVolumeMtx
							, transformedLightPos
							, shadowVolumeImpl
							, settings_shadowVolumeAlgorithm
							, settings_useStencilTexture
							);

					//update display information
					numShadowVolumeVertices += shadowVolume.m_numVertices;
					numShadowVolumeIndices += shadowVolume.m_numIndices;

					//figure out which render state to use
					RenderState::Enum renderStateIndex;
					if (settings_useStencilTexture)
					{
						renderStateIndex = (ShadowVolumeImpl::DepthFail == shadowVolumeImpl ?
								RenderState::ShadowVolume_UsingStencilTexture_CraftStencil_DepthFail :
								RenderState::ShadowVolume_UsingStencilTexture_CraftStencil_DepthPass);
					}
					else
					{
						renderStateIndex = (ShadowVolumeImpl::DepthFail == shadowVolumeImpl ?
								RenderState::ShadowVolume_UsingStencilBuffer_CraftStencil_DepthFail :
								RenderState::ShadowVolume_UsingStencilBuffer_CraftStencil_DepthPass);
					}
					const RenderState& renderStateCraftStencil = s_renderStates[renderStateIndex];

					//figure out which program to use
					ShadowVolumeProgramType::Enum programIndex = ShadowVolumeProgramType::Blank;
					if (settings_useStencilTexture)
					{
						programIndex = (ShadowVolumeAlgorithm::FaceBased == settings_shadowVolumeAlgorithm ?
								ShadowVolumeProgramType::Tex1 :
								ShadowVolumeProgramType::Tex2);
					}

					//sides
					s_uniforms.submitPerDrawUniforms();
					bgfx::setProgram(svProgs[programIndex][ShadowVolumePart::Side]);
					bgfx::setTransform(shadowVolumeMtx);
					bgfx::setVertexBuffer(shadowVolume.m_vbSides);
					bgfx::setIndexBuffer(shadowVolume.m_ibSides);
					::setRenderState(renderStateCraftStencil);
					::submit(viewId);

					if (shadowVolume.m_cap)
					{
						//front
						s_uniforms.submitPerDrawUniforms();
						bgfx::setProgram(svProgs[programIndex][ShadowVolumePart::Front]);
						bgfx::setTransform(shadowVolumeMtx);
						bgfx::setVertexBuffer(group.m_vbh);
						bgfx::setIndexBuffer(shadowVolume.m_ibFrontCap);
						::setRenderState(renderStateCraftStencil);
						::submit(viewId);

						//back
						s_uniforms.submitPerDrawUniforms();
						bgfx::setProgram(svProgs[programIndex][ShadowVolumePart::Back]);
						bgfx::setTransform(shadowVolumeMtx);
						bgfx::setVertexBuffer(group.m_vbh);
						bgfx::setIndexBuffer(shadowVolume.m_ibBackCap);
						::setRenderState(renderStateCraftStencil);
						::submit(viewId);
					}

					if (settings_drawShadowVolumes)
					{
						const RenderState& renderState = s_renderStates[RenderState::Custom_DrawShadowVolume_Lines];

						//sides
						s_uniforms.submitPerDrawUniforms();
						bgfx::setProgram(svProgs[ShadowVolumeProgramType::Color][ShadowVolumePart::Side]);
						bgfx::setTransform(shadowVolumeMtx);
						bgfx::setVertexBuffer(shadowVolume.m_vbSides);
						bgfx::setIndexBuffer(shadowVolume.m_ibSides);
						::setRenderState(renderState);
						::submit(VIEWID_RANGE1_PASS3);

						if (shadowVolume.m_cap)
						{
							//front
							s_uniforms.submitPerDrawUniforms();
							bgfx::setProgram(svProgs[ShadowVolumeProgramType::Color][ShadowVolumePart::Front]);
							bgfx::setTransform(shadowVolumeMtx);
							bgfx::setVertexBuffer(group.m_vbh);
							bgfx::setIndexBuffer(shadowVolume.m_ibFrontCap);
							::setRenderState(renderState);
							::submit(VIEWID_RANGE1_PASS3);

							//back
							s_uniforms.submitPerDrawUniforms();
							bgfx::setProgram(svProgs[ShadowVolumeProgramType::Color][ShadowVolumePart::Back]);
							bgfx::setTransform(shadowVolumeMtx);
							bgfx::setVertexBuffer(group.m_vbh);
							bgfx::setIndexBuffer(shadowVolume.m_ibBackCap);
							::setRenderState(renderState);
							::submit(VIEWID_RANGE1_PASS3);
						}
					}
				}
			}

			/**
			 * Draw diffuse
			 */

			//draw diffuse only
			s_uniforms.m_params.m_ambientPass = 0.0f;
			s_uniforms.m_params.m_lightningPass = 1.0f;

			RenderState& drawDiffuse = (settings_useStencilTexture ?
					s_renderStates[RenderState::ShadowVolume_UsingStencilTexture_DrawDiffuse] :
					s_renderStates[RenderState::ShadowVolume_UsingStencilBuffer_DrawDiffuse]);

			//if using stencil texture, viewId is set to render target. Incr it to render to default back buffer
			viewId += uint8_t(settings_useStencilTexture);

			//draw shadow casters
			for (uint8_t ii = 0; ii < shadowCastersCount[currentScene]; ++ii)
			{
				shadowCasters[currentScene][ii].submit(viewId, drawDiffuse);
			}

			//draw shadow receivers
			for (uint8_t ii = 0; ii < shadowReceiversCount[currentScene]; ++ii)
			{
				shadowReceivers[currentScene][ii].submit(viewId, drawDiffuse);
			}
		}

		//end performance timer
		profTime = bx::getHPCounter() - profTime; 

		//lights
		const float lightScale[3] = { 1.5f, 1.5f, 1.5f };
		for (uint8_t ii = 0; ii < settings_numLights; ++ii)
		{
			//set color
			memcpy(s_uniforms.m_color, lightRgbInnerR[ii], 3*sizeof(float));

			//calculate mtx
			float lightMtx[16];
			mtxBillboard(lightMtx, viewState.m_view, lightPosRadius[ii], lightScale);

			//submit
			vplaneModel.submit(VIEWID_RANGE1_PASS3, lightMtx, s_renderStates[RenderState::Custom_BlendLightTexture]);
		}

		//setup view rect and transform for all used views
		setViewRectTransformMask(s_viewMask, viewState);
		s_viewMask = 0;

		// Advance to next frame. Rendering thread will be kicked to
		// process submitted rendering primitives.
		bgfx::frame();

		//reset clear values on used views
		clearViewMask(s_clearMask, BGFX_CLEAR_NONE, clearValues);
		s_clearMask = 0;

		//reset assigned render target views
		const bgfx::RenderTargetHandle invalidHandle = BGFX_INVALID_HANDLE;
		bgfx::setViewRenderTargetMask(s_rtMask, invalidHandle);
		s_rtMask = 0;
	}

	// Cleanup
	bunnyLowPolyModel.unload();
	bunnyHighPolyModel.unload();
	columnModel.unload();
	cubeModel.unload();
	platformModel.unload();
	hplaneFieldModel.unload();
	hplaneFigureModel.unload();
	vplaneModel.unload();

	s_uniforms.destroy();

	bgfx::destroyUniform(u_texColor);
	bgfx::destroyUniform(u_texStencil);
	bgfx::destroyRenderTarget(s_stencilRt);

	bgfx::destroyTexture(figureTex);
	bgfx::destroyTexture(fieldstoneTex);
	bgfx::destroyTexture(flareTex);

	bgfx::destroyProgram(programTextureLightning);
	bgfx::destroyProgram(programColorLightning);
	bgfx::destroyProgram(programColorTexture);
	bgfx::destroyProgram(programTexture);

	bgfx::destroyProgram(programBackBlank);
	bgfx::destroyProgram(programSideBlank);
	bgfx::destroyProgram(programFrontBlank);
	bgfx::destroyProgram(programBackColor);
	bgfx::destroyProgram(programSideColor);
	bgfx::destroyProgram(programFrontColor);
	bgfx::destroyProgram(programSideTex);
	bgfx::destroyProgram(programBackTex1);
	bgfx::destroyProgram(programBackTex2);
	bgfx::destroyProgram(programFrontTex1);
	bgfx::destroyProgram(programFrontTex2);

	imguiDestroy();

	// Shutdown bgfx.
	bgfx::shutdown();

	return 0;
}