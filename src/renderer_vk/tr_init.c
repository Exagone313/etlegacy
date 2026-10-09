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
 * @file renderer_vk/tr_init.c
 * @brief Functions that are not called every frame
 */

#include "tr_local.h"

glconfig_t glConfig;
qboolean   textureFilterAnisotropic = qfalse;
float      maxAnisotropy            = 2.f;

glstate_t glState;
glstatic_t gls;

Vk_Instance vk;
Vk_World    vk_world;

/**
 * @brief This function is responsible for initializing a valid OpenGL subsystem
 *
 * This is done by calling GLimp_Init (which gives us a working OGL subsystem)
 * then setting variables, checking GL constants, and reporting the gfx system
 * config to the user.
 */
static void InitOpenGL(void)
{
	// initialize OS specific portions of the renderer
	//
	// GLimp_Init directly or indirectly references the following cvars:
	//          - r_fullscreen
	//          - r_mode
	//          - r_(color|depth|stencil)bits
	//          - r_ignorehwgamma
	//          - r_gamma

	if (glConfig.vidWidth == 0)
	{
		char glConfigString[1024] = { 0 };

		Com_Memset(&glConfig, 0, sizeof(glConfig));

		// the window is created with SDL_WINDOW_VULKAN and without OpenGL context
		Info_SetValueForKey(glConfigString, "type", "vulkan");
		Info_SetValueForKey(glConfigString, "major", "1");
		Info_SetValueForKey(glConfigString, "minor", "0");
		Info_SetValueForKey(glConfigString, "samples", "0");

		ri.GLimp_Init(&glConfig, glConfigString);

		gls.windowWidth  = glConfig.vidWidth;
		gls.windowHeight = glConfig.vidHeight;

		gls.captureWidth  = glConfig.vidWidth;
		gls.captureHeight = glConfig.vidHeight;

		// render at a custom resolution and scale it to the window in the final blit
		if (r_fbo->integer && r_renderScale->integer)
		{
			glConfig.vidWidth  = r_renderWidth->integer;
			glConfig.vidHeight = r_renderHeight->integer;

			// the image keeps its aspect ratio with black bars on the sides
			if ((r_renderScale->integer - 1) & 1)
			{
				glConfig.windowAspect = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
			}
		}

		if (r_fbo->integer && r_ext_supersample->integer)
		{
			glConfig.vidWidth  *= 2;
			glConfig.vidHeight *= 2;
		}

		vk_initialize();

		gls.deviceSupportsGamma = glConfig.deviceSupportsGamma;

		// print info
		GfxInfo_f();

		gls.initTime = ri.Milliseconds();
	}

	if (!vk.active)
	{
		// might happen after a shutdown which kept the window
		vk_initialize();
		gls.initTime = ri.Milliseconds();
	}

	if (vk.active)
	{
		vk_init_descriptors();
	}
	else
	{
		Ren_Fatal("Recursive error during Vulkan initialization");
	}

	RB_ClearPipelineCache();

	// set default state
	GL_SetDefaultState();
}

/**
 * @brief GL_CheckErrors
 */
void GL_CheckErrors(void)
{
	// Vulkan errors are checked by the backend on each call
}

/*
 * ==============================================================================
 *
 *                                                SCREEN SHOTS
 *
 * NOTE: some thoughts about the screenshots system:
 * screenshots get written in fs_homepath + fs_gamedir
 * vanilla W:ET .. etmain/screenshots/<FILE>.tga
 * ET: Legacy   .. legacy/screenshots/<FILE>.jpg
 *
 * one command: "screenshot"
 * we use statics to store a count and start writing the first screenshot/screenshot????.jpg available
 * (with FS_FileExists / FS_FOpenFileWrite calls)
 * FIXME: the statics don't get a reinit between fs_game changes
 *
 * ==============================================================================
 */

/**
 * @brief Reads an image but takes care of alignment issues for reading RGB images.
 *
 * @details Reads a minimum offset for where the RGB data starts in the image from
 * integer stored at pointer offset. When the function has returned the actual
 * offset was written back to address offset. This address will always have an
 * alignment of packAlign to ensure efficient copying.
 *
 * Stores the length of padding after a line of pixels to address padlen
 *
 * Return value must be freed with ri.Hunk_FreeTempMemory()
 *
 * @param[in] x
 * @param[in] y
 * @param[in] width
 * @param[in] height
 * @param[in,out] offset
 * @param[in,out] padlen
 * @return
 */
byte *RB_ReadPixels(int x, int y, int width, int height, size_t *offset, int *padlen)
{
	byte *buffer;

	// the Vulkan backend always reads the whole capture area, tightly packed
	buffer = ri.Hunk_AllocateTempMemory(width * height * 3 + *offset);

	vk_read_pixels(buffer + *offset, width, height);

	*padlen = 0;

	return buffer;
}

/*
 * @brief zbuffer writer for the future implementation of the Depth of field effect
 * @param[in] x
 * @param[in] y
 * @param[in] width
 * @param[in] height
 * @param[in] fileName
 *
 * @note Unused.
void RB_TakeDepthshot(int x, int y, int width, int height, const char *fileName)
{
    byte   *allbuf, *buffer;
    byte   *srcptr, *destptr;
    byte   *endline, *endmem;
    int    linelen, padlen;
    size_t offset = 18, memcount;

    allbuf = RB_ReadZBuffer(x, y, width, height, &padlen);
    buffer = ri.Hunk_AllocateTempMemory(width * height * 3 + offset);

    Com_Memset(buffer, 0, 18);
    buffer[2]  = 2;         // uncompressed type
    buffer[12] = width & 255;
    buffer[13] = width >> 8;
    buffer[14] = height & 255;
    buffer[15] = height >> 8;
    buffer[16] = 24;        // pixel size

    linelen = width;

    srcptr  = allbuf;
    destptr = buffer + offset;
    endmem  = srcptr + (linelen + padlen) * height;
    while (srcptr < endmem)
    {
        endline = srcptr + linelen;

        while (srcptr < endline)
        {
            *destptr++ = srcptr[0];
            *destptr++ = srcptr[0];
            *destptr++ = srcptr[0];

            srcptr++;
        }

        // Skip the pad
        srcptr += padlen;
    }

    memcount = linelen * 3 * height + offset;

    ri.FS_WriteFile(fileName, buffer, memcount);

    ri.Hunk_FreeTempMemory(allbuf);
    ri.Hunk_FreeTempMemory(buffer);
}
*/

static screenshotCommand_t pendingScreenshot;
static char                pendingScreenshotName[MAX_OSPATH];
static videoFrameCommand_t pendingVideoFrame;

/**
 * @brief Read back the last submitted frame and hand it to the AVI writer
 * @param[in] cmd
 */
static void RB_WriteVideoFrame(const videoFrameCommand_t *cmd)
{
	byte   *cBuf;
	size_t memcount, linelen;
	int    avipadwidth, avipadlen;

	linelen = cmd->width * 3;

	// AVI line padding
	avipadwidth = PAD(linelen, AVI_LINE_PADDING);
	avipadlen   = avipadwidth - linelen;

	cBuf = cmd->captureBuffer;

	vk_read_pixels(cBuf, cmd->width, cmd->height);

	memcount = linelen * cmd->height;

	// gamma correct
	if (glConfig.deviceSupportsGamma && vk.capture.image == VK_NULL_HANDLE)
	{
		R_GammaCorrect(cBuf, memcount);
	}

	if (cmd->motionJpeg)
	{
		memcount = RE_SaveJPGToBuffer(cmd->encodeBuffer, linelen * cmd->height,
		                              r_screenshotJpegQuality->integer,
		                              cmd->width, cmd->height, cBuf, 0);
		ri.CL_WriteAVIVideoFrame(cmd->encodeBuffer, memcount);
	}
	else
	{
		byte *lineend, *memend;
		byte *srcptr, *destptr;

		srcptr  = cBuf;
		destptr = cmd->encodeBuffer;
		memend  = srcptr + memcount;

		// swap R and B and add line paddings
		while (srcptr < memend)
		{
			lineend = srcptr + linelen;
			while (srcptr < lineend)
			{
				*destptr++ = srcptr[2];
				*destptr++ = srcptr[1];
				*destptr++ = srcptr[0];
				srcptr    += 3;
			}

			Com_Memset(destptr, '\0', avipadlen);
			destptr += avipadlen;
		}

		ri.CL_WriteAVIVideoFrame(cmd->encodeBuffer, avipadwidth * cmd->height);
	}
}

/**
 * @brief RB_TakeScreenshotTGA
 * @param[in] x
 * @param[in] y
 * @param[in] width
 * @param[in] height
 * @param[in] fileName
 */
void RB_TakeScreenshotTGA(int x, int y, int width, int height, const char *fileName)
{
	byte   *allbuf, *buffer;
	byte   *srcptr, *destptr;
	byte   *endline, *endmem;
	byte   temp;
	int    linelen, padlen;
	size_t offset = 18, memcount;

	allbuf = RB_ReadPixels(0, 0, gls.captureWidth, gls.captureHeight, &offset, &padlen);
	buffer = allbuf + offset - 18;

	Com_Memset(buffer, 0, 18);
	buffer[2]  = 2;         // uncompressed type
	buffer[12] = width & 255;
	buffer[13] = width >> 8;
	buffer[14] = height & 255;
	buffer[15] = height >> 8;
	buffer[16] = 24;        // pixel size

	// swap rgb to bgr and remove padding from line endings
	linelen = width * 3;

	srcptr = destptr = allbuf + offset;
	endmem = srcptr + (linelen + padlen) * height;

	while (srcptr < endmem)
	{
		endline = srcptr + linelen;

		while (srcptr < endline)
		{
			temp       = srcptr[0];
			*destptr++ = srcptr[2];
			*destptr++ = srcptr[1];
			*destptr++ = temp;

			srcptr += 3;
		}

		// Skip the pad
		srcptr += padlen;
	}

	memcount = linelen * height;

	// gamma correct
	if (glConfig.deviceSupportsGamma && vk.capture.image == VK_NULL_HANDLE)
	{
		R_GammaCorrect(allbuf + offset, memcount);
	}

	ri.FS_WriteFile(fileName, buffer, memcount + 18);

	ri.Hunk_FreeTempMemory(allbuf);
}

/**
 * @brief RB_TakeScreenshotJPEG
 * @param[in] x
 * @param[in] y
 * @param[in] width
 * @param[in] height
 * @param[in] fileName
 */
void RB_TakeScreenshotJPEG(int x, int y, int width, int height, char *fileName)
{
	byte   *buffer;
	size_t offset = 0, memcount;
	int    padlen;

	buffer   = RB_ReadPixels(0, 0, gls.captureWidth, gls.captureHeight, &offset, &padlen);
	memcount = (width * 3 + padlen) * height;

	// gamma correct
	if (glConfig.deviceSupportsGamma && vk.capture.image == VK_NULL_HANDLE)
	{
		R_GammaCorrect(buffer + offset, memcount);
	}

	RE_SaveJPG(fileName, r_screenshotJpegQuality->integer, width, height, buffer + offset, padlen);
	ri.Hunk_FreeTempMemory(buffer);
}

#ifdef FEATURE_PNG
/**
 * @brief RB_TakeScreenshotPNG
 * @param[in] x
 * @param[in] y
 * @param[in] width
 * @param[in] height
 * @param[in] fileName
 */
void RB_TakeScreenshotPNG(int x, int y, int width, int height, char *fileName)
{
	byte   *buffer;
	size_t offset = 0, memcount;
	int    padlen;

	buffer   = RB_ReadPixels(0, 0, gls.captureWidth, gls.captureHeight, &offset, &padlen);
	memcount = (width * 3 + padlen) * height;

	// gamma correct
	if (glConfig.deviceSupportsGamma && vk.capture.image == VK_NULL_HANDLE)
	{
		R_GammaCorrect(buffer + offset, memcount);
	}

	RE_SavePNG(fileName, width, height, buffer + offset, padlen);
	ri.Hunk_FreeTempMemory(buffer);
}
#endif

/**
 * @brief RB_TakeScreenshotCmd
 * @param[in] data
 * @return
 */
const void *RB_TakeScreenshotCmd(const void *data)
{
	const screenshotCommand_t *cmd = ( const screenshotCommand_t * ) data;

	// the frame is still being recorded, defer the capture to RB_SwapBuffers
	pendingScreenshot = *cmd;
	Q_strncpyz(pendingScreenshotName, cmd->fileName, sizeof(pendingScreenshotName));
	pendingScreenshot.fileName = pendingScreenshotName;
	backEnd.screenshotMask    |= SCREENSHOT_MASK_IMAGE;

	return ( const void * ) (cmd + 1);
}

/**
 * @brief Write the screenshots and video frames requested during this frame,
 * called by RB_SwapBuffers once the frame has been submitted.
 */
void RB_TakePendingScreenshots(void)
{
	if (backEnd.screenshotMask & SCREENSHOT_MASK_IMAGE)
	{
		const screenshotCommand_t *cmd = &pendingScreenshot;

		switch (cmd->format)
		{
		case SSF_TGA:
			RB_TakeScreenshotTGA(cmd->x, cmd->y, cmd->width, cmd->height, cmd->fileName);
			break;
		case SSF_JPEG:
			RB_TakeScreenshotJPEG(cmd->x, cmd->y, cmd->width, cmd->height, (char *)cmd->fileName);
			break;
#ifdef FEATURE_PNG
		case SSF_PNG:
			RB_TakeScreenshotPNG(cmd->x, cmd->y, cmd->width, cmd->height, (char *)cmd->fileName);
			break;
#endif
		}
	}

	if (backEnd.screenshotMask & SCREENSHOT_MASK_VIDEO)
	{
		RB_WriteVideoFrame(&pendingVideoFrame);
	}

	backEnd.screenshotMask = 0;
}

/**
 * @brief R_TakeScreenshot
 * @param[in] x
 * @param[in] y
 * @param[in] width
 * @param[in] height
 * @param[in] name
 * @param[in] format
 */
void R_TakeScreenshot(int x, int y, int width, int height, const char *name, ssFormat_t format)
{
	static char         fileName[MAX_OSPATH]; // bad things if two screenshots per frame?
	screenshotCommand_t *cmd;

	cmd = R_GetCommandBuffer(sizeof(*cmd));
	if (!cmd)
	{
		return;
	}
	cmd->commandId = RC_SCREENSHOT;

	cmd->x      = x;
	cmd->y      = y;
	cmd->width  = width;
	cmd->height = height;
	Q_strncpyz(fileName, name, sizeof(fileName));
	cmd->fileName = fileName;
	cmd->format   = format;
}

/**
 * @brief R_ScreenshotFilename
 * @param[in] lastNumber
 * @param[out] fileName
 */
void R_ScreenshotFilename(int lastNumber, char *fileName, char *ext)
{
	int a, b, c, d;

	if (lastNumber < 0 || lastNumber > 9999)
	{
		Com_sprintf(fileName, MAX_OSPATH, "screenshots/shot9999.%s", ext);
		return;
	}

	a           = lastNumber / 1000;
	lastNumber -= a * 1000;
	b           = lastNumber / 100;
	lastNumber -= b * 100;
	c           = lastNumber / 10;
	lastNumber -= c * 10;
	d           = lastNumber;

	Com_sprintf(fileName, MAX_OSPATH, "screenshots/shot%i%i%i%i.%s"
	            , a, b, c, d, ext);
}

/**
 * @brief RB_TakeVideoFrameCmd
 * @param[in] data
 * @return
 */
const void *RB_TakeVideoFrameCmd(const void *data)
{
	const videoFrameCommand_t *cmd = (const videoFrameCommand_t *)data;

	// the frame is still being recorded, defer the capture to RB_SwapBuffers
	pendingVideoFrame       = *cmd;
	backEnd.screenshotMask |= SCREENSHOT_MASK_VIDEO;

	return (const void *)(cmd + 1);
}

/**
 * @brief Levelshots are specialized 128*128 thumbnails for
 * the menu system, sampled down from full screen distorted images
 */
void R_LevelShot(void)
{
	char   checkname[MAX_OSPATH];
	byte   *buffer;
	byte   *source, *allsource;
	byte   *src, *dst;
	size_t offset = 0;
	int    padlen;
	int    x, y;
	int    r, g, b;
	float  xScale, yScale;
	int    xx, yy;

	Com_sprintf(checkname, sizeof(checkname), "levelshots/%s.tga", tr.world->baseName);

	allsource = RB_ReadPixels(0, 0, gls.captureWidth, gls.captureHeight, &offset, &padlen);
	source    = allsource + offset;

	buffer = ri.Hunk_AllocateTempMemory(128 * 128 * 3 + 18);
	Com_Memset(buffer, 0, 18);
	buffer[2]  = 2;         // uncompressed type
	buffer[12] = 128;
	buffer[14] = 128;
	buffer[16] = 24;        // pixel size

	// resample from source
	xScale = glConfig.windowWidth / 512.0f;
	yScale = glConfig.windowHeight / 384.0f;
	for (y = 0 ; y < 128 ; y++)
	{
		for (x = 0 ; x < 128 ; x++)
		{
			r = g = b = 0;
			for (yy = 0 ; yy < 3 ; yy++)
			{
				for (xx = 0 ; xx < 4 ; xx++)
				{
					src = source + (3 * glConfig.windowWidth + padlen) * ( int ) ((y * 3 + yy) * yScale) +
					      3 * ( int ) ((x * 4 + xx) * xScale);
					r += src[0];
					g += src[1];
					b += src[2];
				}
			}
			dst    = buffer + 18 + 3 * (y * 128 + x);
			dst[0] = (byte)(b / 12);
			dst[1] = (byte)(g / 12);
			dst[2] = (byte)(r / 12);
		}
	}

	// gamma correct
	if (glConfig.deviceSupportsGamma && vk.capture.image == VK_NULL_HANDLE)
	{
		R_GammaCorrect(buffer + 18, 128 * 128 * 3);
	}

	ri.FS_WriteFile(checkname, buffer, 128 * 128 * 3 + 18);

	ri.Hunk_FreeTempMemory(buffer);
	ri.Hunk_FreeTempMemory(allsource);

	Ren_Print("Wrote %s\n", checkname);
}

/**
 * @brief R_ScreenShot_f
 *
 * @note Doesn't print the pacifier message if there is a second arg
 *
 * screenshot
 * screenshot [silent]
 * screenshot [levelshot]
 * screenshot [filename]
 */
void R_ScreenShot_f(void)
{
	char       checkname[MAX_OSPATH];
	static int lastNumber = -1;
	qboolean   silent;
	char       *ext = "";

	ssFormat_t format = r_screenshotFormat->integer;

	// Backwards compatibility
	if (!Q_stricmp(ri.Cmd_Argv(0), "screenshotJPEG"))
	{
		format = SSF_JPEG;
	}

	switch (format)
	{
	case SSF_TGA:
		ext = "tga";
		break;
	case SSF_JPEG:
		ext = "jpg";
		break;
#ifdef FEATURE_PNG
	case SSF_PNG:
		ext = "png";
		break;
#endif
	default:
		return;
	}

	if (!strcmp(ri.Cmd_Argv(1), "levelshot"))
	{
		R_LevelShot();
		return;
	}

	if (!strcmp(ri.Cmd_Argv(1), "silent"))
	{
		silent = qtrue;
	}
	else
	{
		silent = qfalse;
	}

	if (ri.Cmd_Argc() == 2 && !silent)
	{
		// explicit filename
		const char *fileExt = COM_GetExtension(ri.Cmd_Argv(1));
		if (fileExt[0])
		{
			char filename[MAX_QPATH];
			COM_StripExtension(ri.Cmd_Argv(1), filename, MAX_QPATH);

			if (COM_CompareExtension(fileExt, "tga"))
			{
				ext    = "tga";
				format = SSF_TGA;
			}
			else if (COM_CompareExtension(fileExt, "jpg") || COM_CompareExtension(fileExt, "jpeg"))
			{
				ext    = "jpg";
				format = SSF_JPEG;
			}
#ifdef FEATURE_PNG
			else if (COM_CompareExtension(fileExt, "png"))
			{
				ext    = "png";
				format = SSF_PNG;
			}
#endif

			Com_sprintf(checkname, MAX_OSPATH, "screenshots/%s.%s", filename, ext);
		}
		else
		{
			Com_sprintf(checkname, MAX_OSPATH, "screenshots/%s.%s", ri.Cmd_Argv(1), ext);
		}
	}
	else
	{
		// scan for a free filename

		// if we have saved a previous screenshot, don't scan
		// again, because recording demo avis can involve
		// thousands of shots
		if (lastNumber == -1)
		{
			lastNumber = 0;
		}
		// scan for a free number
		for ( ; lastNumber <= 99999 ; lastNumber++)
		{
			R_ScreenshotFilename(lastNumber, checkname, ext);

			if (!ri.FS_FileExists(checkname))
			{
				break; // file doesn't exist
			}
		}

		if (lastNumber >= 99999)
		{
			Ren_Print("ScreenShot: Couldn't create a file\n");
			return;
		}

		lastNumber++;
	}

	R_TakeScreenshot(0, 0, glConfig.windowWidth, glConfig.windowHeight, checkname, format);

	if (!silent)
	{
		Ren_Print("Wrote %s\n", checkname);
	}
}

//============================================================================

/**
 * @brief GL_SetDefaultState
 */
void GL_SetDefaultState(void)
{
	GL_TextureMode(r_textureMode->string);

	GL_SelectTexture(1);
	GL_TexEnv(GL_MODULATE);
	GL_SelectTexture(0);
	GL_TexEnv(GL_MODULATE);

	glState.faceCulling   = CT_TWO_SIDED;
	glState.polygonOffset = qfalse;

	// make sure our GL state vector is set correctly
	glState.glStateBits = GLS_DEPTHTEST_DISABLE | GLS_DEPTHMASK_TRUE;
}

/**
 * @brief GfxInfo_f
 */
void GfxInfo_f(void)
{
	const char *fsstrings[] =
	{
		"windowed",
		"fullscreen"
	};

	Ren_Print("VK_VENDOR: %s\n", glConfig.vendor_string);
	Ren_Print("VK_RENDERER: %s\n", glConfig.renderer_string);
	Ren_Print("VK_VERSION: %s\n", glConfig.version_string);

	if (vk.driverNote[0] != '\0')
	{
		Ren_Print("%s", vk.driverNote);
	}

	if (r_gfxInfo->integer > 0)
	{
		Ren_Print("VK_EXTENSIONS: ");
		R_PrintLongString(glConfig.extensions_string);
		Ren_Print("\n");
	}

	Ren_Print("VK_MAX_TEXTURE_SIZE: %d\n", glConfig.maxTextureSize);
	Ren_Print("VK_MAX_TEXTURE_UNITS: %d\n", glConfig.maxActiveTextures);
	Ren_Print("PIXELFORMAT: color(%d-bits) Z(%d-bit) stencil(%d-bits)\n", glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits);
	Ren_Print(" presentation: %s\n", vk_format_string(vk.present_format.format));
	if (vk.color_format != vk.present_format.format)
	{
		Ren_Print(" color: %s\n", vk_format_string(vk.color_format));
	}
	if (vk.capture_format != vk.present_format.format || vk.capture_format != vk.color_format)
	{
		Ren_Print(" capture: %s\n", vk_format_string(vk.capture_format));
	}
	Ren_Print(" depth: %s\n", vk_format_string(vk.depth_format));

	if (glConfig.vidWidth != gls.windowWidth || glConfig.vidHeight != gls.windowHeight)
	{
		Ren_Print("RENDER: %d x %d, ", glConfig.vidWidth, glConfig.vidHeight);
	}
	Ren_Print("MODE: %d, SCREEN: %d x %d %s (ratio %.4f) Hz:", ri.Cvar_VariableIntegerValue("r_mode"), gls.windowWidth, gls.windowHeight, fsstrings[ri.Cvar_VariableIntegerValue("r_fullscreen") == 1], glConfig.windowAspect);

	if (glConfig.displayFrequency)
	{
		Ren_Print("%d\n", glConfig.displayFrequency);
	}
	else
	{
		Ren_Print("N/A\n");
	}

	if (glConfig.deviceSupportsGamma)
	{
		Ren_Print("GAMMA: hardware w/ %d overbright bits\n", tr.overbrightBits);
	}
	else
	{
		Ren_Print("GAMMA: software w/ %d overbright bits\n", tr.overbrightBits);
	}

	Ren_Print("texturemode: %s\n", r_textureMode->string);
	Ren_Print("picmip: %d\n", r_picMip->integer);
	Ren_Print("texture bits: %d\n", r_textureBits->integer);

	if (r_finish->integer)
	{
		Ren_Print("Forcing glFinish\n");
	}
}

/**
 * @brief Print Vulkan backend statistics
 */
void VkInfo_f(void)
{
	Ren_Print("max_vertex_usage: %iKb\n", (int)((vk.stats.vertex_buffer_max + 1023) / 1024));
	Ren_Print("max_push_size: %ib\n", vk.stats.push_size_max);
	Ren_Print("pipeline handles: %i\n", vk.pipeline_create_count);
	Ren_Print("pipeline descriptors: %i, base: %i\n", vk.pipelines_count, vk.pipelines_world_base);
	Ren_Print("image chunks: %i\n", vk_world.num_image_chunks);
}

/**
 * @brief R_Init
 */
void R_Init(void)
{
	int  i;
	byte *ptr;

	Ren_Print("----- Initializing Renderer ----\n");

	// clear all our internal state
	Com_Memset(&tr, 0, sizeof(tr));
	Com_Memset(&backEnd, 0, sizeof(backEnd));
	Com_Memset(&tess, 0, sizeof(tess));

	if ((intptr_t) tess.xyz & 15)
	{
		Ren_Warning("tess.xyz not 16 byte aligned\n");
	}
	Com_Memset(tess.constantColor255, 255, sizeof(tess.constantColor255));

	// init function tables
	for (i = 0; i < FUNCTABLE_SIZE; i++)
	{
		tr.sinTable[i]             = sin(DEG2RAD(i * 360.0f / (( float ) (FUNCTABLE_SIZE - 1))));
		tr.squareTable[i]          = (i < FUNCTABLE_SIZE / 2) ? 1.0f : -1.0f;
		tr.sawToothTable[i]        = ( float ) i / FUNCTABLE_SIZE;
		tr.inverseSawToothTable[i] = 1.0f - tr.sawToothTable[i];

		if (i < FUNCTABLE_SIZE / 2)
		{
			if (i < FUNCTABLE_SIZE / 4)
			{
				tr.triangleTable[i] = ( float ) i / (FUNCTABLE_SIZE / 4);
			}
			else
			{
				tr.triangleTable[i] = 1.0f - tr.triangleTable[i - FUNCTABLE_SIZE / 4];
			}
		}
		else
		{
			tr.triangleTable[i] = -tr.triangleTable[i - FUNCTABLE_SIZE / 2];
		}
	}

	// init the virtual memory
	R_Hunk_Begin();

	R_NoiseInit();

	R_Register();

	ptr = ri.Hunk_Alloc(sizeof(*backEndData) + sizeof(srfPoly_t) * r_maxPolys->integer + sizeof(polyVert_t) * r_maxPolyVerts->integer, h_low);

	backEndData            = (backEndData_t *) ptr;
	backEndData->polys     = (srfPoly_t *) ((char *) ptr + sizeof(*backEndData));
	backEndData->polyVerts = (polyVert_t *) ((char *) ptr + sizeof(*backEndData) + sizeof(srfPoly_t) * r_maxPolys->integer);

	R_InitNextFrame();

	InitOpenGL();

	R_InitImages();

	vk_create_pipelines();

	R_InitShaders();

	R_InitSkins();

	R_ModelInit();

	R_InitFreeType();

	R_InitSplash();

	Ren_Print("--------------------------------\n");
}

void R_PurgeCache(void)
{
	R_PurgeDynamicShaders();
	R_PurgeShaders(9999999);
	R_PurgeBackupImages(9999999);
	R_PurgeModels(9999999);
}

/**
 * @brief RE_Shutdown
 * @param[in] destroyWindow
 */
void RE_Shutdown(qboolean destroyWindow)
{
	Ren_Print("RE_Shutdown( %i )\n", destroyWindow);

	ri.Cmd_RemoveSystemCommand("imagelist");
	ri.Cmd_RemoveSystemCommand("shaderlist");
	ri.Cmd_RemoveSystemCommand("skinlist");
	ri.Cmd_RemoveSystemCommand("modellist");
	ri.Cmd_RemoveSystemCommand("screenshot");
	ri.Cmd_RemoveSystemCommand("screenshotJPEG");
	ri.Cmd_RemoveSystemCommand("gfxinfo");
	ri.Cmd_RemoveSystemCommand("vkinfo");
	ri.Cmd_RemoveSystemCommand("taginfo");

	// clean out any remaining unused media from the last backup
	R_PurgeCache();

	// the media cache is disabled with Vulkan, see R_Register()
	if (tr.registered)
	{
		R_IssuePendingRenderCommands();
	}

	if (vk.active && vk.frame_count)
	{
		// a frame was interrupted (e.g. by a drop error), submit what was recorded
		vk_end_frame();
		vk_present_frame();
	}

	R_DeleteTextures();

	R_DoneFreeType();

	if (vk.active)
	{
		vk_release_resources();
	}

	// shut down platform specific Vulkan stuff
	if (destroyWindow)
	{
		vk_shutdown(r_device->modified ? REF_UNLOAD_DLL : REF_DESTROY_WINDOW);

		R_DoGLimpShutdown();

		// release the virtual memory
		R_Hunk_End();
		R_FreeImageBuffer();
		ri.Tag_Free();  // wipe all render alloc'd zone memory
	}

	tr.registered = qfalse;
}

/**
 * @brief Touch all images to make sure they are resident
 */
void RE_EndRegistration(void)
{
	R_IssuePendingRenderCommands();
	/*
	RB: disabled unneeded reference to Sys_LowPhysicalMemory
	if (!Sys_LowPhysicalMemory())
	{
	//              RB_ShowImages();
	}
	*/
}

/**
 * @brief Called by GLimp_SetMode() once the window is created, nothing to check
 * for Vulkan as the device is selected later by vk_initialize()
 * @return
 */
int RE_InitOpenGlSubsystems(void)
{
	return qtrue;
}

/**
 * @brief Called by GLimp_Init() once the window is ready
 */
void RE_InitOpenGl(void)
{
	glConfig.driverType   = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
}

/**
 * @brief Destroy the window
 */
void R_DoGLimpShutdown(void)
{
	ri.GLimp_Shutdown();

	Com_Memset(&glConfig, 0, sizeof(glConfig));
	Com_Memset(&glState, 0, sizeof(glState));
}

void R_DebugPolygon(int color, int numPoints, float *points);

#ifdef USE_RENDERER_DLOPEN
/**
 * @brief GetRefAPI
 * @param[in] apiVersion
 * @param[in] rimp
 * @return
 */
Q_EXPORT refexport_t *QDECL GetRefAPI(int apiVersion, refimport_t *rimp)
#else
refexport_t *GetRefAPI(int apiVersion, refimport_t *rimp)
#endif
{
	static refexport_t re;

	ri = *rimp;

	Com_Memset(&re, 0, sizeof(re));

	if (apiVersion != REF_API_VERSION)
	{
		Ren_Print("Mismatched REF_API_VERSION: expected %i, got %i\n", REF_API_VERSION, apiVersion);
		return NULL;
	}

	// the RE_ functions are Renderer Entry points

	re.Shutdown = RE_Shutdown;

	re.BeginRegistration = RE_BeginRegistration;
	re.RegisterModel     = RE_RegisterModel;
	re.RegisterSkin      = RE_RegisterSkin;

	re.GetSkinModel       = RE_GetSkinModel;
	re.GetShaderFromModel = RE_GetShaderFromModel;

	re.RegisterShader      = RE_RegisterShader;
	re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
	re.LoadWorld           = RE_LoadWorldMap;
	re.SetWorldVisData     = RE_SetWorldVisData;
	re.EndRegistration     = RE_EndRegistration;

	re.BeginFrame = RE_BeginFrame;
	re.EndFrame   = RE_EndFrame;

	re.MarkFragments = R_MarkFragments;
	re.ProjectDecal  = RE_ProjectDecal;
	re.ClearDecals   = RE_ClearDecals;

	re.LerpTag     = R_LerpTag;
	re.ModelBounds = R_ModelBounds;

	re.ClearScene          = RE_ClearScene;
	re.AddRefEntityToScene = RE_AddRefEntityToScene;

	re.AddPolyToScene  = RE_AddPolyToScene;
	re.AddPolysToScene = RE_AddPolysToScene;
	re.AddLightToScene = RE_AddLightToScene;

	re.AddCoronaToScene = RE_AddCoronaToScene;
	re.SetFog           = R_SetFog;

	re.RenderScene = RE_RenderScene;

	re.SetColor               = RE_SetColor;
	re.DrawStretchPic         = RE_StretchPic;
	re.DrawRotatedPic         = RE_RotatedPic;
	re.Add2dPolys             = RE_2DPolyies;
	re.DrawStretchPicGradient = RE_StretchPicGradient;
	re.DrawStretchRaw         = RE_StretchRaw;
	re.UploadCinematic        = RE_UploadCinematic;
	re.RegisterFont           = RE_RegisterFont;
	re.RemapShader            = R_RemapShader;
	re.GetEntityToken         = R_GetEntityToken;

	re.DrawDebugPolygon = R_DebugPolygon;
	re.DrawDebugText    = R_DebugText;

	re.AddPolyBufferToScene = RE_AddPolyBufferToScene;

	re.SetGlobalFog = RE_SetGlobalFog;

	re.inPVS = R_inPVS;

	re.purgeCache = R_PurgeCache;

	re.LoadDynamicShader = RE_LoadDynamicShader;
	re.GetTextureId      = R_GetTextureId;

	re.RenderToTexture = RE_RenderToTexture;

	re.Finish              = RE_Finish;
	re.TakeVideoFrame      = RE_TakeVideoFrame;
	re.InitOpenGL          = RE_InitOpenGl;
	re.InitOpenGLSubSystem = RE_InitOpenGlSubsystems;

	return &re;
}
