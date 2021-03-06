#pragma once
#ifndef _WIN32
#include <GL/glew.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#include "GL/gl.h"
#include <glext.h>
typedef BOOL (WINAPI* PFNWGLSWAPINTERVALEXTPROC) (int interval);

#define OPENGL_PROC(p, n) extern p gl##n
#define OPENGL_PROC2(p, n, tn) OPENGL_PROC(p, n)
	#include "GLProcTable.h"
#undef OPENGL_PROC
#undef OPENGL_PROC2

#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>

#else
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#endif

#ifndef GL_TEXTURE_BUFFER_BINDING
//During spec release, this enum was removed during upgrade from ARB equivalent
//See https://www.khronos.org/bugzilla/show_bug.cgi?id=844
#define GL_TEXTURE_BUFFER_BINDING 0x8C2A
#endif

namespace gl
{
	void init();
}

struct OpenGL
{
#define OPENGL_PROC2(p, n, tn) OPENGL_PROC(p, n)
#define OPENGL_PROC(p, n) p n
	#include "GLProcTable.h"
#undef OPENGL_PROC
#undef OPENGL_PROC2

	OpenGL();
	~OpenGL();

	void Init();
	void Close();
};
