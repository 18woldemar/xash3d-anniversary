/*
r_gl.h - ref_gl3: the OpenGL 3.3 core entry points the renderer uses
Copyright (C) 2026 18woldemar

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef R_GL_H
#define R_GL_H

// The core profile's header declares no functions for a loader to fight with: every entry point below is a
// pointer fetched through the engine's GL_GetProcAddress at start. A function missing from this list does not
// exist for the renderer, which is the point - nothing of the fixed-function pipeline can creep back in.
#define GL_GLEXT_PROTOTYPES 0
#include <GL/glcorearb.h>

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#define GL3_FUNCTIONS( X ) \
	X( PFNGLGETSTRINGPROC, glGetString ) \
	X( PFNGLGETSTRINGIPROC, glGetStringi ) \
	X( PFNGLGETINTEGERVPROC, glGetIntegerv ) \
	X( PFNGLGETFLOATVPROC, glGetFloatv ) \
	X( PFNGLGETERRORPROC, glGetError ) \
	X( PFNGLENABLEPROC, glEnable ) \
	X( PFNGLDISABLEPROC, glDisable ) \
	X( PFNGLCLEARPROC, glClear ) \
	X( PFNGLCLEARCOLORPROC, glClearColor ) \
	X( PFNGLCLEARDEPTHPROC, glClearDepth ) \
	X( PFNGLVIEWPORTPROC, glViewport ) \
	X( PFNGLSCISSORPROC, glScissor ) \
	X( PFNGLDEPTHRANGEPROC, glDepthRange ) \
	X( PFNGLDEPTHFUNCPROC, glDepthFunc ) \
	X( PFNGLDEPTHMASKPROC, glDepthMask ) \
	X( PFNGLCOLORMASKPROC, glColorMask ) \
	X( PFNGLBLENDFUNCPROC, glBlendFunc ) \
	X( PFNGLCULLFACEPROC, glCullFace ) \
	X( PFNGLFRONTFACEPROC, glFrontFace ) \
	X( PFNGLPOLYGONOFFSETPROC, glPolygonOffset ) \
	X( PFNGLPIXELSTOREIPROC, glPixelStorei ) \
	X( PFNGLREADPIXELSPROC, glReadPixels ) \
	X( PFNGLFINISHPROC, glFinish ) \
	X( PFNGLGENTEXTURESPROC, glGenTextures ) \
	X( PFNGLDELETETEXTURESPROC, glDeleteTextures ) \
	X( PFNGLBINDTEXTUREPROC, glBindTexture ) \
	X( PFNGLACTIVETEXTUREPROC, glActiveTexture ) \
	X( PFNGLTEXIMAGE2DPROC, glTexImage2D ) \
	X( PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D ) \
	X( PFNGLTEXPARAMETERIPROC, glTexParameteri ) \
	X( PFNGLTEXPARAMETERFPROC, glTexParameterf ) \
	X( PFNGLGENBUFFERSPROC, glGenBuffers ) \
	X( PFNGLDELETEBUFFERSPROC, glDeleteBuffers ) \
	X( PFNGLBINDBUFFERPROC, glBindBuffer ) \
	X( PFNGLBINDBUFFERBASEPROC, glBindBufferBase ) \
	X( PFNGLBUFFERDATAPROC, glBufferData ) \
	X( PFNGLBUFFERSUBDATAPROC, glBufferSubData ) \
	X( PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays ) \
	X( PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays ) \
	X( PFNGLBINDVERTEXARRAYPROC, glBindVertexArray ) \
	X( PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray ) \
	X( PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer ) \
	X( PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer ) \
	X( PFNGLDRAWARRAYSPROC, glDrawArrays ) \
	X( PFNGLDRAWELEMENTSPROC, glDrawElements ) \
	X( PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex ) \
	X( PFNGLCREATESHADERPROC, glCreateShader ) \
	X( PFNGLDELETESHADERPROC, glDeleteShader ) \
	X( PFNGLSHADERSOURCEPROC, glShaderSource ) \
	X( PFNGLCOMPILESHADERPROC, glCompileShader ) \
	X( PFNGLGETSHADERIVPROC, glGetShaderiv ) \
	X( PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog ) \
	X( PFNGLCREATEPROGRAMPROC, glCreateProgram ) \
	X( PFNGLDELETEPROGRAMPROC, glDeleteProgram ) \
	X( PFNGLATTACHSHADERPROC, glAttachShader ) \
	X( PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation ) \
	X( PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation ) \
	X( PFNGLLINKPROGRAMPROC, glLinkProgram ) \
	X( PFNGLGETPROGRAMIVPROC, glGetProgramiv ) \
	X( PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog ) \
	X( PFNGLUSEPROGRAMPROC, glUseProgram ) \
	X( PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation ) \
	X( PFNGLGETACTIVEUNIFORMPROC, glGetActiveUniform ) \
	X( PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex ) \
	X( PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding ) \
	X( PFNGLUNIFORM1IPROC, glUniform1i ) \
	X( PFNGLUNIFORM4FVPROC, glUniform4fv ) \
	X( PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers ) \
	X( PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers ) \
	X( PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer ) \
	X( PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D ) \
	X( PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer ) \
	X( PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus ) \
	X( PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer ) \
	X( PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers ) \
	X( PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers ) \
	X( PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer ) \
	X( PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample ) \

#define GL3_DECLARE( type, name ) extern type p##name;
GL3_FUNCTIONS( GL3_DECLARE )
#undef GL3_DECLARE

// the code calls them by their own names
#define glGetString                    pglGetString
#define glGetStringi                   pglGetStringi
#define glGetIntegerv                  pglGetIntegerv
#define glGetFloatv                    pglGetFloatv
#define glGetError                     pglGetError
#define glEnable                       pglEnable
#define glDisable                      pglDisable
#define glClear                        pglClear
#define glClearColor                   pglClearColor
#define glClearDepth                   pglClearDepth
#define glViewport                     pglViewport
#define glScissor                      pglScissor
#define glDepthRange                   pglDepthRange
#define glDepthFunc                    pglDepthFunc
#define glDepthMask                    pglDepthMask
#define glColorMask                    pglColorMask
#define glBlendFunc                    pglBlendFunc
#define glCullFace                     pglCullFace
#define glFrontFace                    pglFrontFace
#define glPolygonOffset                pglPolygonOffset
#define glPixelStorei                  pglPixelStorei
#define glReadPixels                   pglReadPixels
#define glFinish                       pglFinish
#define glGenTextures                  pglGenTextures
#define glDeleteTextures               pglDeleteTextures
#define glBindTexture                  pglBindTexture
#define glActiveTexture                pglActiveTexture
#define glTexImage2D                   pglTexImage2D
#define glTexSubImage2D                pglTexSubImage2D
#define glTexParameteri                pglTexParameteri
#define glTexParameterf                pglTexParameterf
#define glGenBuffers                   pglGenBuffers
#define glDeleteBuffers                pglDeleteBuffers
#define glBindBuffer                   pglBindBuffer
#define glBindBufferBase               pglBindBufferBase
#define glBufferData                   pglBufferData
#define glBufferSubData                pglBufferSubData
#define glGenVertexArrays              pglGenVertexArrays
#define glDeleteVertexArrays           pglDeleteVertexArrays
#define glBindVertexArray              pglBindVertexArray
#define glEnableVertexAttribArray      pglEnableVertexAttribArray
#define glVertexAttribPointer          pglVertexAttribPointer
#define glVertexAttribIPointer         pglVertexAttribIPointer
#define glDrawArrays                   pglDrawArrays
#define glDrawElements                 pglDrawElements
#define glDrawElementsBaseVertex       pglDrawElementsBaseVertex
#define glCreateShader                 pglCreateShader
#define glDeleteShader                 pglDeleteShader
#define glShaderSource                 pglShaderSource
#define glCompileShader                pglCompileShader
#define glGetShaderiv                  pglGetShaderiv
#define glGetShaderInfoLog             pglGetShaderInfoLog
#define glCreateProgram                pglCreateProgram
#define glDeleteProgram                pglDeleteProgram
#define glAttachShader                 pglAttachShader
#define glBindAttribLocation           pglBindAttribLocation
#define glBindFragDataLocation         pglBindFragDataLocation
#define glLinkProgram                  pglLinkProgram
#define glGetProgramiv                 pglGetProgramiv
#define glGetProgramInfoLog            pglGetProgramInfoLog
#define glUseProgram                   pglUseProgram
#define glGetUniformLocation           pglGetUniformLocation
#define glGetActiveUniform             pglGetActiveUniform
#define glGetUniformBlockIndex         pglGetUniformBlockIndex
#define glUniformBlockBinding          pglUniformBlockBinding
#define glUniform1i                    pglUniform1i
#define glUniform4fv                   pglUniform4fv
#define glGenFramebuffers              pglGenFramebuffers
#define glDeleteFramebuffers           pglDeleteFramebuffers
#define glBindFramebuffer              pglBindFramebuffer
#define glFramebufferTexture2D         pglFramebufferTexture2D
#define glFramebufferRenderbuffer      pglFramebufferRenderbuffer
#define glCheckFramebufferStatus       pglCheckFramebufferStatus
#define glBlitFramebuffer              pglBlitFramebuffer
#define glGenRenderbuffers             pglGenRenderbuffers
#define glDeleteRenderbuffers          pglDeleteRenderbuffers
#define glBindRenderbuffer             pglBindRenderbuffer
#define glRenderbufferStorageMultisample pglRenderbufferStorageMultisample

qboolean R_GL3LoadFunctions( void );

#endif // R_GL_H
