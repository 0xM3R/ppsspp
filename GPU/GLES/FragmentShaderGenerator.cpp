// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#if defined(ANDROID) || defined(BLACKBERRY)
#define GLSL_ES_1_0
#else
#define GLSL_1_3
#endif


#include "FragmentShaderGenerator.h"
#include "../ge_constants.h"
#include "../GPUState.h"
#include <cstdio>

// TODO: remove
static char buffer[16384];

// SDL 1.2 on Apple does not have support for OpenGL 3 and hence needs
// special treatment in the shader generator.
#ifdef __APPLE__
#define FORCE_OPENGL_2_0
#endif


#define WRITE p+=sprintf

// GL_NV_shader_framebuffer_fetch looks interesting....

// Here we must take all the bits of the gstate that determine what the fragment shader will
// look like, and concatenate them together into an ID.
void ComputeFragmentShaderID(FragmentShaderID *id)
{
	memset(&id->d[0], 0, sizeof(id->d));
	if (gstate.clearmode & 1) {
		// We only need one clear shader, so let's ignore the rest of the bits.
		id->d[0] = 1;
	} else {
		// id->d[0] |= (gstate.clearmode & 1);
		id->d[0] |= (gstate.texfunc & 0x7) << 1;
		id->d[0] |= ((gstate.texfunc & 0x100) >> 8) << 4; // rgb or rgba
		id->d[0] |= ((gstate.texfunc & 0x10000) >> 16) << 5;	// color double
		id->d[0] |= (gstate.lmode & 1) << 6;
		id->d[0] |= (gstate.textureMapEnable & 1) << 7;
		id->d[0] |= (gstate.alphaTestEnable & 1) << 8;
		id->d[0] |= (gstate.alphatest & 0x7) << 9;	 // alpha test func
		//id->d[0] |= (gstate.fogEnable & 1) << 9;
	}
}

// Missing: Alpha test, color test, Z depth range, fog
// Also, logic ops etc, of course. Urgh.
char *GenerateFragmentShader()
{
	char *p = buffer;
#if defined(GLSL_ES_1_0)
	WRITE(p, "precision mediump float;\n");
#elif !defined(FORCE_OPENGL_2_0)
	WRITE(p, "#version 130\n");
#endif

	int lmode = gstate.lmode & 1;

	if (gstate.textureMapEnable & 1)
		WRITE(p, "uniform sampler2D tex;\n");
	if (gstate.alphaTestEnable & 1)
		WRITE(p, "uniform vec4 u_alpharef;\n");
	WRITE(p, "uniform vec4 u_texenv;\n");
	WRITE(p, "varying vec4 v_color0;\n");
	if (lmode)
		WRITE(p, "varying vec4 v_color1;\n");
	WRITE(p, "varying vec2 v_texcoord;\n");

	WRITE(p, "void main() {");
	WRITE(p, "  vec4 v;\n");

	if (gstate.clearmode & 1)
	{
		WRITE(p, "v = v_color0;\n");
	}
	else
	{
		const char *secondary = "";
		// Secondary color for specular on top of texture
		if (lmode) {
			WRITE(p, "  vec4 s = vec4(v_color1.xyz, 0.0);");
			secondary = " + s";
		} else {
			WRITE(p, "	vec4 s = vec4(0.0, 0.0, 0.0, 0.0);\n");
			secondary = "";
		}

		if (gstate.textureMapEnable & 1) {
			WRITE(p, "	vec4 t = texture2D(tex, v_texcoord);\n");
			// WRITE(p, "	vec4 t = vec4(1,0,1,1);");
			WRITE(p, "	vec4 p = clamp(v_color0, 0.0, 1.0);\n");
		} else {
			// No texture mapping
			WRITE(p, "	vec4 t = vec4(1.0, 1.0, 1.0, 1.0);\n"); //, secondary);
			WRITE(p, "	vec4 p = clamp(v_color0, 0.0, 1.0);\n"); // , secondary);
		}

		if (gstate.texfunc & 0x100) { // texfmt == RGBA
			switch (gstate.texfunc & 0x7) {
			case GE_TEXFUNC_MODULATE:
				WRITE(p, "	v = t * p%s;\n", secondary); break;
			case GE_TEXFUNC_DECAL:
				WRITE(p, "	v = vec4(1.0 - t.a * p.rgb + t.a * u_texenv.rgb, p.a)%s;\n", secondary); break;
			case GE_TEXFUNC_BLEND:
				WRITE(p, "	v = vec4((1.0 - t.rgb) * p.rgb + t.rgb * u_texenv.rgb, p.a * t.a)%s;\n", secondary); break;
			case GE_TEXFUNC_REPLACE:
				WRITE(p, "	v = t%s;\n", secondary); break;
			case GE_TEXFUNC_ADD:
				WRITE(p, "	v = vec4(t.rgb + p.rgb, p.a * t.a)%s;\n", secondary); break;
			}
		} else {	// texfmt == RGB
			switch (gstate.texfunc & 0x7) {
			case GE_TEXFUNC_MODULATE:
				WRITE(p, "	v = vec4(t.rgb * p.rgb, p.a)%s;\n", secondary); break;
			case GE_TEXFUNC_DECAL:
				WRITE(p, "	v = vec4(t.rgb, p.a)%s;\n", secondary); break;
			case GE_TEXFUNC_BLEND:
				WRITE(p, "	v = vec4(1.0 - t.rgb) * p.rgb + t.rgb * u_texenv.rgb, p.a)%s;\n", secondary); break;
			case GE_TEXFUNC_REPLACE:
				WRITE(p, "	v = vec4(t.rgb, p.a)%s;\n", secondary); break;
			case GE_TEXFUNC_ADD:
				WRITE(p, "	v = vec4(t.rgb + p.rgb, p.a)%s;\n", secondary); break;
			}
		}

		// Color doubling
		if (gstate.texfunc & 0x10000) {
			WRITE(p, "  gl_FragColor = v * vec4(2.0, 2.0, 2.0, 1.0);");
		} else {
			WRITE(p, "  gl_FragColor = v;\n");
		}

/*
		if (gstate.alphaTestEnable & 1) {
			int alphaTestFunc = gstate.alphatest & 7;
			const char *alphaTestFuncs[] = { "#", "#", " == ", " != ", " < ", " <= ", " > ", " >= " };	// never/always don't make sense
			WRITE(p, "if (!(gl_FragColor.a %s u_alpharef.x)) discard;", alphaTestFuncs[alphaTestFunc]);
		}*/
		// Fogging should be added here - and disabled during clear mode

	}
	//WRITE(p, "	gl_FragColor = vec4(1,0,1,1);");
	WRITE(p, "}\n");

	return buffer;
}

