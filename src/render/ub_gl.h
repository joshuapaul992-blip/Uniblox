/* Uniblox - minimal OpenGL loader.
 *
 * The .exe never links opengl32.dll: every function is looked up at run
 * time through plat_gl_proc(), so the game still starts on machines with
 * no OpenGL at all (and uses the software renderer there). Only what the
 * renderer uses is declared. Constants are standard GL values. */
#ifndef UB_GL_H
#define UB_GL_H

#include <stddef.h>
#include "core/ub_types.h"

#if defined(_WIN32)
#define UBGL_API __stdcall
#else
#define UBGL_API
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef char GLchar;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NONE 0
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_QUADS 0x0007
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_LIGHTING 0x0B50
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_COLOR_MATERIAL 0x0B57
#define GL_FOG 0x0B60
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_FOG_HINT 0x0C54
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_TEXTURE_2D 0x0DE1
#define GL_NICEST 0x1102
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_DEPTH_COMPONENT 0x1902
#define GL_ALPHA 0x1906
#define GL_LUMINANCE 0x1909
#define GL_SMOOTH 0x1D01
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_EXP 0x0800
#define GL_MODULATE 0x2100
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV 0x2300
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_LIGHT0 0x4000
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_RGBA8 0x8058
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_MAX_SAMPLES 0x8D57
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4

/* X(return type, name, parameter list, alternative name or NULL) */
#define UBGL_CORE(X) \
    X(const GLubyte *, glGetString, (GLenum), NULL) \
    X(void, glGetIntegerv, (GLenum, GLint *), NULL) \
    X(void, glGetFloatv, (GLenum, GLfloat *), NULL) \
    X(GLenum, glGetError, (void), NULL) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei), NULL) \
    X(void, glClear, (GLbitfield), NULL) \
    X(void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf), NULL) \
    X(void, glEnable, (GLenum), NULL) \
    X(void, glDisable, (GLenum), NULL) \
    X(void, glDepthFunc, (GLenum), NULL) \
    X(void, glDepthMask, (GLboolean), NULL) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean), NULL) \
    X(void, glCullFace, (GLenum), NULL) \
    X(void, glFrontFace, (GLenum), NULL) \
    X(void, glBlendFunc, (GLenum, GLenum), NULL) \
    X(void, glPolygonOffset, (GLfloat, GLfloat), NULL) \
    X(void, glHint, (GLenum, GLenum), NULL) \
    X(void, glShadeModel, (GLenum), NULL) \
    X(void, glPixelStorei, (GLenum, GLint), NULL) \
    X(void, glGenTextures, (GLsizei, GLuint *), NULL) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *), NULL) \
    X(void, glBindTexture, (GLenum, GLuint), NULL) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *), NULL) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint), NULL) \
    X(void, glTexParameterf, (GLenum, GLenum, GLfloat), NULL) \
    X(void, glTexEnvi, (GLenum, GLenum, GLint), NULL) \
    X(void, glMatrixMode, (GLenum), NULL) \
    X(void, glLoadIdentity, (void), NULL) \
    X(void, glLoadMatrixf, (const GLfloat *), NULL) \
    X(void, glEnableClientState, (GLenum), NULL) \
    X(void, glDisableClientState, (GLenum), NULL) \
    X(void, glVertexPointer, (GLint, GLenum, GLsizei, const void *), NULL) \
    X(void, glNormalPointer, (GLenum, GLsizei, const void *), NULL) \
    X(void, glColorPointer, (GLint, GLenum, GLsizei, const void *), NULL) \
    X(void, glTexCoordPointer, (GLint, GLenum, GLsizei, const void *), NULL) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei), NULL) \
    X(void, glBegin, (GLenum), NULL) \
    X(void, glEnd, (void), NULL) \
    X(void, glVertex2f, (GLfloat, GLfloat), NULL) \
    X(void, glColor4f, (GLfloat, GLfloat, GLfloat, GLfloat), NULL) \
    X(void, glTexCoord2f, (GLfloat, GLfloat), NULL) \
    X(void, glLightfv, (GLenum, GLenum, const GLfloat *), NULL) \
    X(void, glLightModelfv, (GLenum, const GLfloat *), NULL) \
    X(void, glMaterialfv, (GLenum, GLenum, const GLfloat *), NULL) \
    X(void, glMaterialf, (GLenum, GLenum, GLfloat), NULL) \
    X(void, glColorMaterial, (GLenum, GLenum), NULL) \
    X(void, glFogi, (GLenum, GLint), NULL) \
    X(void, glFogf, (GLenum, GLfloat), NULL) \
    X(void, glFogfv, (GLenum, const GLfloat *), NULL) \
    X(void, glDrawBuffer, (GLenum), NULL) \
    X(void, glReadBuffer, (GLenum), NULL)

/* OpenGL 2.0 shaders (and 1.3 multitexture) */
#define UBGL_SHADER(X) \
    X(void, glActiveTexture, (GLenum), "glActiveTextureARB") \
    X(GLuint, glCreateShader, (GLenum), NULL) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *), NULL) \
    X(void, glCompileShader, (GLuint), NULL) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *), NULL) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *), NULL) \
    X(void, glDeleteShader, (GLuint), NULL) \
    X(GLuint, glCreateProgram, (void), NULL) \
    X(void, glAttachShader, (GLuint, GLuint), NULL) \
    X(void, glLinkProgram, (GLuint), NULL) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *), NULL) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *), NULL) \
    X(void, glDeleteProgram, (GLuint), NULL) \
    X(void, glUseProgram, (GLuint), NULL) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *), NULL) \
    X(void, glUniform1i, (GLint, GLint), NULL) \
    X(void, glUniform1f, (GLint, GLfloat), NULL) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat), NULL) \
    X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat), NULL) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *), NULL)

/* framebuffer objects: OpenGL 3.0 / ARB_framebuffer_object, or the EXT
 * extension on older drivers (same constants) */
#define UBGL_FBO(X) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *), "glGenFramebuffersEXT") \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *), "glDeleteFramebuffersEXT") \
    X(void, glBindFramebuffer, (GLenum, GLuint), "glBindFramebufferEXT") \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint), "glFramebufferTexture2DEXT") \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint), "glFramebufferRenderbufferEXT") \
    X(GLenum, glCheckFramebufferStatus, (GLenum), "glCheckFramebufferStatusEXT") \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *), "glGenRenderbuffersEXT") \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *), "glDeleteRenderbuffersEXT") \
    X(void, glBindRenderbuffer, (GLenum, GLuint), "glBindRenderbufferEXT")

/* vertex buffer objects: OpenGL 1.5 or ARB_vertex_buffer_object */
#define UBGL_VBO(X) \
    X(void, glGenBuffers, (GLsizei, GLuint *), "glGenBuffersARB") \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *), "glDeleteBuffersARB") \
    X(void, glBindBuffer, (GLenum, GLuint), "glBindBufferARB") \
    X(void, glBufferData, (GLenum, ptrdiff_t, const void *, GLenum), "glBufferDataARB")

#define UBGL_MSAA(X) \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei), "glRenderbufferStorageMultisampleEXT") \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum), "glBlitFramebufferEXT")

typedef struct {
#define UBGL_FIELD(ret, name, args, alt) ret (UBGL_API *name) args;
    UBGL_CORE(UBGL_FIELD)
    UBGL_SHADER(UBGL_FIELD)
    UBGL_FBO(UBGL_FIELD)
    UBGL_MSAA(UBGL_FIELD)
    UBGL_VBO(UBGL_FIELD)
#undef UBGL_FIELD
    bool has_core, has_shaders, has_fbo, has_msaa, has_vbo;
} ub_gl_api;

extern ub_gl_api ubgl;

/* Look everything up; returns false if even OpenGL 1.1 is incomplete. */
bool ub_gl_load(void);

#endif
