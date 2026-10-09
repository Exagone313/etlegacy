/*
 * Wolfenstein: Enemy Territory GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
 *
 * Quake3e GPL Source Code
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
 * @file renderer_vk/tr_vbo.c
 * @brief Static world surfaces in a vertex buffer, ported from Quake3e (code/renderervk/vk_vbo.c)
 *
 * All the data of the world surfaces whose shader can be evaluated at map load time
 * (vertexes, and the colors and texture coordinates of every stage) is stored in
 * device-local memory and accessed with indexes only.
 *
 * Every static surface gets an item index which is queued instead of tesselating
 * the surface. When the batch is drawn, the queued items are sorted to find runs
 * of consecutive indexes: long runs are drawn from the device-local index buffer,
 * the short ones are copied to the host-visible index buffer and drawn at once.
 *
 * Surfaces with dynamic lights, inside fog volumes or of other entities than
 * the world are tesselated like before.
 */

#include "tr_local.h"

#define MIN_IBO_RUN 320

/**
 * @struct vbo_item_s
 * @brief A static surface
 */
typedef struct vbo_item_s
{
	int index_offset;           ///< device-local, relative to current shader
	int soft_offset;            ///< host-visible, absolute
	int num_indexes;
	int num_vertexes;
} vbo_item_t;

/**
 * @struct ibo_item_s
 * @brief A run of device-local indexes
 */
typedef struct ibo_item_s
{
	int offset;
	int length;
} ibo_item_t;

/**
 * @struct vbo_s
 * @brief
 */
typedef struct vbo_s
{
	byte *vbo_buffer;
	int vbo_offset;
	int vbo_size;

	byte *ibo_buffer;
	int ibo_offset;
	int ibo_size;

	uint32_t soft_buffer_indexes;
	uint32_t soft_buffer_offset;

	ibo_item_t *ibo_items;
	int ibo_items_count;

	vbo_item_t *items;
	int items_count;

	int *items_queue;
	int items_queue_count;
} vbo_t;

static vbo_t world_vbo;

/**
 * @brief isStaticRGBgen
 * @param[in] cgen
 * @return qtrue if the stage colors don't change over time
 */
static qboolean isStaticRGBgen(colorGen_t cgen)
{
	switch (cgen)
	{
	case CGEN_BAD:
	case CGEN_IDENTITY_LIGHTING:
	case CGEN_IDENTITY:
	case CGEN_EXACT_VERTEX:
	case CGEN_VERTEX:
	case CGEN_ONE_MINUS_VERTEX:
	case CGEN_CONST:
		return qtrue;
	default:
		return qfalse;
	}
}

/**
 * @brief isStaticAgen
 * @param[in] agen
 * @return qtrue if the stage alpha doesn't change over time
 */
static qboolean isStaticAgen(alphaGen_t agen)
{
	switch (agen)
	{
	case AGEN_IDENTITY:
	case AGEN_SKIP:
	case AGEN_VERTEX:
	case AGEN_ONE_MINUS_VERTEX:
	case AGEN_CONST:
		return qtrue;
	default:
		return qfalse;
	}
}

/**
 * @brief isStaticTCgen
 * @param[in] bundle
 * @return qtrue if the texture coordinates don't change over time
 */
static qboolean isStaticTCgen(const textureBundle_t *bundle)
{
	switch (bundle->tcGen)
	{
	case TCGEN_BAD:
	case TCGEN_IDENTITY:
	case TCGEN_LIGHTMAP:
	case TCGEN_TEXTURE:
	case TCGEN_VECTOR:
		return qtrue;
	default:
		return qfalse;
	}
}

/**
 * @brief isStaticTCmod
 * @param[in] bundle
 * @return qtrue if the texture coordinate modifiers don't change over time
 */
static qboolean isStaticTCmod(const textureBundle_t *bundle)
{
	int i;

	for (i = 0; i < bundle->numTexMods; i++)
	{
		switch (bundle->texMods[i].type)
		{
		case TMOD_NONE:
		case TMOD_TRANSFORM:
		case TMOD_SCALE:
		case TMOD_SWAP:
			break;
		default:
			return qfalse;
		}
	}

	return qtrue;
}

/**
 * @brief Decide if the surfaces of a shader can be put in the static VBO
 * @param[in,out] shader
 * @return
 */
static qboolean isStaticShader(shader_t *shader)
{
	const shaderStage_t *stage;
	int                 i, b, svarsSize;

	if (shader->isStaticShader)
	{
		return qtrue;
	}

	if (shader->isSky || shader->remappedShader || shader->numDeforms)
	{
		return qfalse;
	}

	svarsSize = 0;

	for (i = 0; i < MAX_SHADER_STAGES; i++)
	{
		stage = shader->stages[i];
		if (!stage || !stage->active)
		{
			break;
		}

		if (!isStaticRGBgen(stage->rgbGen) || !isStaticAgen(stage->alphaGen))
		{
			return qfalse;
		}

		if (stage->adjustColorsForFog != ACFF_NONE)
		{
			return qfalse;
		}

		for (b = 0; b < NUM_TEXTURE_BUNDLES; b++)
		{
			if (!isStaticTCgen(&stage->bundle[b]) || !isStaticTCmod(&stage->bundle[b]))
			{
				return qfalse;
			}
		}

		// colors and texture coordinates, a second set for multitexture
		svarsSize += sizeof(color4ub_t) + sizeof(vec2_t);
		if (stage->bundle[1].image[0])
		{
			svarsSize += sizeof(vec2_t);
		}
	}

	if (i == 0)
	{
		return qfalse;
	}

	shader->isStaticShader = qtrue;
	shader->svarsSize      = svarsSize;
	shader->iboOffset      = -1;
	shader->vboOffset      = -1;
	shader->normalOffset   = -1;
	shader->curIndexes     = 0;
	shader->curVertexes    = 0;
	shader->numIndexes     = 0;
	shader->numVertexes    = 0;

	return qtrue;
}

/**
 * @brief Copy the indexes and vertexes of the tesselated surface
 * @param[in,out] vbo
 * @param[in,out] vi
 * @param[in,out] input
 */
static void VBO_AddGeometry(vbo_t *vbo, vbo_item_t *vi, shaderCommands_t *input)
{
	shader_t *shader = input->shader;
	uint32_t size, offs;
	int      i;

	if (shader->iboOffset == -1 || shader->vboOffset == -1)
	{
		// allocate indexes
		shader->iboOffset = vbo->vbo_offset;
		vbo->vbo_offset  += shader->numIndexes * sizeof(input->indexes[0]);

		// allocate xyz + svars
		shader->vboOffset    = vbo->vbo_offset;
		shader->normalOffset = shader->vboOffset; // normals are not stored, no pipeline uses them
		vbo->vbo_offset     += shader->numVertexes * (sizeof(input->xyz[0]) + shader->svarsSize);

		// go to first color offset
		offs = shader->vboOffset + shader->numVertexes * sizeof(input->xyz[0]);

		for (i = 0; i < MAX_SHADER_STAGES; i++)
		{
			shaderStage_t *pStage = input->xstages[i];

			if (!pStage || !pStage->active)
			{
				break;
			}

			pStage->rgb_offset[0] = pStage->rgb_offset[1] = pStage->rgb_offset[2] = offs;
			offs                 += shader->numVertexes * sizeof(color4ub_t);

			pStage->tex_offset[0] = pStage->tex_offset[1] = pStage->tex_offset[2] = offs;
			offs                 += shader->numVertexes * sizeof(vec2_t);

			if (pStage->bundle[1].image[0])
			{
				pStage->tex_offset[1] = offs;
				offs                 += shader->numVertexes * sizeof(vec2_t);
			}
		}

		shader->curVertexes = 0;
		shader->curIndexes  = 0;
	}

	// shift indexes relative to current shader
	for (i = 0; i < input->numIndexes; i++)
	{
		input->indexes[i] += shader->curVertexes;
	}

	if (vi->index_offset == -1)
	{
		// initialize geometry offsets relative to current shader
		vi->index_offset = shader->curIndexes;
		vi->soft_offset  = vbo->ibo_offset;
	}

	offs = shader->iboOffset + shader->curIndexes * sizeof(input->indexes[0]);
	size = input->numIndexes * sizeof(input->indexes[0]);
	if (offs + size > vbo->vbo_size)
	{
		Ren_Drop("VBO_AddGeometry: index overflow");
	}
	Com_Memcpy(vbo->vbo_buffer + offs, input->indexes, size);

	// fill soft buffer too
	if (vbo->ibo_offset + size > vbo->ibo_size)
	{
		Ren_Drop("VBO_AddGeometry: soft index overflow");
	}
	Com_Memcpy(vbo->ibo_buffer + vbo->ibo_offset, input->indexes, size);
	vbo->ibo_offset += size;

	// vertexes
	offs = shader->vboOffset + shader->curVertexes * sizeof(input->xyz[0]);
	size = input->numVertexes * sizeof(input->xyz[0]);
	if (offs + size > vbo->vbo_size)
	{
		Ren_Drop("VBO_AddGeometry: vertex overflow");
	}
	Com_Memcpy(vbo->vbo_buffer + offs, input->xyz, size);

	vi->num_indexes  += input->numIndexes;
	vi->num_vertexes += input->numVertexes;
}

/**
 * @brief Evaluate the stages of the tesselated surface and store the results
 * @param[in] itemIndex
 * @param[in,out] input
 */
static void VBO_PushData(int itemIndex, shaderCommands_t *input)
{
	shaderStage_t *pStage;
	vbo_t         *vbo = &world_vbo;
	vbo_item_t    *vi  = vbo->items + itemIndex;
	shader_t      *shader;
	int           i;

	VBO_AddGeometry(vbo, vi, input);

	shader = input->shader;

	for (i = 0; i < MAX_SHADER_STAGES; i++)
	{
		pStage = input->xstages[i];
		if (!pStage || !pStage->active)
		{
			break;
		}

		RB_ComputeStageVars(pStage);

		Com_Memcpy(vbo->vbo_buffer + pStage->rgb_offset[0] + shader->curVertexes * sizeof(color4ub_t),
		           input->svars.colors, input->numVertexes * sizeof(color4ub_t));

		Com_Memcpy(vbo->vbo_buffer + pStage->tex_offset[0] + shader->curVertexes * sizeof(vec2_t),
		           input->svars.texcoordPtr[0], input->numVertexes * sizeof(vec2_t));

		if (pStage->bundle[1].image[0])
		{
			Com_Memcpy(vbo->vbo_buffer + pStage->tex_offset[1] + shader->curVertexes * sizeof(vec2_t),
			           input->svars.texcoordPtr[1], input->numVertexes * sizeof(vec2_t));
		}
	}

	shader->curVertexes += input->numVertexes;
	shader->curIndexes  += input->numIndexes;
}

/**
 * @brief surfSortFunc
 * @param[in] a
 * @param[in] b
 * @return
 */
static int surfSortFunc(const void *a, const void *b)
{
	const msurface_t **sa = (const msurface_t **)a;
	const msurface_t **sb = (const msurface_t **)b;

	return (*sa)->shader->index - (*sb)->shader->index;
}

/**
 * @brief Find the static world surfaces and upload them to a vertex buffer
 * @param[in] surf
 * @param[in] surfCount
 */
void R_BuildWorldVBO(msurface_t *surf, int surfCount)
{
	vbo_t          *vbo = &world_vbo;
	msurface_t     **surfList;
	msurface_t     *sf;
	srfTriangles_t *tris;
	trRefEntity_t  *oldEntity;
	int            ibo_size;
	int            vbo_size;
	int            i, n;

	int numStaticSurfaces = 0;
	int numStaticIndexes  = 0;
	int numStaticVertexes = 0;

	VBO_Cleanup();

	if (!r_vbo->integer)
	{
		return;
	}

	vbo_size = 0;

	// initial scan to count surfaces/indexes/vertexes for memory allocation
	for (i = 0, sf = surf; i < surfCount; i++, sf++)
	{
		tris = (srfTriangles_t *) sf->data;
		if (tris->surfaceType != SF_TRIANGLES || !isStaticShader(sf->shader))
		{
			continue;
		}

		if (tris->numVerts >= SHADER_MAX_VERTEXES || tris->numIndexes >= SHADER_MAX_INDEXES)
		{
			continue;
		}

		tris->vboItemIndex = ++numStaticSurfaces;
		numStaticVertexes += tris->numVerts;
		numStaticIndexes  += tris->numIndexes;

		vbo_size                 += tris->numVerts * (sf->shader->svarsSize + sizeof(tess.xyz[0]));
		sf->shader->numVertexes += tris->numVerts;
		sf->shader->numIndexes  += tris->numIndexes;
	}

	if (numStaticSurfaces == 0)
	{
		Ren_Print("...no static surfaces for VBO\n");
		return;
	}

	vbo_size = PAD(vbo_size, 32);

	ibo_size = numStaticIndexes * sizeof(tess.indexes[0]);
	ibo_size = PAD(ibo_size, 32);

	// 0 item is unused
	vbo->items       = ri.Hunk_Alloc((numStaticSurfaces + 1) * sizeof(vbo_item_t), h_low);
	vbo->items_count = numStaticSurfaces;

	// last item will be used for run length termination
	vbo->items_queue       = ri.Hunk_Alloc((numStaticSurfaces + 1) * sizeof(int), h_low);
	vbo->items_queue_count = 0;

	Ren_Print("...found %i VBO surfaces (%i vertexes, %i indexes)\n",
	          numStaticSurfaces, numStaticVertexes, numStaticIndexes);

	// vertex buffer, the device-local indexes come first
	vbo_size        += ibo_size;
	vbo->vbo_buffer  = ri.Hunk_AllocateTempMemory(vbo_size);
	vbo->vbo_offset  = 0;
	vbo->vbo_size    = vbo_size;

	// index buffer
	vbo->ibo_buffer = ri.Hunk_Alloc(ibo_size, h_low);
	vbo->ibo_offset = 0;
	vbo->ibo_size   = ibo_size;

	// ibo runs buffer
	vbo->ibo_items       = ri.Hunk_Alloc(((numStaticIndexes / MIN_IBO_RUN) + 1) * sizeof(ibo_item_t), h_low);
	vbo->ibo_items_count = 0;

	surfList = ri.Hunk_AllocateTempMemory(numStaticSurfaces * sizeof(msurface_t *));

	for (i = 0, n = 0, sf = surf; i < surfCount; i++, sf++)
	{
		tris = (srfTriangles_t *) sf->data;
		if (tris->surfaceType == SF_TRIANGLES && tris->vboItemIndex)
		{
			surfList[n++] = sf;
		}
	}

	if (n != numStaticSurfaces)
	{
		Ren_Drop("R_BuildWorldVBO: invalid VBO surface count");
	}

	// sort surfaces by shader
	qsort(surfList, numStaticSurfaces, sizeof(surfList[0]), surfSortFunc);

	oldEntity             = backEnd.currentEntity;
	backEnd.currentEntity = &tr.worldEntity;

	for (i = 0; i < numStaticSurfaces; i++)
	{
		vbo_item_t *vi = vbo->items + i + 1;

		sf   = surfList[i];
		tris = (srfTriangles_t *) sf->data;

		tris->vboItemIndex = i + 1;

		vi->num_vertexes = 0;
		vi->num_indexes  = 0;
		vi->index_offset = -1;
		vi->soft_offset  = -1;

		RB_BeginSurface(sf->shader, 0);
		tess.allowVBO = qfalse; // the geometry must be tesselated here

		rb_surfaceTable[*sf->data](sf->data);

		// setup colors and texture coordinates
		VBO_PushData(i + 1, &tess);

		tess.numIndexes  = 0;
		tess.numVertexes = 0;
	}

	backEnd.currentEntity = oldEntity;

	ri.Hunk_FreeTempMemory(surfList);

	vk_alloc_vbo(vbo->vbo_buffer, vbo->vbo_size);

	// release host memory
	ri.Hunk_FreeTempMemory(vbo->vbo_buffer);
	vbo->vbo_buffer = NULL;
}

/**
 * @brief Forget the static surfaces, called before building a new VBO and on shutdown
 */
void VBO_Cleanup(void)
{
	int i;

	Com_Memset(&world_vbo, 0, sizeof(world_vbo));

	for (i = 0; i < tr.numShaders; i++)
	{
		tr.shaders[i]->isStaticShader = qfalse;
		tr.shaders[i]->iboOffset      = -1;
		tr.shaders[i]->vboOffset      = -1;
	}

	tess.vboIndex = 0;
}

/**
 * @brief Sort integers in ascending order
 * @param[in,out] a
 * @param[in] n
 */
static void qsort_int(int *a, const int n)
{
	int temp, m;
	int i, j;

	if (n < 32) // CUTOFF
	{
		for (i = 1 ; i < n + 1 ; i++)
		{
			j = i;
			while (j > 0 && a[j] < a[j - 1])
			{
				temp     = a[j];
				a[j]     = a[j - 1];
				a[j - 1] = temp;
				j--;
			}
		}
		return;
	}

	i = 0;
	j = n;
	m = a[n >> 1];

	do
	{
		while (a[i] < m)
			i++;
		while (a[j] > m)
			j--;
		if (i <= j)
		{
			temp = a[i];
			a[i] = a[j];
			a[j] = temp;
			i++;
			j--;
		}
	}
	while (i <= j);

	if (j > 0)
	{
		qsort_int(a, j);
	}
	if (n > i)
	{
		qsort_int(a + i, n - i);
	}
}

/**
 * @brief Count the consecutive items starting at from
 * @param[in] a
 * @param[in] from
 * @param[in] to
 * @param[out] count number of indexes in the run
 * @return number of items in the run
 */
static int run_length(const int *a, int from, int to, int *count)
{
	vbo_t *vbo = &world_vbo;
	int   i, n, cnt;

	for (cnt = 0, n = 1, i = from; i < to; i++, n++)
	{
		cnt += vbo->items[a[i]].num_indexes;
		if (a[i] + 1 != a[i + 1])
		{
			break;
		}
	}
	*count = cnt;
	return n;
}

/**
 * @brief VBO_QueueItem
 * @param[in] itemIndex
 */
void VBO_QueueItem(int itemIndex)
{
	vbo_t *vbo = &world_vbo;

	if (vbo->items_queue_count < vbo->items_count)
	{
		vbo->items_queue[vbo->items_queue_count++] = itemIndex;
	}
	else
	{
		Ren_Drop("VBO queue overflow");
	}
}

/**
 * @brief VBO_ClearQueue
 */
void VBO_ClearQueue(void)
{
	world_vbo.items_queue_count = 0;
}

/**
 * @brief Draw the queued items before tesselating a surface into the same batch
 */
void VBO_Flush(void)
{
	if (tess.vboIndex)
	{
		RB_EndSurface();
		tess.vboIndex = 0;
		RB_BeginSurface(tess.shader, tess.fogNum);
	}
}

/**
 * @brief VBO_AddItemDataToSoftBuffer
 * @param[in] itemIndex
 */
static void VBO_AddItemDataToSoftBuffer(int itemIndex)
{
	vbo_t            *vbo   = &world_vbo;
	const vbo_item_t *vi    = vbo->items + itemIndex;
	const uint32_t   offset = vk_tess_index(vi->num_indexes, vbo->ibo_buffer + vi->soft_offset);

	if (vbo->soft_buffer_indexes == 0)
	{
		// start recording into host-visible memory
		vbo->soft_buffer_offset = offset;
	}

	vbo->soft_buffer_indexes += vi->num_indexes;
}

/**
 * @brief VBO_AddItemRangeToIBOBuffer
 * @param[in] offset
 * @param[in] length
 */
static void VBO_AddItemRangeToIBOBuffer(int offset, int length)
{
	vbo_t      *vbo = &world_vbo;
	ibo_item_t *it;

	it = vbo->ibo_items + vbo->ibo_items_count++;

	it->offset = offset;
	it->length = length;
}

/**
 * @brief Draw the prepared runs, called by vk_draw_geometry()
 */
void VBO_RenderIBOItems(void)
{
	const vbo_t *vbo = &world_vbo;
	int         i;

	// from device-local memory
	if (vbo->ibo_items_count)
	{
		vk_bind_index_buffer(vk.vbo.vertex_buffer, tess.shader->iboOffset);

		for (i = 0; i < vbo->ibo_items_count; i++)
		{
			vk_draw_indexed(vbo->ibo_items[i].length, vbo->ibo_items[i].offset);
		}
	}

	// from host-visible memory
	if (vbo->soft_buffer_indexes)
	{
		vk_bind_index_buffer(vk.cmd->vertex_buffer, vbo->soft_buffer_offset);

		vk_draw_indexed(vbo->soft_buffer_indexes, 0);
	}
}

/**
 * @brief Sort the queued items and split them in runs, once per batch
 */
void VBO_PrepareQueues(void)
{
	vbo_t     *vbo = &world_vbo;
	int       i, item_run, index_run, n;
	const int *a;

	vbo->items_queue[vbo->items_queue_count] = 0; // terminate run

	// sort items so we can scan for longest runs
	if (vbo->items_queue_count > 1)
	{
		qsort_int(vbo->items_queue, vbo->items_queue_count - 1);
	}

	vbo->soft_buffer_indexes = 0;
	vbo->ibo_items_count     = 0;

	a = vbo->items_queue;
	i = 0;
	while (i < vbo->items_queue_count)
	{
		item_run = run_length(a, i, vbo->items_queue_count, &index_run);
		if (index_run < MIN_IBO_RUN)
		{
			for (n = 0; n < item_run; n++)
			{
				VBO_AddItemDataToSoftBuffer(a[i + n]);
			}
		}
		else
		{
			vbo_item_t *start = vbo->items + a[i];
			vbo_item_t *end   = vbo->items + a[i + item_run - 1];

			n = (end->index_offset - start->index_offset) + end->num_indexes;
			VBO_AddItemRangeToIBOBuffer(start->index_offset, n);
		}
		i += item_run;
	}
}
