/*
 * Wolfenstein: Enemy Territory GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
 *
 * Quake3e GPL Source Code (Vulkan backend integration)
 * Copyright (C) 2016 Eugene
 *
 * ET: Legacy
 * Copyright (C) 2012-2024 ET:Legacy team <mail@etlegacy.com>
 *
 * This file is part of ET: Legacy - http://www.etlegacy.com
 *
 * ET: Legacy is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ET: Legacy is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ET: Legacy. If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, Wolfenstein: Enemy Territory GPL Source Code is also
 * subject to certain additional terms. You should have received a copy
 * of these additional terms immediately following the terms and conditions
 * of the GNU General Public License which accompanied the source code.
 * If not, please request a copy in writing from id Software at the address below.
 *
 * id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
 */
/**
 * @file renderer_vk/tr_backend.c
 */

#include "tr_local.h"

backEndData_t  *backEndData;
backEndState_t backEnd;

/**
 * @brief GL_Bind
 * @param[in,out] image
 */
void GL_Bind(image_t *image)
{
	if (!image)
	{
		Ren_Warning("GL_Bind: NULL image\n");
		image = tr.defaultImage;
	}

	if (r_noBind->integer && tr.dlightImage)            // performance evaluation option
	{
		image = tr.dlightImage;
	}

	image->frameUsed = tr.frameCount;
	vk_update_descriptor(glState.currenttmu + VK_DESC_TEXTURE_BASE, image->descriptor);
}

/**
 * @brief GL_SelectTexture
 * @param[in] unit
 */
void GL_SelectTexture(int unit)
{
	if (unit < 0 || unit >= NUM_TEXTURE_BUNDLES)
	{
		Ren_Drop("GL_SelectTexture: unit = %i", unit);
	}

	glState.currenttmu = unit;
}

/**
 * @brief GL_Cull
 * @param[in] cullType
 *
 * @note Mirrored views are handled by the pipeline definition
 */
void GL_Cull(int cullType)
{
	glState.faceCulling = cullType;
}

/**
 * @brief GL_TexEnv
 * @param[in] env
 */
void GL_TexEnv(int env)
{
	switch (env)
	{
	case GL_MODULATE:
	case GL_REPLACE:
	case GL_DECAL:
	case GL_ADD:
		glState.texEnv[glState.currenttmu] = env;
		break;
	default:
		Ren_Drop("GL_TexEnv: invalid env '%d' passed\n", env);
	}
}

/**
 * @brief This routine is responsible for setting the most commonly changed state in Q3.
 * @param[in] stateBits
 *
 * @note With Vulkan the state is part of the pipeline, it's only recorded here
 * and used to select the pipeline on the next draw.
 */
void GL_State(unsigned long stateBits)
{
	glState.glStateBits = stateBits;
}

/**
 * @brief Enable or disable polygon offset for the next draws
 * @param[in] enable
 */
void GL_PolygonOffset(qboolean enable)
{
	glState.polygonOffset = enable;
}

#define PIPELINE_CACHE_SIZE  4096 // must be a power of two
#define PIPELINE_CACHE_PROBE 16

/**
 * @struct pipelineCacheEntry_t
 * @brief Maps a pipeline definition to its index in vk.pipelines
 */
typedef struct
{
	Vk_Pipeline_Def def;
	uint32_t index;
	qboolean used;
} pipelineCacheEntry_t;

static pipelineCacheEntry_t pipelineCache[PIPELINE_CACHE_SIZE];

/**
 * @brief Hash table lookup on top of vk_find_pipeline_ext(), which is a linear search
 * @param[in] def must be cleared with memset before being filled
 * @return pipeline index
 */
uint32_t RB_FindPipeline(const Vk_Pipeline_Def *def)
{
	const byte           *p = (const byte *)def;
	pipelineCacheEntry_t *entry;
	uint32_t             hash = 2166136261u;
	uint32_t             i;

	for (i = 0; i < sizeof(*def); i++)
	{
		hash = (hash ^ p[i]) * 16777619u;
	}

	for (i = 0; i < PIPELINE_CACHE_PROBE; i++)
	{
		entry = &pipelineCache[(hash + i) & (PIPELINE_CACHE_SIZE - 1)];

		if (!entry->used)
		{
			entry->used  = qtrue;
			entry->def   = *def;
			entry->index = vk_find_pipeline_ext(0, def, qfalse);
			return entry->index;
		}

		if (!memcmp(&entry->def, def, sizeof(*def)))
		{
			// pipelines can be released on map change, revalidate the index
			if (entry->index >= vk.pipelines_count || memcmp(&vk.pipelines[entry->index].def, def, sizeof(*def)))
			{
				entry->index = vk_find_pipeline_ext(0, def, qfalse);
			}
			return entry->index;
		}
	}

	return vk_find_pipeline_ext(0, def, qfalse);
}

/**
 * @brief Clear the pipeline lookup table
 */
void RB_ClearPipelineCache(void)
{
	Com_Memset(pipelineCache, 0, sizeof(pipelineCache));
}

/**
 * @brief Upload the parameters of the enabled distance fog
 * @return 0 when no fog applies, else the fog mode of the pipeline (1 GL_LINEAR, 2 GL_EXP)
 */
static int RB_SetupFogUniform(void)
{
	const glfog_t *fog = fogCurrent;
	const float   *m   = vk_world.modelview_transform;
	vkUniform_t   uniform;
	float         end;

	if (!fogIsOn || !fog || backEnd.projection2D)
	{
		return 0;
	}

	Com_Memset(&uniform, 0, sizeof(uniform));

	// eye-space depth of the vertexes, like the fixed-function OpenGL fog
	Vector4Set(uniform.fogDistanceVector, -m[2], -m[6], -m[10], -m[14]);
	Vector4Copy(fog->color, uniform.fogColor);

	if (fog->mode == GL_EXP)
	{
		uniform.fogDepthVector[2] = fog->density;
		vk_push_uniform(&uniform);
		return 2;
	}

	// allow override for helping level designers test fog distances
	end = (r_zFar->value != 0.f) ? r_zFar->value : fog->end;

	uniform.fogDepthVector[0] = end;
	uniform.fogDepthVector[1] = (end != fog->start) ? 1.0f / (end - fog->start) : 1.0e6f;
	vk_push_uniform(&uniform);
	return 1;
}

/**
 * @brief Find the pipeline matching the current GL-like state
 * @param[in] numTextures 1 or 2, the second texture is combined according to the texture env of unit 1
 * @param[in] primitives
 * @return pipeline index
 */
uint32_t RB_StatePipeline(int numTextures, Vk_Primitive_Topology primitives)
{
	Vk_Pipeline_Def def;

	Com_Memset(&def, 0, sizeof(def));

	if (numTextures > 1)
	{
		def.shader_type = (glState.texEnv[1] == GL_ADD) ? TYPE_MULTI_TEXTURE_ADD2_1_1 : TYPE_MULTI_TEXTURE_MUL2;
	}
	else
	{
		def.shader_type = TYPE_SIGNLE_TEXTURE;
	}

	def.state_bits     = glState.glStateBits;
	def.face_culling   = glState.faceCulling;
	def.polygon_offset = glState.polygonOffset;
	def.mirror         = (!backEnd.projection2D && backEnd.viewParms.isMirror) ? qtrue : qfalse;
	def.primitives     = primitives;
	def.global_fog     = RB_SetupFogUniform();

	return RB_FindPipeline(&def);
}

/**
 * @brief Draw the current tess vertexes with the current GL-like state
 *
 * Vertex data is taken from tess.xyz, tess.svars.colors and tess.svars.texcoordPtr[].
 *
 * @param[in] numTextures number of texture units in use (1 or 2)
 * @param[in] numIndexes number of indexes, 0 to draw the vertexes without indexes
 * @param[in] indexes
 */
void RB_DrawElements(int numTextures, int numIndexes, const glIndex_t *indexes)
{
	uint32_t flags = TESS_XYZ | TESS_RGBA0 | TESS_ST0;

	if (numTextures > 1)
	{
		flags |= TESS_ST1;
	}

	vk_bind_pipeline(RB_StatePipeline(numTextures, TRIANGLE_LIST));
	if (numIndexes)
	{
		vk_bind_index_ext(numIndexes, indexes);
	}
	vk_bind_geometry(flags);
	vk_draw_geometry(tess.depthRange, numIndexes ? qtrue : qfalse);
}

/**
 * @brief Draw tess.numVertexes vertexes as lines or points with a solid color
 *
 * Replaces the debug drawing done with glBegin()/glEnd().
 *
 * @param[in] primitives LINE_LIST, POINT_LIST or TRIANGLE_STRIP
 * @param[in] color
 * @param[in] depthRange
 */
void RB_DrawDebugPrimitives(Vk_Primitive_Topology primitives, const vec4_t color, Vk_Depth_Range depthRange)
{
	Vk_Pipeline_Def def;
	byte            c[4];
	int             i;

	if (!tess.numVertexes)
	{
		return;
	}

	c[0] = (byte)(color[0] * 255.f);
	c[1] = (byte)(color[1] * 255.f);
	c[2] = (byte)(color[2] * 255.f);
	c[3] = (byte)(color[3] * 255.f);

	for (i = 0; i < tess.numVertexes; i++)
	{
		tess.svars.colors[i][0] = c[0];
		tess.svars.colors[i][1] = c[1];
		tess.svars.colors[i][2] = c[2];
		tess.svars.colors[i][3] = c[3];
		tess.svars.texcoords[0][i][0] = 0.f;
		tess.svars.texcoords[0][i][1] = 0.f;
	}
	tess.svars.texcoordPtr[0] = tess.svars.texcoords[0];

	Com_Memset(&def, 0, sizeof(def));
	def.shader_type  = TYPE_SIGNLE_TEXTURE;
	def.state_bits   = glState.glStateBits;
	def.face_culling = CT_TWO_SIDED;
	def.mirror       = (!backEnd.projection2D && backEnd.viewParms.isMirror) ? qtrue : qfalse;
	def.primitives   = primitives;

	GL_SelectTexture(0);
	GL_Bind(tr.whiteImage);

	vk_bind_pipeline(RB_FindPipeline(&def));
	vk_bind_geometry(TESS_XYZ | TESS_RGBA0 | TESS_ST0);
	vk_draw_geometry(depthRange, qfalse);
}

#define MAX_DEBUG_VERTEXES 1024 // must be a multiple of 2 and 3

static struct
{
	Vk_Primitive_Topology primitives;
	Vk_Depth_Range depthRange;
	qboolean blend;
	byte color[4];
	int numVertexes;
	vec3_t xyz[MAX_DEBUG_VERTEXES];
	color4ub_t colors[MAX_DEBUG_VERTEXES];
} debugDraw = { LINE_LIST, DEPTH_RANGE_NORMAL, qfalse, { 255, 255, 255, 255 }, 0 };

/**
 * @brief Draw the accumulated debug vertexes, tess content is preserved
 */
static void RB_DebugFlush(void)
{
	static vec4_t     savedXyz[MAX_DEBUG_VERTEXES];
	static color4ub_t savedColors[MAX_DEBUG_VERTEXES];
	static vec2_t     savedTexcoords[MAX_DEBUG_VERTEXES];
	vec2_t            *savedTexcoordPtr = tess.svars.texcoordPtr[0];
	const int         savedNumVertexes  = tess.numVertexes;
	const int         n                 = debugDraw.numVertexes;
	Vk_Pipeline_Def   def;
	int               i;

	if (!n)
	{
		return;
	}

	// the debug drawing happens in the middle of the surface tesselation
	Com_Memcpy(savedXyz, tess.xyz, n * sizeof(tess.xyz[0]));
	Com_Memcpy(savedColors, tess.svars.colors, n * sizeof(tess.svars.colors[0]));
	Com_Memcpy(savedTexcoords, tess.svars.texcoords[0], n * sizeof(tess.svars.texcoords[0][0]));

	for (i = 0; i < n; i++)
	{
		VectorCopy(debugDraw.xyz[i], tess.xyz[i]);
		Com_Memcpy(tess.svars.colors[i], debugDraw.colors[i], sizeof(color4ub_t));
		tess.svars.texcoords[0][i][0] = 0.f;
		tess.svars.texcoords[0][i][1] = 0.f;
	}
	tess.svars.texcoordPtr[0] = tess.svars.texcoords[0];
	tess.numVertexes          = n;

	Com_Memset(&def, 0, sizeof(def));
	def.shader_type  = TYPE_SIGNLE_TEXTURE;
	def.state_bits   = GLS_DEPTHMASK_TRUE | (debugDraw.blend ? (GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE) : 0);
	def.face_culling = CT_TWO_SIDED;
	def.mirror       = (!backEnd.projection2D && backEnd.viewParms.isMirror) ? qtrue : qfalse;
	def.primitives   = debugDraw.primitives;

	GL_SelectTexture(0);
	GL_Bind(tr.whiteImage);

	vk_bind_pipeline(RB_FindPipeline(&def));
	vk_bind_geometry(TESS_XYZ | TESS_RGBA0 | TESS_ST0);
	vk_draw_geometry(debugDraw.depthRange, qfalse);

	Com_Memcpy(tess.xyz, savedXyz, n * sizeof(tess.xyz[0]));
	Com_Memcpy(tess.svars.colors, savedColors, n * sizeof(tess.svars.colors[0]));
	Com_Memcpy(tess.svars.texcoords[0], savedTexcoords, n * sizeof(tess.svars.texcoords[0][0]));
	tess.svars.texcoordPtr[0] = savedTexcoordPtr;
	tess.numVertexes          = savedNumVertexes;

	debugDraw.numVertexes = 0;
}

/**
 * @brief Start immediate debug drawing, replaces glBegin()
 * @param[in] primitives LINE_LIST or POINT_LIST
 */
void RB_DebugBegin(Vk_Primitive_Topology primitives)
{
	debugDraw.primitives  = primitives;
	debugDraw.numVertexes = 0;
}

/**
 * @brief Set the color of the next debug vertexes, replaces glColor4f()
 * @param[in] r
 * @param[in] g
 * @param[in] b
 * @param[in] a
 */
void RB_DebugColor(float r, float g, float b, float a)
{
	debugDraw.color[0] = (byte)(Com_Clamp(0.f, 1.f, r) * 255.f);
	debugDraw.color[1] = (byte)(Com_Clamp(0.f, 1.f, g) * 255.f);
	debugDraw.color[2] = (byte)(Com_Clamp(0.f, 1.f, b) * 255.f);
	debugDraw.color[3] = (byte)(Com_Clamp(0.f, 1.f, a) * 255.f);
}

/**
 * @brief Add a debug vertex, replaces glVertex3fv()
 * @param[in] v
 */
void RB_DebugVertex(const vec3_t v)
{
	if (debugDraw.numVertexes == MAX_DEBUG_VERTEXES)
	{
		RB_DebugFlush();
	}

	VectorCopy(v, debugDraw.xyz[debugDraw.numVertexes]);
	Com_Memcpy(debugDraw.colors[debugDraw.numVertexes], debugDraw.color, sizeof(debugDraw.color));
	debugDraw.numVertexes++;
}

/**
 * @brief Draw the debug vertexes, replaces glEnd()
 */
void RB_DebugEnd(void)
{
	RB_DebugFlush();
}

/**
 * @brief Set the depth range of the next debug draws, replaces glDepthRange()
 * @param[in] depthRange
 */
void RB_DebugDepthRange(Vk_Depth_Range depthRange)
{
	debugDraw.depthRange = depthRange;
}

/**
 * @brief Enable additive alpha blending for the next debug draws
 * @param[in] enable
 */
void RB_DebugBlend(qboolean enable)
{
	debugDraw.blend = enable;
}

/**
 * @brief Load a model matrix and update the MVP push constants
 * @param[in] modelMatrix
 */
void RB_LoadModelMatrix(const float *modelMatrix)
{
	Com_Memcpy(vk_world.modelview_transform, modelMatrix, sizeof(vk_world.modelview_transform));
	vk_update_mvp(NULL);
}

/**
 * @brief A player has predicted a teleport, but hasn't arrived yet
 */
static void RB_Hyperspace(void)
{
	float  c = (backEnd.refdef.time & 255) / 255.0f;
	vec4_t color;

	Vector4Set(color, c, c, c, 1.f);
	vk_clear_color(color);

	backEnd.isHyperspace = qtrue;
}

/**
 * @brief SetViewportAndScissor
 */
static void SetViewportAndScissor(void)
{
	// the projection matrix is part of the MVP push constants,
	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}

#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400

/**
 * @brief Any mirrored or portaled views have already been drawn, so prepare
 * to actually render the visible surfaces for this view
 */
void RB_BeginDrawingView(void)
{
	int    clearBits = 0;
	vec4_t clearColor;

	Vector4Set(clearColor, 0.f, 0.f, 0.f, 1.f);

	// sync with gl if needed
	if (r_finish->integer == 1 && !glState.finishCalled)
	{
		vk_queue_wait_idle();
		glState.finishCalled = qtrue;
	}
	if (r_finish->integer == 0)
	{
		glState.finishCalled = qtrue;
	}

	// we will need to change the projection matrix before drawing
	// 2D images again
	backEnd.projection2D = qfalse;

	// set the modelview matrix for the viewer
	SetViewportAndScissor();

	// ensures that depth writes are enabled for the depth clear
	GL_State(GLS_DEFAULT);


	////////// modified to ensure one glclear() per frame at most

	// clear relevant buffers
	clearBits = 0;

	if (r_measureOverdraw->integer || r_shadows->integer == 2)
	{
		clearBits |= GL_STENCIL_BUFFER_BIT;
	}
	// global q3 fog volume
	else if (tr.world && tr.world->globalFog >= 0)
	{
		clearBits |= GL_DEPTH_BUFFER_BIT;
		clearBits |= GL_COLOR_BUFFER_BIT;
		//
		Vector4Set(clearColor, tr.world->fogs[tr.world->globalFog].shader->fogParms.color[0] * tr.identityLight,
		             tr.world->fogs[tr.world->globalFog].shader->fogParms.color[1] * tr.identityLight,
		             tr.world->fogs[tr.world->globalFog].shader->fogParms.color[2] * tr.identityLight, 1.0);
	}
	else if (skyboxportal)
	{
		if (backEnd.refdef.rdflags & RDF_SKYBOXPORTAL)     // portal scene, clear whatever is necessary
		{
			clearBits |= GL_DEPTH_BUFFER_BIT;

			if (r_fastSky->integer || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL))      // fastsky: clear color
			{   // try clearing first with the portal sky fog color, then the world fog color, then finally a default
				clearBits |= GL_COLOR_BUFFER_BIT;
				if (glfogsettings[FOG_PORTALVIEW].registered)
				{
					Vector4Set(clearColor, glfogsettings[FOG_PORTALVIEW].color[0], glfogsettings[FOG_PORTALVIEW].color[1], glfogsettings[FOG_PORTALVIEW].color[2], glfogsettings[FOG_PORTALVIEW].color[3]);
				}
				else if (glfogNum > FOG_NONE && glfogsettings[FOG_CURRENT].registered)
				{
					Vector4Set(clearColor, glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3]);
				}
				else
				{
					Vector4Set(clearColor, 0.5, 0.5, 0.5, 1.0);
				}
			}
			else                                                        // rendered sky (either clear color or draw quake sky)
			{
				if (glfogsettings[FOG_PORTALVIEW].registered)
				{
					Vector4Set(clearColor, glfogsettings[FOG_PORTALVIEW].color[0], glfogsettings[FOG_PORTALVIEW].color[1], glfogsettings[FOG_PORTALVIEW].color[2], glfogsettings[FOG_PORTALVIEW].color[3]);

					if (glfogsettings[FOG_PORTALVIEW].clearscreen)        // portal fog requests a screen clear (distance fog rather than quake sky)
					{
						clearBits |= GL_COLOR_BUFFER_BIT;
					}
				}

			}
		}
		else  // world scene with portal sky, don't clear any buffers, just set the fog color if there is one
		{
			clearBits |= GL_DEPTH_BUFFER_BIT;   // this will go when I get the portal sky rendering way out in the zbuffer (or not writing to zbuffer at all)

			if (glfogNum > FOG_NONE && glfogsettings[FOG_CURRENT].registered)
			{
				if (backEnd.refdef.rdflags & RDF_UNDERWATER)
				{
					if (glfogsettings[FOG_CURRENT].mode == GL_LINEAR)
					{
						clearBits |= GL_COLOR_BUFFER_BIT;
					}

				}
				else if (!(r_portalSky->integer)) // portal skies have been manually turned off, clear bg color
				{
					clearBits |= GL_COLOR_BUFFER_BIT;
				}

				Vector4Set(clearColor, glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3]);
			}
			else if (!(r_portalSky->integer)) // portal skies have been manually turned off, clear bg color
			{
				clearBits |= GL_COLOR_BUFFER_BIT;
				Vector4Set(clearColor, 0.5, 0.5, 0.5, 1.0);
			}
		}
	}
	else // world scene with no portal sky
	{
		clearBits |= GL_DEPTH_BUFFER_BIT;

		// we don't want to clear the buffer when no world model is specified
		if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)
		{
			clearBits &= ~GL_COLOR_BUFFER_BIT;
		}
		else if (r_fastSky->integer || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL))
		{

			clearBits |= GL_COLOR_BUFFER_BIT;

			if (glfogsettings[FOG_CURRENT].registered)     // try to clear fastsky with current fog color
			{
				Vector4Set(clearColor, glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3]);
			}
			else
			{
				Vector4Set(clearColor, 0.05f, 0.05f, 0.05f, 1.0f);    // JPW NERVE changed per id req was 0.5s
			}
		}
		else  // world scene, no portal sky, not fastsky, clear color if fog says to, otherwise, just set the clearcolor
		{
			if (glfogsettings[FOG_CURRENT].registered)     // try to clear fastsky with current fog color
			{
				Vector4Set(clearColor, glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3]);

				if (glfogsettings[FOG_CURRENT].clearscreen)       // world fog requests a screen clear (distance fog rather than quake sky)
				{
					clearBits |= GL_COLOR_BUFFER_BIT;
				}
			}
		}
	}

	// don't clear the color buffer when no world model is specified
	if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)
	{
		clearBits &= ~GL_COLOR_BUFFER_BIT;
	}

	if (clearBits & GL_COLOR_BUFFER_BIT)
	{
		vk_clear_color(clearColor);
	}
	vk_clear_depth((clearBits & GL_STENCIL_BUFFER_BIT) ? qtrue : qfalse);

	if ((backEnd.refdef.rdflags & RDF_HYPERSPACE))
	{
		RB_Hyperspace();
		return;
	}
	else
	{
		backEnd.isHyperspace = qfalse;
	}

	// we will only draw a sun if there was sky rendered in this view
	backEnd.skyRenderedThisView = qfalse;

	// portal views are clipped with an oblique projection matrix, see R_SetupProjection()
}

/**
 * @brief RB_RenderDrawSurfList
 * @param[in] drawSurfs
 * @param[in] numDrawSurfs
 */
void RB_RenderDrawSurfList(drawSurf_t *drawSurfs, int numDrawSurfs)
{
	shader_t   *shader, *oldShader;
	int        fogNum, oldFogNum;
	int        entityNum, oldEntityNum;
	int        frontFace;
	int        dlighted, oldDlighted;
	qboolean   depthRange;
	int        i;
	drawSurf_t *drawSurf;
	int        oldSort;
	double     originalTime = backEnd.refdef.floatTime; // save original time for entity shader offsets

	// clear the z buffer, set the modelview, etc
	RB_BeginDrawingView();

	// draw everything
	oldEntityNum          = -1;
	backEnd.currentEntity = &tr.worldEntity;
	oldShader             = NULL;
	oldFogNum             = -1;
	oldDlighted           = qfalse;
	oldSort               = -1;
	depthRange            = qfalse;

	backEnd.pc.c_surfaces += numDrawSurfs;

	for (i = 0, drawSurf = drawSurfs ; i < numDrawSurfs ; i++, drawSurf++)
	{
		if (drawSurf->sort == oldSort)
		{
			// fast path, same as previous sort
			rb_surfaceTable[*drawSurf->surface] (drawSurf->surface);
			continue;
		}
		oldSort = drawSurf->sort;
		R_DecomposeSort(drawSurf->sort, &entityNum, &shader, &fogNum, &frontFace, &dlighted);

		// change the tess parameters if needed
		// a "entityMergable" shader is a shader that can have surfaces from seperate
		// entities merged into a single batch, like smoke and blood puff sprites
		if (shader && (shader != oldShader || fogNum != oldFogNum || dlighted != oldDlighted
		               || (entityNum != oldEntityNum && !shader->entityMergable)))
		{
			if (oldShader != NULL)
			{
				RB_EndSurface();
			}
			RB_BeginSurface(shader, fogNum);
			oldShader   = shader;
			oldFogNum   = fogNum;
			oldDlighted = dlighted;
		}

		// change the modelview matrix if needed
		if (entityNum != oldEntityNum)
		{
			depthRange = qfalse;

			if (entityNum != ENTITYNUM_WORLD)
			{
				backEnd.currentEntity = &backEnd.refdef.entities[entityNum];

				// FIXME: e.shaderTime must be passed as int to avoid fp-precision loss issues
				backEnd.refdef.floatTime = originalTime; // - backEnd.currentEntity->e.shaderTime; // JPW NERVE pulled this to match q3ta

				// we have to reset the shaderTime as well otherwise image animations start
				// from the wrong frame
				// tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;

				// set up the transformation matrix
				R_RotateForEntity(backEnd.currentEntity, &backEnd.viewParms, &backEnd.orientation);

				// set up the dynamic lighting if needed
				if (backEnd.currentEntity->needDlights)
				{
					R_TransformDlights(backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.orientation);
				}

				if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK)
				{
					// hack the depth range to prevent view model from poking into walls
					depthRange = qtrue;
				}
			}
			else
			{
				backEnd.currentEntity    = &tr.worldEntity;
				backEnd.refdef.floatTime = originalTime;
				backEnd.orientation      = backEnd.viewParms.world;

				// we have to reset the shaderTime as well otherwise image animations on
				// the world (like water) continue with the wrong frame
				// tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;

				R_TransformDlights(backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.orientation);
			}

			tess.depthRange = depthRange ? DEPTH_RANGE_WEAPON : DEPTH_RANGE_NORMAL;
			RB_LoadModelMatrix(backEnd.orientation.modelMatrix);

			oldEntityNum = entityNum;
		}

		// add the triangles for this surface
		rb_surfaceTable[*drawSurf->surface] (drawSurf->surface);
	}

	// draw the contents of the last shader batch
	if (oldShader != NULL)
	{
		RB_EndSurface();
	}

	// go back to the world modelview matrix
	backEnd.currentEntity    = &tr.worldEntity;
	backEnd.refdef.floatTime = originalTime;
	backEnd.orientation      = backEnd.viewParms.world;
	R_TransformDlights(backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.orientation);

	tess.depthRange = DEPTH_RANGE_NORMAL;
	RB_LoadModelMatrix(backEnd.viewParms.world.modelMatrix);

	// draw sun
	RB_DrawSun();

	// darken down any stencil shadows
	RB_ShadowFinish();

	// add light flares on lights that aren't obscured
	RB_RenderFlares();
}

/*
============================================================================
RENDER BACK END FUNCTIONS
============================================================================
*/

/**
 * @brief RB_SetGL2D
 */
void RB_SetGL2D(void)
{
	backEnd.projection2D = qtrue;

	// set 2D virtual screen size
	vk_update_mvp(NULL);

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	tess.depthRange     = DEPTH_RANGE_NORMAL;

	GL_State(GLS_DEPTHTEST_DISABLE |
	         GLS_SRCBLEND_SRC_ALPHA |
	         GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);

	GL_Cull(CT_TWO_SIDED);

	// set time for 2D shaders
	backEnd.refdef.time      = ri.Milliseconds();
	backEnd.refdef.floatTime = backEnd.refdef.time * 0.001;

	// apply bloom to the 3D scene before any 2D drawing
	if (r_bloom->integer)
	{
		vk_bloom();
	}
}

/**
 * @brief Stretches a raw 32 bit power of 2 bitmap image over the given screen rectangle.
 * Used for cinematics.
 *
 * @param[in] x
 * @param[in] y
 * @param[in] w
 * @param[in] h
 * @param[in] cols
 * @param[in] rows
 * @param[in] data
 * @param[in] client
 * @param[in] dirty
 *
 * @todo FIXME: not exactly backend
 */
void RE_StretchRaw(int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty)
{
	if (!tr.registered)
	{
		return;
	}
	R_IssuePendingRenderCommands();

	if (tess.numIndexes)
	{
		RB_EndSurface();
	}

	RE_UploadCinematic(0, 0, cols, rows, data, client, dirty);

	// nothing to draw into outside of a frame
	if (!vk.frame_count)
	{
		return;
	}

	if (!backEnd.projection2D)
	{
		RB_SetGL2D();
	}

	tess.numVertexes = 4;

	tess.xyz[0][0] = x;
	tess.xyz[0][1] = y;
	tess.xyz[0][2] = 0;
	tess.xyz[1][0] = x + w;
	tess.xyz[1][1] = y;
	tess.xyz[1][2] = 0;
	tess.xyz[2][0] = x;
	tess.xyz[2][1] = y + h;
	tess.xyz[2][2] = 0;
	tess.xyz[3][0] = x + w;
	tess.xyz[3][1] = y + h;
	tess.xyz[3][2] = 0;

	tess.svars.texcoords[0][0][0] = 0.5f / cols;
	tess.svars.texcoords[0][0][1] = 0.5f / rows;
	tess.svars.texcoords[0][1][0] = (cols - 0.5f) / cols;
	tess.svars.texcoords[0][1][1] = 0.5f / rows;
	tess.svars.texcoords[0][2][0] = 0.5f / cols;
	tess.svars.texcoords[0][2][1] = (rows - 0.5f) / rows;
	tess.svars.texcoords[0][3][0] = (cols - 0.5f) / cols;
	tess.svars.texcoords[0][3][1] = (rows - 0.5f) / rows;
	tess.svars.texcoordPtr[0]     = tess.svars.texcoords[0];

	Com_Memset(tess.svars.colors, tr.identityLightByte, 4 * sizeof(color4ub_t));
	tess.svars.colors[0][3] = tess.svars.colors[1][3] = tess.svars.colors[2][3] = tess.svars.colors[3][3] = 255;

	GL_SelectTexture(0);
	GL_Bind(tr.scratchImage[client]);
	GL_State(GLS_DEPTHTEST_DISABLE);

	vk_bind_pipeline(RB_StatePipeline(1, TRIANGLE_STRIP));
	vk_bind_geometry(TESS_XYZ | TESS_RGBA0 | TESS_ST0);
	vk_draw_geometry(DEPTH_RANGE_NORMAL, qfalse);

	tess.numVertexes = 0;
}

/**
 * @brief RE_UploadCinematic
 *
 * @param w - unused
 * @param h - unused
 * @param[in] cols
 * @param[in] rows
 * @param[in] data
 * @param[in] client
 * @param[in] dirty
 */
void RE_UploadCinematic(int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty)
{
	int     start;
	image_t *image;

	if (client < 0 || client >= (sizeof(tr.scratchImage) / sizeof(tr.scratchImage[0])))
	{
		Ren_Drop("RE_UploadCinematic: image offset out of range");
	}

	if (!tr.scratchImage[client])
	{
		tr.scratchImage[client] = R_CreateImage(va("*scratch%i", client), data, cols, rows, qfalse, qtrue, GL_CLAMP_TO_EDGE);
		dirty                   = qfalse;
	}

	start = 0;
	if (r_speeds->integer)
	{
		start = ri.Milliseconds();
	}

	image = tr.scratchImage[client];

	// if the scratchImage isn't in the format we want, specify it as a new texture
	if (cols != image->width || rows != image->height)
	{
		image->width  = image->uploadWidth = cols;
		image->height = image->uploadHeight = rows;
		vk_create_image(image, cols, rows, 1);
		vk_upload_image_data(image, 0, 0, cols, rows, 1, (byte *)data, cols * rows * 4, qfalse);
	}
	else if (dirty)
	{
		// otherwise, just subimage upload it so that drivers can tell we are going to be changing
		// it and don't try and do a texture compression
		vk_upload_image_data(image, 0, 0, cols, rows, 1, (byte *)data, cols * rows * 4, qtrue);
	}

	if (r_speeds->integer)
	{
		int end = ri.Milliseconds();

		Ren_Print("RE_UploadCinematic %i, %i: %i msec\n", cols, rows, end - start);
	}
}

/**
 * @brief RB_SetColor
 * @param[in] data
 * @return
 */
const void *RB_SetColor(const void *data)
{
	const setColorCommand_t *cmd = ( const setColorCommand_t * ) data;

	backEnd.color2D[0] = (byte)(cmd->color[0] * 255);
	backEnd.color2D[1] = (byte)(cmd->color[1] * 255);
	backEnd.color2D[2] = (byte)(cmd->color[2] * 255);
	backEnd.color2D[3] = (byte)(cmd->color[3] * 255);

	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_StretchPic
 * @param[in] data
 * @return
 */
const void *RB_StretchPic(const void *data)
{
	const stretchPicCommand_t *cmd = ( const stretchPicCommand_t * ) data;
	shader_t                  *shader;
	int                       numVerts, numIndexes;

	if (!backEnd.projection2D)
	{
		RB_SetGL2D();
	}

	shader = cmd->shader;
	if (shader != tess.shader)
	{
		if (tess.numIndexes)
		{
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface(shader, 0);
	}

	RB_CHECKOVERFLOW(4, 6);
	numVerts   = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes  += 6;

	tess.indexes[numIndexes]     = numVerts + 3;
	tess.indexes[numIndexes + 1] = numVerts + 0;
	tess.indexes[numIndexes + 2] = numVerts + 2;
	tess.indexes[numIndexes + 3] = numVerts + 2;
	tess.indexes[numIndexes + 4] = numVerts + 0;
	tess.indexes[numIndexes + 5] = numVerts + 1;

	*( int * ) tess.vertexColors[numVerts]                 =
		*( int * ) tess.vertexColors[numVerts + 1]         =
			*( int * ) tess.vertexColors[numVerts + 2]     =
				*( int * ) tess.vertexColors[numVerts + 3] = *( int * ) backEnd.color2D;

	tess.xyz[numVerts][0] = cmd->x;
	tess.xyz[numVerts][1] = cmd->y;
	tess.xyz[numVerts][2] = 0;

	tess.texCoords[numVerts][0][0] = cmd->s1;
	tess.texCoords[numVerts][0][1] = cmd->t1;

	tess.xyz[numVerts + 1][0] = cmd->x + cmd->w;
	tess.xyz[numVerts + 1][1] = cmd->y;
	tess.xyz[numVerts + 1][2] = 0;

	tess.texCoords[numVerts + 1][0][0] = cmd->s2;
	tess.texCoords[numVerts + 1][0][1] = cmd->t1;

	tess.xyz[numVerts + 2][0] = cmd->x + cmd->w;
	tess.xyz[numVerts + 2][1] = cmd->y + cmd->h;
	tess.xyz[numVerts + 2][2] = 0;

	tess.texCoords[numVerts + 2][0][0] = cmd->s2;
	tess.texCoords[numVerts + 2][0][1] = cmd->t2;

	tess.xyz[numVerts + 3][0] = cmd->x;
	tess.xyz[numVerts + 3][1] = cmd->y + cmd->h;
	tess.xyz[numVerts + 3][2] = 0;

	tess.texCoords[numVerts + 3][0][0] = cmd->s1;
	tess.texCoords[numVerts + 3][0][1] = cmd->t2;

	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_Draw2dPolys
 * @param[in] data
 * @return
 */
const void *RB_Draw2dPolys(const void *data)
{
	const poly2dCommand_t *cmd = ( const poly2dCommand_t * ) data;
	shader_t              *shader;
	int                   i;

	if (!backEnd.projection2D)
	{
		RB_SetGL2D();
	}

	shader = cmd->shader;
	if (shader != tess.shader)
	{
		if (tess.numIndexes)
		{
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface(shader, 0);
	}

	RB_CHECKOVERFLOW(cmd->numverts, (cmd->numverts - 2) * 3);

	for (i = 0; i < cmd->numverts - 2; i++)
	{
		tess.indexes[tess.numIndexes + 0] = tess.numVertexes;
		tess.indexes[tess.numIndexes + 1] = tess.numVertexes + i + 1;
		tess.indexes[tess.numIndexes + 2] = tess.numVertexes + i + 2;
		tess.numIndexes                  += 3;
	}

	for (i = 0; i < cmd->numverts; i++)
	{
		tess.xyz[tess.numVertexes][0] = cmd->verts[i].xyz[0];
		tess.xyz[tess.numVertexes][1] = cmd->verts[i].xyz[1];
		tess.xyz[tess.numVertexes][2] = 0;

		tess.texCoords[tess.numVertexes][0][0] = cmd->verts[i].st[0];
		tess.texCoords[tess.numVertexes][0][1] = cmd->verts[i].st[1];

		tess.vertexColors[tess.numVertexes][0] = cmd->verts[i].modulate[0];
		tess.vertexColors[tess.numVertexes][1] = cmd->verts[i].modulate[1];
		tess.vertexColors[tess.numVertexes][2] = cmd->verts[i].modulate[2];
		tess.vertexColors[tess.numVertexes][3] = cmd->verts[i].modulate[3];
		tess.numVertexes++;
	}

	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_RotatedPic
 * @param[in] data
 * @return
 */
const void *RB_RotatedPic(const void *data)
{
	const stretchPicCommand_t *cmd = ( const stretchPicCommand_t * ) data;
	shader_t                  *shader;
	int                       numVerts, numIndexes;
	float                     angle;

	if (!backEnd.projection2D)
	{
		RB_SetGL2D();
	}

	shader = cmd->shader;
	if (shader != tess.shader)
	{
		if (tess.numIndexes)
		{
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface(shader, 0);
	}

	RB_CHECKOVERFLOW(4, 6);
	numVerts   = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes  += 6;

	tess.indexes[numIndexes]     = numVerts + 3;
	tess.indexes[numIndexes + 1] = numVerts + 0;
	tess.indexes[numIndexes + 2] = numVerts + 2;
	tess.indexes[numIndexes + 3] = numVerts + 2;
	tess.indexes[numIndexes + 4] = numVerts + 0;
	tess.indexes[numIndexes + 5] = numVerts + 1;

	*( int * ) tess.vertexColors[numVerts]                 =
		*( int * ) tess.vertexColors[numVerts + 1]         =
			*( int * ) tess.vertexColors[numVerts + 2]     =
				*( int * ) tess.vertexColors[numVerts + 3] = *( int * ) backEnd.color2D;

	angle                 = cmd->angle * M_TAU_F;
	tess.xyz[numVerts][0] = cmd->x + (cos(angle) * cmd->w);
	tess.xyz[numVerts][1] = cmd->y + (sin(angle) * cmd->h);
	tess.xyz[numVerts][2] = 0;

	tess.texCoords[numVerts][0][0] = cmd->s1;
	tess.texCoords[numVerts][0][1] = cmd->t1;

	angle                     = cmd->angle * M_TAU_F + 0.25 * M_TAU_F;
	tess.xyz[numVerts + 1][0] = cmd->x + (cos(angle) * cmd->w);
	tess.xyz[numVerts + 1][1] = cmd->y + (sin(angle) * cmd->h);
	tess.xyz[numVerts + 1][2] = 0;

	tess.texCoords[numVerts + 1][0][0] = cmd->s2;
	tess.texCoords[numVerts + 1][0][1] = cmd->t1;

	angle                     = cmd->angle * M_TAU_F + 0.50 * M_TAU_F;
	tess.xyz[numVerts + 2][0] = cmd->x + (cos(angle) * cmd->w);
	tess.xyz[numVerts + 2][1] = cmd->y + (sin(angle) * cmd->h);
	tess.xyz[numVerts + 2][2] = 0;

	tess.texCoords[numVerts + 2][0][0] = cmd->s2;
	tess.texCoords[numVerts + 2][0][1] = cmd->t2;

	angle                     = cmd->angle * M_TAU_F + 0.75 * M_TAU_F;
	tess.xyz[numVerts + 3][0] = cmd->x + (cos(angle) * cmd->w);
	tess.xyz[numVerts + 3][1] = cmd->y + (sin(angle) * cmd->h);
	tess.xyz[numVerts + 3][2] = 0;

	tess.texCoords[numVerts + 3][0][0] = cmd->s1;
	tess.texCoords[numVerts + 3][0][1] = cmd->t2;

	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_StretchPicGradient
 * @param[in] data
 * @return
 */
const void *RB_StretchPicGradient(const void *data)
{
	const stretchPicCommand_t *cmd = ( const stretchPicCommand_t * ) data;
	shader_t                  *shader;
	int                       numVerts, numIndexes;

	if (!backEnd.projection2D)
	{
		RB_SetGL2D();
	}

	shader = cmd->shader;
	if (shader != tess.shader)
	{
		if (tess.numIndexes)
		{
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface(shader, 0);
	}

	RB_CHECKOVERFLOW(4, 6);
	numVerts   = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes  += 6;

	tess.indexes[numIndexes]     = numVerts + 3;
	tess.indexes[numIndexes + 1] = numVerts + 0;
	tess.indexes[numIndexes + 2] = numVerts + 2;
	tess.indexes[numIndexes + 3] = numVerts + 2;
	tess.indexes[numIndexes + 4] = numVerts + 0;
	tess.indexes[numIndexes + 5] = numVerts + 1;

	*( int * ) tess.vertexColors[numVerts]         =
		*( int * ) tess.vertexColors[numVerts + 1] = *( int * ) backEnd.color2D;

	*( int * ) tess.vertexColors[numVerts + 2]     =
		*( int * ) tess.vertexColors[numVerts + 3] = *( int * ) cmd->gradientColor;

	tess.xyz[numVerts][0] = cmd->x;
	tess.xyz[numVerts][1] = cmd->y;
	tess.xyz[numVerts][2] = 0;

	tess.texCoords[numVerts][0][0] = cmd->s1;
	tess.texCoords[numVerts][0][1] = cmd->t1;

	tess.xyz[numVerts + 1][0] = cmd->x + cmd->w;
	tess.xyz[numVerts + 1][1] = cmd->y;
	tess.xyz[numVerts + 1][2] = 0;

	tess.texCoords[numVerts + 1][0][0] = cmd->s2;
	tess.texCoords[numVerts + 1][0][1] = cmd->t1;

	tess.xyz[numVerts + 2][0] = cmd->x + cmd->w;
	tess.xyz[numVerts + 2][1] = cmd->y + cmd->h;
	tess.xyz[numVerts + 2][2] = 0;

	tess.texCoords[numVerts + 2][0][0] = cmd->s2;
	tess.texCoords[numVerts + 2][0][1] = cmd->t2;

	tess.xyz[numVerts + 3][0] = cmd->x;
	tess.xyz[numVerts + 3][1] = cmd->y + cmd->h;
	tess.xyz[numVerts + 3][2] = 0;

	tess.texCoords[numVerts + 3][0][0] = cmd->s1;
	tess.texCoords[numVerts + 3][0][1] = cmd->t2;

	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_DrawSurfs
 * @param[in] data
 * @return
 */
const void *RB_DrawSurfs(const void *data)
{
	const drawSurfsCommand_t *cmd;

	// finish any 2D drawing if needed
	if (tess.numIndexes)
	{
		RB_EndSurface();
	}

	cmd = ( const drawSurfsCommand_t * ) data;

	backEnd.refdef    = cmd->refdef;
	backEnd.viewParms = cmd->viewParms;

	RB_RenderDrawSurfList(cmd->drawSurfs, cmd->numDrawSurfs);

	if (!(backEnd.refdef.rdflags & RDF_SKYBOXPORTAL))
	{
		backEnd.doneSurfaces = qtrue; // for bloom
	}

	return ( const void * ) (cmd + 1);
}

static void RB_BeginFrame(void);

/**
 * @brief RB_DrawBuffer
 * @param[in] data
 * @return
 */
const void *RB_DrawBuffer(const void *data)
{
	const drawBufferCommand_t *cmd = ( const drawBufferCommand_t * ) data;

	RB_BeginFrame();

	return ( const void * ) (cmd + 1);
}

/**
 * @brief Start recording the frame command buffer
 *
 * Also used for 2D commands queued before RE_BeginFrame(), e.g. the cgame loading
 * screen, which OpenGL draws into the back buffer before the frame's own commands.
 */
static void RB_BeginFrame(void)
{
	vk_begin_frame();

	tess.depthRange = DEPTH_RANGE_NORMAL;

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	// clear screen for debugging
	if (r_clear->integer && vk.clearAttachment)
	{
		const vec4_t color = { 1, 0, 0.5f, 1 };

		backEnd.projection2D = qtrue; // to ensure we have viewport that occupies entire window
		vk_clear_color(color);
		backEnd.projection2D = qfalse;
	}
}

/**
 * @brief Draw all the images to the screen, on top of whatever
 * was there.  This is used to test for texture thrashing.
 *
 * Also called by RE_EndRegistration
 */
void RB_ShowImages(void)
{
	int     i;
	image_t *image;
	float   x, y, w, h;
	int     start, end;

	RB_SetGL2D();

	start = ri.Milliseconds();

	// draw full-screen quad
	tess.numVertexes = 4;

	Com_Memset(tess.svars.colors, 255, 4 * sizeof(color4ub_t));

	tess.svars.texcoords[0][0][0] = 0.0f;
	tess.svars.texcoords[0][0][1] = 0.0f;
	tess.svars.texcoords[0][1][0] = 1.0f;
	tess.svars.texcoords[0][1][1] = 0.0f;
	tess.svars.texcoords[0][2][0] = 0.0f;
	tess.svars.texcoords[0][2][1] = 1.0f;
	tess.svars.texcoords[0][3][0] = 1.0f;
	tess.svars.texcoords[0][3][1] = 1.0f;
	tess.svars.texcoordPtr[0]     = tess.svars.texcoords[0];

	tess.xyz[0][0] = 0.0f;
	tess.xyz[0][1] = 0.0f;
	tess.xyz[1][0] = (float)glConfig.vidWidth;
	tess.xyz[1][1] = 0.0f;
	tess.xyz[2][0] = 0.0f;
	tess.xyz[2][1] = (float)glConfig.vidHeight;
	tess.xyz[3][0] = (float)glConfig.vidWidth;
	tess.xyz[3][1] = (float)glConfig.vidHeight;
	for (i = 0; i < 4; i++)
	{
		tess.xyz[i][2] = 0.0f;
	}

	vk_bind_pipeline(vk.images_debug_pipeline2);
	vk_bind_geometry(TESS_XYZ | TESS_RGBA0 | TESS_ST0);
	vk_draw_geometry(DEPTH_RANGE_NORMAL, qfalse);

	for (i = 0 ; i < tr.numImages ; i++)
	{
		image = tr.images[i];

		w = glConfig.vidWidth / 40;
		h = glConfig.vidHeight / 30;

		x = i % 40 * w;
		y = i / 30 * h;

		// show in proportional size in mode 2
		if (r_showImages->integer == 2)
		{
			w *= image->uploadWidth / 512.0f;
			h *= image->uploadHeight / 512.0f;
		}

		tess.xyz[0][0] = x;
		tess.xyz[0][1] = y;
		tess.xyz[1][0] = x + w;
		tess.xyz[1][1] = y;
		tess.xyz[2][0] = x;
		tess.xyz[2][1] = y + h;
		tess.xyz[3][0] = x + w;
		tess.xyz[3][1] = y + h;

		GL_Bind(image);
		vk_bind_pipeline(vk.images_debug_pipeline);
		vk_bind_geometry(TESS_XYZ);
		vk_draw_geometry(DEPTH_RANGE_NORMAL, qfalse);
	}

	tess.numIndexes  = 0;
	tess.numVertexes = 0;

	end = ri.Milliseconds();
	Ren_Print("%i msec to draw all images\n", end - start);
}

/*
 * @brief RB_DrawBounds
 * @param[in,out] mins
 * @param[in,out] maxs
 *
 * @note Unused.
void RB_DrawBounds(vec3_t mins, vec3_t maxs)
{
    vec3_t center;

    GL_Bind(tr.whiteImage);
    GL_State(GLS_POLYMODE_LINE);

    // box corners
    glBegin(GL_LINES);
    glColor3f(1, 1, 1);

    glVertex3f(mins[0], mins[1], mins[2]);
    glVertex3f(maxs[0], mins[1], mins[2]);
    glVertex3f(mins[0], mins[1], mins[2]);
    glVertex3f(mins[0], maxs[1], mins[2]);
    glVertex3f(mins[0], mins[1], mins[2]);
    glVertex3f(mins[0], mins[1], maxs[2]);

    glVertex3f(maxs[0], maxs[1], maxs[2]);
    glVertex3f(mins[0], maxs[1], maxs[2]);
    glVertex3f(maxs[0], maxs[1], maxs[2]);
    glVertex3f(maxs[0], mins[1], maxs[2]);
    glVertex3f(maxs[0], maxs[1], maxs[2]);
    glVertex3f(maxs[0], maxs[1], mins[2]);
    glEnd();

    center[0] = (mins[0] + maxs[0]) * 0.5f;
    center[1] = (mins[1] + maxs[1]) * 0.5f;
    center[2] = (mins[2] + maxs[2]) * 0.5f;

    // center axis
    glBegin(GL_LINES);
    glColor3f(1, 0.85f, 0);

    glVertex3f(mins[0], center[1], center[2]);
    glVertex3f(maxs[0], center[1], center[2]);
    glVertex3f(center[0], mins[1], center[2]);
    glVertex3f(center[0], maxs[1], center[2]);
    glVertex3f(center[0], center[1], mins[2]);
    glVertex3f(center[0], center[1], maxs[2]);
    glEnd();
}
*/

/**
 * @brief RB_SwapBuffers
 * @param[in] data
 * @return
 */
const void *RB_SwapBuffers(const void *data)
{
	const swapBuffersCommand_t *cmd;

	// finish any 2D drawing if needed
	if (tess.numIndexes)
	{
		RB_EndSurface();
	}

	// texture swapping test
	if (r_showImages->integer)
	{
		RB_ShowImages();
	}

	cmd = ( const swapBuffersCommand_t * ) data;

	vk_end_frame();

	if (backEnd.doneSurfaces && !glState.finishCalled)
	{
		vk_queue_wait_idle();
	}

	if (backEnd.screenshotMask && vk.cmd->waitForFence)
	{
		RB_TakePendingScreenshots();
	}
	backEnd.screenshotMask = 0;

	Ren_LogComment("***************** RB_SwapBuffers *****************\n\n\n");

	vk_present_frame();

	// no buffer swap there with Vulkan, but it applies r_fullscreen changes
	ri.GLimp_SwapFrame();

	backEnd.projection2D = qfalse;
	backEnd.doneSurfaces = qfalse;
	backEnd.doneBloom    = qfalse;

	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_RenderToTexture
 * @param[in] data
 * @return
 */
const void *RB_RenderToTexture(const void *data)
{
	const renderToTextureCommand_t *cmd = ( const renderToTextureCommand_t * ) data;
	static qboolean                warned = qfalse;

	// FIXME: copying the framebuffer into a texture is not implemented yet
	if (!warned)
	{
		Ren_Warning("WARNING: RB_RenderToTexture is not supported by the Vulkan renderer\n");
		warned = qtrue;
	}

	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_Finish
 * @param[in] data
 * @return
 */
const void *RB_Finish(const void *data)
{
	const renderFinishCommand_t *cmd = ( const renderFinishCommand_t * ) data;

	// the frame is submitted and waited for in RB_SwapBuffers
	return ( const void * ) (cmd + 1);
}

/**
 * @brief RB_ExecuteRenderCommands
 * @param[in] data
 */
void RB_ExecuteRenderCommands(const void *data)
{
	int t1, t2;

	t1 = ri.Milliseconds();

	while (1)
	{
		data = PADP(data, sizeof(intptr_t));

		// drawing needs a command buffer in recording state
		if (!vk.frame_count)
		{
			switch (*( const int * ) data)
			{
			case RC_STRETCH_PIC:
			case RC_2DPOLYS:
			case RC_ROTATED_PIC:
			case RC_STRETCH_PIC_GRADIENT:
			case RC_DRAW_SURFS:
				RB_BeginFrame();
				break;
			default:
				break;
			}
		}

		switch (*( const int * ) data)
		{
		case RC_SET_COLOR:
			data = RB_SetColor(data);
			break;
		case RC_STRETCH_PIC:
			data = RB_StretchPic(data);
			break;
		case RC_2DPOLYS:
			data = RB_Draw2dPolys(data);
			break;
		case RC_ROTATED_PIC:
			data = RB_RotatedPic(data);
			break;
		case RC_STRETCH_PIC_GRADIENT:
			data = RB_StretchPicGradient(data);
			break;
		case RC_DRAW_SURFS:
			data = RB_DrawSurfs(data);
			break;
		case RC_DRAW_BUFFER:
			data = RB_DrawBuffer(data);
			break;
		case RC_SWAP_BUFFERS:
			data = RB_SwapBuffers(data);
			break;
		case RC_SCREENSHOT:
			data = RB_TakeScreenshotCmd(data);
			break;
		case RC_VIDEOFRAME:
			data = RB_TakeVideoFrameCmd(data);
			break;
		case RC_RENDERTOTEXTURE:
			data = RB_RenderToTexture(data);
			break;
		case RC_FINISH:
			data = RB_Finish(data);
			break;
		case RC_END_OF_LIST:
		default:
			// stop rendering on this thread
			t2              = ri.Milliseconds();
			backEnd.pc.msec = t2 - t1;
			return;
		}
	}
}
