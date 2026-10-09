/*
 * Wolfenstein: Enemy Territory GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
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
 * @file renderer_vk/tr_glcompat.h
 * @brief OpenGL constants and types still used as plain values by the shared
 * renderer code (texture wrap/filter modes, texture environments, fog modes).
 *
 * The Vulkan renderer never calls OpenGL, it only keeps these values so the
 * front end code shared with the OpenGL renderer can stay unchanged.
 */

#ifndef TR_GLCOMPAT_H
#define TR_GLCOMPAT_H

typedef unsigned int GLenum;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;

#define GL_FALSE                        0
#define GL_TRUE                         1

// texture environments, used to describe multitexture collapsing
#define GL_ADD                          0x0104
#define GL_MODULATE                     0x2100
#define GL_DECAL                        0x2101
#define GL_REPLACE                      0x1E01

// texture filters
#define GL_NEAREST                      0x2600
#define GL_LINEAR                       0x2601
#define GL_NEAREST_MIPMAP_NEAREST       0x2700
#define GL_LINEAR_MIPMAP_NEAREST        0x2701
#define GL_NEAREST_MIPMAP_LINEAR        0x2702
#define GL_LINEAR_MIPMAP_LINEAR         0x2703

// texture wrap modes
#define GL_CLAMP                        0x2900
#define GL_REPEAT                       0x2901
#define GL_CLAMP_TO_EDGE                0x812F

// texture formats, only used to describe images
#define GL_LUMINANCE                    0x1909
#define GL_LUMINANCE_ALPHA              0x190A
#define GL_RGB                          0x1907
#define GL_RGBA                         0x1908
#define GL_RGB5                         0x8050
#define GL_RGB8                         0x8051
#define GL_RGBA4                        0x8056
#define GL_RGBA8                        0x8058
#define GL_LUMINANCE8                   0x8040
#define GL_LUMINANCE16                  0x8042
#define GL_LUMINANCE8_ALPHA8            0x8045
#define GL_LUMINANCE16_ALPHA16          0x8048
#define GL_RGB4_S3TC                    0x83A1
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

// fog modes and hints (glfog_t)
#define GL_EXP                          0x0800
#define GL_DONT_CARE                    0x1100

// draw buffers
#define GL_FRONT                        0x0404
#define GL_BACK                         0x0405

#endif // TR_GLCOMPAT_H
