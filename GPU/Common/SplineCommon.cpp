// Copyright (c) 2013- PPSSPP Project.

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

#include <string.h>
#include <algorithm>

#include "profiler/profiler.h"

#include "Common/CPUDetect.h"
#include "Common/MemoryUtil.h"
#include "Core/Config.h"

#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"  // only needed for UVScale stuff

#if defined(_M_SSE)
#include <emmintrin.h>

inline __m128 SSECrossProduct(__m128 a, __m128 b)
{
	const __m128 left = _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2)));
	const __m128 right = _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 1, 0, 2)), _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1)));
	return _mm_sub_ps(left, right);
}

inline __m128 SSENormalizeMultiplierSSE2(__m128 v)
{
	const __m128 sq = _mm_mul_ps(v, v);
	const __m128 r2 = _mm_shuffle_ps(sq, sq, _MM_SHUFFLE(0, 0, 0, 1));
	const __m128 r3 = _mm_shuffle_ps(sq, sq, _MM_SHUFFLE(0, 0, 0, 2));
	const __m128 res = _mm_add_ss(r3, _mm_add_ss(r2, sq));

	const __m128 rt = _mm_rsqrt_ss(res);
	return _mm_shuffle_ps(rt, rt, _MM_SHUFFLE(0, 0, 0, 0));
}

#if _M_SSE >= 0x401
#include <smmintrin.h>

inline __m128 SSENormalizeMultiplierSSE4(__m128 v)
{
	return _mm_rsqrt_ps(_mm_dp_ps(v, v, 0xFF));
}

inline __m128 SSENormalizeMultiplier(bool useSSE4, __m128 v)
{
	if (useSSE4)
		return SSENormalizeMultiplierSSE4(v);
	return SSENormalizeMultiplierSSE2(v);
}
#else
inline __m128 SSENormalizeMultiplier(bool useSSE4, __m128 v)
{
	return SSENormalizeMultiplierSSE2(v);
}
#endif

#endif


#define START_OPEN 1
#define END_OPEN 2



static void CopyQuad(u8 *&dest, const SimpleVertex *v1, const SimpleVertex *v2, const SimpleVertex *v3, const SimpleVertex *v4) {
	int vertexSize = sizeof(SimpleVertex);
	memcpy(dest, v1, vertexSize);
	dest += vertexSize;
	memcpy(dest, v2, vertexSize);
	dest += vertexSize;
	memcpy(dest, v3, vertexSize);
	dest += vertexSize;
	memcpy(dest, v4, vertexSize);
	dest += vertexSize;
}

static void CopyQuadIndex(u16 *&indices, GEPatchPrimType type, const int idx0, const int idx1, const int idx2, const int idx3) {
	if (type == GE_PATCHPRIM_LINES) {
		*(indices++) = idx0;
		*(indices++) = idx2;
		*(indices++) = idx1;
		*(indices++) = idx3;
		*(indices++) = idx1;
		*(indices++) = idx2;
	}
	else {
		*(indices++) = idx0;
		*(indices++) = idx2;
		*(indices++) = idx1;
		*(indices++) = idx1;
		*(indices++) = idx2;
		*(indices++) = idx3;
	}
}

#undef b2

// Bernstein basis functions
inline float bern0(float x) { return (1 - x) * (1 - x) * (1 - x); }
inline float bern1(float x) { return 3 * x * (1 - x) * (1 - x); }
inline float bern2(float x) { return 3 * x * x * (1 - x); }
inline float bern3(float x) { return x * x * x; }

inline float bern0deriv(float x) { return -3 * (x - 1) * (x - 1); }
inline float bern1deriv(float x) { return 9 * x * x - 12 * x + 3; }
inline float bern2deriv(float x) { return 3 * (2 - 3 * x) * x; }
inline float bern3deriv(float x) { return 3 * x * x; }

// http://en.wikipedia.org/wiki/Bernstein_polynomial
static Math3D::Vec2f Bernstein3D(const Math3D::Vec2f& p0, const Math3D::Vec2f& p1, const Math3D::Vec2f& p2, const Math3D::Vec2f& p3, float x) {
	if (x == 0) return p0;
	else if (x == 1) return p3;
	return p0 * bern0(x) + p1 * bern1(x) + p2 * bern2(x) + p3 * bern3(x);
}

static Vec3f Bernstein3D(const Vec3f& p0, const Vec3f& p1, const Vec3f& p2, const Vec3f& p3, float x) {
	if (x == 0) return p0;
	else if (x == 1) return p3;
	return p0 * bern0(x) + p1 * bern1(x) + p2 * bern2(x) + p3 * bern3(x);
}

static Vec4f Bernstein3D(const Vec4f& p0, const Vec4f& p1, const Vec4f& p2, const Vec4f& p3, float x) {
	if (x == 0) return p0;
	else if (x == 1) return p3;
	return p0 * bern0(x) + p1 * bern1(x) + p2 * bern2(x) + p3 * bern3(x);
}

static Vec4f Bernstein3D(const u32& p0, const u32& p1, const u32& p2, const u32& p3, float x) {
	return Bernstein3D(Vec4f::FromRGBA(p0), Vec4f::FromRGBA(p1), Vec4f::FromRGBA(p2), Vec4f::FromRGBA(p3), x);
}

static Vec3f Bernstein3DDerivative(const Vec3f& p0, const Vec3f& p1, const Vec3f& p2, const Vec3f& p3, float x) {
	return p0 * bern0deriv(x) + p1 * bern1deriv(x) + p2 * bern2deriv(x) + p3 * bern3deriv(x);
}

struct KnotDiv {
	float _3_0 = 1.0f / 3.0f;
	float _4_1 = 1.0f / 3.0f;
	float _5_2 = 1.0f / 3.0f;
	float _3_1 = 1.0f / 2.0f;
	float _4_2 = 1.0f / 2.0f;
	float _3_2 = 1.0f; // Always 1
};

static void spline_n_4(int i, float t, float *knot, const KnotDiv &div, float *splineVal, float *derivs) {
	knot += i;

#ifdef _M_SSE
	const __m128 knot012 = _mm_loadu_ps(knot);
	const __m128 t012 = _mm_sub_ps(_mm_set_ps1(t), knot012);
	const __m128 f30_41_52 = _mm_mul_ps(t012, _mm_loadu_ps(&div._3_0));
	const __m128 f52_31_42 = _mm_mul_ps(t012, _mm_loadu_ps(&div._5_2));
	const float &f32 = t012.m128_f32[2];

	// Following comments are for explains order of the multiply.
//	float a = (1-f30)*(1-f31);
//	float c = (1-f41)*(1-f42);
//	float b = (  f31 *   f41);
//	float d = (  f42 *   f52);
	const __m128 f30_41_31_42 = _mm_shuffle_ps(f30_41_52, f52_31_42, _MM_SHUFFLE(2, 1, 1, 0));
	const __m128 f31_42_41_52 = _mm_shuffle_ps(f52_31_42, f30_41_52, _MM_SHUFFLE(2, 1, 2, 1));
	const __m128 c1_1_0_0 = { 1, 1, 0, 0 };
	const __m128 acbd = _mm_mul_ps(_mm_sub_ps(c1_1_0_0, f30_41_31_42), _mm_sub_ps(c1_1_0_0, f31_42_41_52));
	const float &a = acbd.m128_f32[0];
	const float &b = acbd.m128_f32[2];
	const float &c = acbd.m128_f32[1];
	const float &d = acbd.m128_f32[3];

	// For derivative
	const float &f31 = f30_41_31_42.m128_f32[2];
	const float &f42 = f30_41_31_42.m128_f32[3];
#else
	// TODO: Maybe compilers could be coaxed into vectorizing this code without the above explicitly...
	float t0 = (t - knot[0]);
	float t1 = (t - knot[1]);
	float t2 = (t - knot[2]);

	float f30 = t0 * div._3_0;
	float f41 = t1 * div._4_1;
	float f52 = t2 * div._5_2;
	float f31 = t1 * div._3_1;
	float f42 = t2 * div._4_2;
	float f32 = t2 * div._3_2;

	float a = (1-f30)*(1-f31);
	float b = (f31*f41);
	float c = (1-f41)*(1-f42);
	float d = (f42*f52);
#endif

	splineVal[0] = a-(a*f32);
	splineVal[1] = 1-a-b+((a+b+c-1)*f32);
	splineVal[2] = b+((1-b-c-d)*f32);
	splineVal[3] = d*f32;

	// Derivative
	float i1 = (1 - f31) * (1 - f32);
	float i2 = f31 * (1 - f32) + (1 - f42) * f32;
	float i3 = f42 * f32;

	float f130 = i1 * div._3_0;
	float f241 = i2 * div._4_1;
	float f352 = i3 * div._5_2;

	derivs[0] = 3 * (0 - f130);
	derivs[1] = 3 * (f130 - f241);
	derivs[2] = 3 * (f241 - f352);
	derivs[3] = 3 * (f352 - 0);
}

// knot should be an array sized n + 5  (n + 1 + 1 + degree (cubic))
static void spline_knot(int n, int type, float *knots, KnotDiv *divs) {
		// Basic theory (-2 to +3), optimized with KnotDiv (-2 to +0) 
	//	for (int i = 0; i < n + 5; ++i) {
		for (int i = 0; i < n + 2; ++i) {
			knots[i] = (float)i - 2;
		}

		if ((type & 1) != 0) {
			knots[0] = 0;
			knots[1] = 0;

			divs[0]._3_0 = 1.0f;
			divs[0]._4_1 = 1.0f / 2.0f;
			divs[0]._3_1 = 1.0f;
			if (n > 1)
				divs[1]._3_0 = 1.0f / 2.0f;
		}
		if ((type & 2) != 0) {
		//	knots[n + 2] = (float)n; // Got rid of this line optimized with KnotDiv
		//	knots[n + 3] = (float)n; // Got rid of this line optimized with KnotDiv
		//	knots[n + 4] = (float)n; // Got rid of this line optimized with KnotDiv
			divs[n - 1]._4_1 = 1.0f / 2.0f;
			divs[n - 1]._5_2 = 1.0f;
			divs[n - 1]._4_2 = 1.0f;
			if (n > 1)
				divs[n - 2]._5_2 = 1.0f / 2.0f;
		}
}

bool CanUseHardwareTessellation(GEPatchPrimType prim) {
	if (g_Config.bHardwareTessellation && !g_Config.bSoftwareRendering) {
		return CanUseHardwareTransform(PatchPrimToPrim(prim));
	}
	return false;
}

// Prepare mesh of one patch for "Instanced Tessellation".
static void TessellateSplinePatchHardware(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch) {
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float inv_u = 1.0f / (float)spatch.tess_u;
	float inv_v = 1.0f / (float)spatch.tess_v;

	// Generating simple input vertices for the spline-computing vertex shader.
	for (int tile_v = 0; tile_v < spatch.tess_v + 1; ++tile_v) {
		for (int tile_u = 0; tile_u < spatch.tess_u + 1; ++tile_u) {
			SimpleVertex &vert = vertices[tile_v * (spatch.tess_u + 1) + tile_u];
			vert.pos.x = (float)tile_u * inv_u;
			vert.pos.y = (float)tile_v * inv_v;

			// TODO: Move to shader uniform and unify this method spline and bezier if necessary.
			// For compute normal
			vert.nrm.x = inv_u;
			vert.nrm.y = inv_v;
		}
	}

	// Combine the vertices into triangles.
	for (int tile_v = 0; tile_v < spatch.tess_v; ++tile_v) {
		for (int tile_u = 0; tile_u < spatch.tess_u; ++tile_u) {
			int idx0 = tile_v * (spatch.tess_u + 1) + tile_u;
			int idx1 = tile_v * (spatch.tess_u + 1) + tile_u + 1;
			int idx2 = (tile_v + 1) * (spatch.tess_u + 1) + tile_u;
			int idx3 = (tile_v + 1) * (spatch.tess_u + 1) + tile_u + 1;

			CopyQuadIndex(indices, spatch.primType, idx0, idx1, idx2, idx3);
			count += 6;
		}
	}
}

static void _SplinePatchLowQuality(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType) {
	// Fast and easy way - just draw the control points, generate some very basic normal vector substitutes.
	// Very inaccurate but okay for Loco Roco. Maybe should keep it as an option because it's fast.

	const int tile_min_u = (spatch.type_u & START_OPEN) ? 0 : 1;
	const int tile_min_v = (spatch.type_v & START_OPEN) ? 0 : 1;
	const int tile_max_u = (spatch.type_u & END_OPEN) ? spatch.count_u - 1 : spatch.count_u - 2;
	const int tile_max_v = (spatch.type_v & END_OPEN) ? spatch.count_v - 1 : spatch.count_v - 2;

	float tu_width = (float)spatch.count_u - 3.0f;
	float tv_height = (float)spatch.count_v - 3.0f;
	tu_width /= (float)(tile_max_u - tile_min_u);
	tv_height /= (float)(tile_max_v - tile_min_v);

	GEPatchPrimType prim_type = spatch.primType;
	bool computeNormals = spatch.computeNormals;
	bool patchFacing = spatch.patchFacing;

	int i = 0;
	for (int tile_v = tile_min_v; tile_v < tile_max_v; ++tile_v) {
		for (int tile_u = tile_min_u; tile_u < tile_max_u; ++tile_u) {
			int point_index = tile_u + tile_v * spatch.count_u;

			SimpleVertex v0 = *spatch.points[point_index];
			SimpleVertex v1 = *spatch.points[point_index + 1];
			SimpleVertex v2 = *spatch.points[point_index + spatch.count_u];
			SimpleVertex v3 = *spatch.points[point_index + spatch.count_u + 1];

			// Generate UV. TODO: Do this even if UV specified in control points?
			if ((origVertType & GE_VTYPE_TC_MASK) == 0) {
				float u = (tile_u - tile_min_u) * tu_width;
				float v = (tile_v - tile_min_v) * tv_height;

				v0.uv[0] = u;
				v0.uv[1] = v;
				v1.uv[0] = u + tu_width;
				v1.uv[1] = v;
				v2.uv[0] = u;
				v2.uv[1] = v + tv_height;
				v3.uv[0] = u + tu_width;
				v3.uv[1] = v + tv_height;
			}

			// Generate normal if lighting is enabled (otherwise there's no point).
			// This is a really poor quality algorithm, we get facet normals.
			if (computeNormals) {
				Vec3Packedf norm = Cross(v1.pos - v0.pos, v2.pos - v0.pos);
				norm.Normalize();
				if (patchFacing)
					norm *= -1.0f;
				v0.nrm = norm;
				v1.nrm = norm;
				v2.nrm = norm;
				v3.nrm = norm;
			}

			int idx0 = i * 4 + 0;
			int idx1 = i * 4 + 1;
			int idx2 = i * 4 + 2;
			int idx3 = i * 4 + 3;
			i++;

			CopyQuad(dest, &v0, &v1, &v2, &v3);
			CopyQuadIndex(indices, prim_type, idx0, idx1, idx2, idx3);
			count += 6;
		}
	}

}

static inline void AccumulateWeighted(Vec3f &out, const Vec3Packedf &in, const Vec4f &w) {
#ifdef _M_SSE
	out.vec = _mm_add_ps(out.vec, _mm_mul_ps(_mm_loadu_ps(in.AsArray()), w.vec));
#else
	out += in * w.x;
#endif
}

static inline void AccumulateWeighted(Vec4f &out, const Vec4f &in, const Vec4f &w) {
#ifdef _M_SSE
	out.vec = _mm_add_ps(out.vec, _mm_mul_ps(in.vec, w.vec));
#else
	out += in * w;
#endif
}

template <bool origNrm, bool origCol, bool origTc, bool useSSE4>
static void SplinePatchFullQuality(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	// Full (mostly) correct tessellation of spline patches.
	// Not very fast.

	float *knot_u = new float[spatch.count_u + 4];
	float *knot_v = new float[spatch.count_v + 4];
	KnotDiv *divs_u = new KnotDiv[spatch.count_u - 3];
	KnotDiv *divs_v = new KnotDiv[spatch.count_v - 3];
	spline_knot(spatch.count_u - 3, spatch.type_u, knot_u, divs_u);
	spline_knot(spatch.count_v - 3, spatch.type_v, knot_v, divs_v);

	// Increase tessellation based on the size. Should be approximately right?
	int patch_div_s = (spatch.count_u - 3) * spatch.tess_u;
	int patch_div_t = (spatch.count_v - 3) * spatch.tess_v;
	if (quality > 1) {
		// Don't cut below 2, though.
		if (patch_div_s > 2) {
			patch_div_s /= quality;
		}
		if (patch_div_t > 2) {
			patch_div_t /= quality;
		}
	}

	// Downsample until it fits, in case crazy tessellation factors are sent.
	while ((patch_div_s + 1) * (patch_div_t + 1) > maxVertices) {
		patch_div_s /= 2;
		patch_div_t /= 2;
	}

	if (patch_div_s < 1) patch_div_s = 1;
	if (patch_div_t < 1) patch_div_t = 1;

	// First compute all the vertices and put them in an array
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float tu_width = (float)spatch.count_u - 3.0f;
	float tv_height = (float)spatch.count_v - 3.0f;

	// int max_idx = spatch.count_u * spatch.count_v;

	bool computeNormals = spatch.computeNormals;

	float one_over_patch_div_s = 1.0f / (float)(patch_div_s);
	float one_over_patch_div_t = 1.0f / (float)(patch_div_t);

	for (int tile_v = 0; tile_v < patch_div_t + 1; tile_v++) {
		float v = (float)tile_v * (float)(spatch.count_v - 3) * one_over_patch_div_t;
		if (v < 0.0f)
			v = 0.0f;
		for (int tile_u = 0; tile_u < patch_div_s + 1; tile_u++) {
			float u = (float)tile_u * (float)(spatch.count_u - 3) * one_over_patch_div_s;
			if (u < 0.0f)
				u = 0.0f;
			SimpleVertex *vert = &vertices[tile_v * (patch_div_s + 1) + tile_u];
			Vec4f vert_color(0, 0, 0, 0);
			Vec3f vert_pos;
			vert_pos.SetZero();
			Vec3f vert_nrm;
			Vec3f du, dv;
			du.SetZero();
			dv.SetZero();
			if (origNrm) {
				vert_nrm.SetZero();
			}
			if (origCol) {
				vert_color.SetZero();
			} else {
				memcpy(vert->color, spatch.points[0]->color, 4);
			}
			if (origTc) {
				vert->uv[0] = 0.0f;
				vert->uv[1] = 0.0f;
			} else {
				vert->uv[0] = tu_width * ((float)tile_u * one_over_patch_div_s);
				vert->uv[1] = tv_height * ((float)tile_v * one_over_patch_div_t);
			}


			// Collect influences from surrounding control points.
			float u_weights[4];
			float v_weights[4];
			float u_derivs[4];
			float v_derivs[4];

			int iu = (int)u;
			int iv = (int)v;

			// TODO: Would really like to fix the surrounding logic somehow to get rid of these but I can't quite get it right..
			// Without the previous epsilons and with large count_u, we will end up doing an out of bounds access later without these.
			if (iu >= spatch.count_u - 3) iu = spatch.count_u - 4;
			if (iv >= spatch.count_v - 3) iv = spatch.count_v - 4;

			spline_n_4(iu, u, knot_u, divs_u[iu], u_weights, u_derivs);
			spline_n_4(iv, v, knot_v, divs_v[iv], v_weights, v_derivs);

			// Handle degenerate patches. without this, spatch.points[] may read outside the number of initialized points.
			int patch_w = std::min(spatch.count_u - iu, 4);
			int patch_h = std::min(spatch.count_v - iv, 4);

			for (int ii = 0; ii < patch_w; ++ii) {
				for (int jj = 0; jj < patch_h; ++jj) {
					float u_spline = u_weights[ii];
					float v_spline = v_weights[jj];
					float f = u_spline * v_spline;

					if (f > 0.0f) {
#ifdef _M_SSE
						Vec4f fv(_mm_set_ps1(f));
#else
						Vec4f fv = Vec4f::AssignToAll(f);
#endif
						int idx = spatch.count_u * (iv + jj) + (iu + ii);
						/*
						if (idx >= max_idx) {
							char temp[512];
							snprintf(temp, sizeof(temp), "count_u: %d count_v: %d patch_w: %d patch_h: %d  ii: %d  jj: %d  iu: %d  iv: %d  patch_div_s: %d  patch_div_t: %d\n", spatch.count_u, spatch.count_v, patch_w, patch_h, ii, jj, iu, iv, patch_div_s, patch_div_t);
							OutputDebugStringA(temp);
							Crash();
						}*/
						const SimpleVertex *a = spatch.points[idx];
						AccumulateWeighted(vert_pos, a->pos, fv);
						if (origTc) {
							vert->uv[0] += a->uv[0] * f;
							vert->uv[1] += a->uv[1] * f;
						}
						if (origCol) {
							Vec4f a_color = Vec4f::FromRGBA(a->color_32);
							AccumulateWeighted(vert_color, a_color, fv);
						}
						if (origNrm) {
							AccumulateWeighted(du, a->pos, Vec4f::AssignToAll(u_derivs[ii] * v_weights[jj]));
							AccumulateWeighted(dv, a->pos, Vec4f::AssignToAll(u_weights[ii] * v_derivs[jj]));
						}
					}
				}
			}
			vert->pos = vert_pos;
			if (origNrm) {
				vert_nrm = Cross(du, dv);
#ifdef _M_SSE
				const __m128 normalize = SSENormalizeMultiplier(useSSE4, vert_nrm.vec);
				vert_nrm.vec = _mm_mul_ps(vert_nrm.vec, normalize);
#else
				vert_nrm.Normalize();
#endif
				vert->nrm = vert_nrm;
			} else {
				vert->nrm.SetZero();
				vert->nrm.z = 1.0f;
			}
			if (origCol) {
				vert->color_32 = vert_color.ToRGBA();
			}
		}
	}

	delete[] knot_u;
	delete[] knot_v;
	delete[] divs_u;
	delete[] divs_v;

	GEPatchPrimType prim_type = spatch.primType;
	// Tessellate.
	for (int tile_v = 0; tile_v < patch_div_t; ++tile_v) {
		for (int tile_u = 0; tile_u < patch_div_s; ++tile_u) {
			int idx0 = tile_v * (patch_div_s + 1) + tile_u;
			int idx1 = tile_v * (patch_div_s + 1) + tile_u + 1;
			int idx2 = (tile_v + 1) * (patch_div_s + 1) + tile_u;
			int idx3 = (tile_v + 1) * (patch_div_s + 1) + tile_u + 1;

			CopyQuadIndex(indices, prim_type, idx0, idx1, idx2, idx3);
			count += 6;
		}
	}
}

template <bool origNrm, bool origCol, bool origTc>
static inline void SplinePatchFullQualityDispatch4(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	if (cpu_info.bSSE4_1)
		SplinePatchFullQuality<origNrm, origCol, origTc, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQuality<origNrm, origCol, origTc, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

template <bool origNrm, bool origCol>
static inline void SplinePatchFullQualityDispatch3(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origTc = (origVertType & GE_VTYPE_TC_MASK) != 0;

	if (origTc)
		SplinePatchFullQualityDispatch4<origNrm, origCol, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch4<origNrm, origCol, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

template <bool origNrm>
static inline void SplinePatchFullQualityDispatch2(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origCol = (origVertType & GE_VTYPE_COL_MASK) != 0;

	if (origCol)
		SplinePatchFullQualityDispatch3<origNrm, true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch3<origNrm, false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

static void SplinePatchFullQualityDispatch(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int quality, int maxVertices) {
	bool origNrm = (origVertType & GE_VTYPE_NRM_MASK) != 0;

	if (origNrm)
		SplinePatchFullQualityDispatch2<true>(dest, indices, count, spatch, origVertType, quality, maxVertices);
	else
		SplinePatchFullQualityDispatch2<false>(dest, indices, count, spatch, origVertType, quality, maxVertices);
}

void TessellateSplinePatch(u8 *&dest, u16 *indices, int &count, const SplinePatchLocal &spatch, u32 origVertType, int maxVertexCount) {
	switch (g_Config.iSplineBezierQuality) {
	case LOW_QUALITY:
		_SplinePatchLowQuality(dest, indices, count, spatch, origVertType);
		break;
	case MEDIUM_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 2, maxVertexCount);
		break;
	case HIGH_QUALITY:
		SplinePatchFullQualityDispatch(dest, indices, count, spatch, origVertType, 1, maxVertexCount);
		break;
	}
}

static void _BezierPatchLowQuality(u8 *&dest, u16 *&indices, int &count, int tess_u, int tess_v, const BezierPatch &patch, u32 origVertType) {
	const float third = 1.0f / 3.0f;
	// Fast and easy way - just draw the control points, generate some very basic normal vector subsitutes.
	// Very inaccurate though but okay for Loco Roco. Maybe should keep it as an option.

	float u_base = patch.u_index / 3.0f;
	float v_base = patch.v_index / 3.0f;

	GEPatchPrimType prim_type = patch.primType;

	for (int tile_v = 0; tile_v < 3; tile_v++) {
		for (int tile_u = 0; tile_u < 3; tile_u++) {
			int point_index = tile_u + tile_v * 4;

			SimpleVertex v0 = *patch.points[point_index];
			SimpleVertex v1 = *patch.points[point_index + 1];
			SimpleVertex v2 = *patch.points[point_index + 4];
			SimpleVertex v3 = *patch.points[point_index + 5];

			// Generate UV. TODO: Do this even if UV specified in control points?
			if ((origVertType & GE_VTYPE_TC_MASK) == 0) {
				float u = u_base + tile_u * third;
				float v = v_base + tile_v * third;
				v0.uv[0] = u;
				v0.uv[1] = v;
				v1.uv[0] = u + third;
				v1.uv[1] = v;
				v2.uv[0] = u;
				v2.uv[1] = v + third;
				v3.uv[0] = u + third;
				v3.uv[1] = v + third;
			}

			// Generate normal if lighting is enabled (otherwise there's no point).
			// This is a really poor quality algorithm, we get facet normals.
			if (patch.computeNormals) {
				Vec3Packedf norm = Cross(v1.pos - v0.pos, v2.pos - v0.pos);
				norm.Normalize();
				if (patch.patchFacing)
					norm *= -1.0f;
				v0.nrm = norm;
				v1.nrm = norm;
				v2.nrm = norm;
				v3.nrm = norm;
			}

			int total = patch.index * 3 * 3 * 4; // A patch has 3x3 tiles, and each tiles have 4 vertices.
			int tile_index = tile_u + tile_v * 3;
			int idx0 = total + tile_index * 4 + 0;
			int idx1 = total + tile_index * 4 + 1;
			int idx2 = total + tile_index * 4 + 2;
			int idx3 = total + tile_index * 4 + 3;

			CopyQuad(dest, &v0, &v1, &v2, &v3);
			CopyQuadIndex(indices, prim_type, idx0, idx1, idx2, idx3);
			count += 6;
		}
	}
}

template <typename T>
struct PrecomputedCurves {
	PrecomputedCurves(int count) {
		horiz1 = (T *)AllocateAlignedMemory(count * 4 * sizeof(T), 16);
		horiz2 = horiz1 + count * 1;
		horiz3 = horiz1 + count * 2;
		horiz4 = horiz1 + count * 3;
	}
	~PrecomputedCurves() {
		FreeAlignedMemory(horiz1);
	}

	T Bernstein3D(int u, float bv) {
		return ::Bernstein3D(horiz1[u], horiz2[u], horiz3[u], horiz4[u], bv);
	}

	T Bernstein3DDerivative(int u, float bv) {
		return ::Bernstein3DDerivative(horiz1[u], horiz2[u], horiz3[u], horiz4[u], bv);
	}

	T *horiz1;
	T *horiz2;
	T *horiz3;
	T *horiz4;
};

static void _BezierPatchHighQuality(u8 *&dest, u16 *&indices, int &count, int tess_u, int tess_v, const BezierPatch &patch, u32 origVertType) {
	const float third = 1.0f / 3.0f;

	// First compute all the vertices and put them in an array
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	PrecomputedCurves<Vec3f> prepos(tess_u + 1);
	PrecomputedCurves<Vec4f> precol(tess_u + 1);
	PrecomputedCurves<Math3D::Vec2f> pretex(tess_u + 1);
	PrecomputedCurves<Vec3f> prederivU(tess_u + 1);

	const bool computeNormals = patch.computeNormals;
	const bool sampleColors = (origVertType & GE_VTYPE_COL_MASK) != 0;
	const bool sampleTexcoords = (origVertType & GE_VTYPE_TC_MASK) != 0;

	// Precompute the horizontal curves to we only have to evaluate the vertical ones.
	for (int i = 0; i < tess_u + 1; i++) {
		float u = ((float)i / (float)tess_u);
		prepos.horiz1[i] = Bernstein3D(patch.points[0]->pos, patch.points[1]->pos, patch.points[2]->pos, patch.points[3]->pos, u);
		prepos.horiz2[i] = Bernstein3D(patch.points[4]->pos, patch.points[5]->pos, patch.points[6]->pos, patch.points[7]->pos, u);
		prepos.horiz3[i] = Bernstein3D(patch.points[8]->pos, patch.points[9]->pos, patch.points[10]->pos, patch.points[11]->pos, u);
		prepos.horiz4[i] = Bernstein3D(patch.points[12]->pos, patch.points[13]->pos, patch.points[14]->pos, patch.points[15]->pos, u);

		if (sampleColors) {
			precol.horiz1[i] = Bernstein3D(patch.points[0]->color_32, patch.points[1]->color_32, patch.points[2]->color_32, patch.points[3]->color_32, u);
			precol.horiz2[i] = Bernstein3D(patch.points[4]->color_32, patch.points[5]->color_32, patch.points[6]->color_32, patch.points[7]->color_32, u);
			precol.horiz3[i] = Bernstein3D(patch.points[8]->color_32, patch.points[9]->color_32, patch.points[10]->color_32, patch.points[11]->color_32, u);
			precol.horiz4[i] = Bernstein3D(patch.points[12]->color_32, patch.points[13]->color_32, patch.points[14]->color_32, patch.points[15]->color_32, u);
		}
		if (sampleTexcoords) {
			pretex.horiz1[i] = Bernstein3D(Math3D::Vec2f(patch.points[0]->uv), Math3D::Vec2f(patch.points[1]->uv), Math3D::Vec2f(patch.points[2]->uv), Math3D::Vec2f(patch.points[3]->uv), u);
			pretex.horiz2[i] = Bernstein3D(Math3D::Vec2f(patch.points[4]->uv), Math3D::Vec2f(patch.points[5]->uv), Math3D::Vec2f(patch.points[6]->uv), Math3D::Vec2f(patch.points[7]->uv), u);
			pretex.horiz3[i] = Bernstein3D(Math3D::Vec2f(patch.points[8]->uv), Math3D::Vec2f(patch.points[9]->uv), Math3D::Vec2f(patch.points[10]->uv), Math3D::Vec2f(patch.points[11]->uv), u);
			pretex.horiz4[i] = Bernstein3D(Math3D::Vec2f(patch.points[12]->uv), Math3D::Vec2f(patch.points[13]->uv), Math3D::Vec2f(patch.points[14]->uv), Math3D::Vec2f(patch.points[15]->uv), u);
		}

		if (computeNormals) {
			prederivU.horiz1[i] = Bernstein3DDerivative(patch.points[0]->pos, patch.points[1]->pos, patch.points[2]->pos, patch.points[3]->pos, u);
			prederivU.horiz2[i] = Bernstein3DDerivative(patch.points[4]->pos, patch.points[5]->pos, patch.points[6]->pos, patch.points[7]->pos, u);
			prederivU.horiz3[i] = Bernstein3DDerivative(patch.points[8]->pos, patch.points[9]->pos, patch.points[10]->pos, patch.points[11]->pos, u);
			prederivU.horiz4[i] = Bernstein3DDerivative(patch.points[12]->pos, patch.points[13]->pos, patch.points[14]->pos, patch.points[15]->pos, u);
		}
	}


	for (int tile_v = 0; tile_v < tess_v + 1; ++tile_v) {
		for (int tile_u = 0; tile_u < tess_u + 1; ++tile_u) {
			float u = ((float)tile_u / (float)tess_u);
			float v = ((float)tile_v / (float)tess_v);
			float bv = v;

			SimpleVertex &vert = vertices[tile_v * (tess_u + 1) + tile_u];

			if (computeNormals) {
				const Vec3f derivU = prederivU.Bernstein3D(tile_u, bv);
				const Vec3f derivV = prepos.Bernstein3DDerivative(tile_u, bv);

				vert.nrm = Cross(derivU, derivV).Normalized();
				if (patch.patchFacing)
					vert.nrm *= -1.0f;
			} else {
				vert.nrm.SetZero();
			}

			vert.pos = prepos.Bernstein3D(tile_u, bv);

			if (!sampleTexcoords) {
				// Generate texcoord
				vert.uv[0] = u + patch.u_index * third;
				vert.uv[1] = v + patch.v_index * third;
			} else {
				// Sample UV from control points
				const Math3D::Vec2f res = pretex.Bernstein3D(tile_u, bv);
				vert.uv[0] = res.x;
				vert.uv[1] = res.y;
			} 

			if (sampleColors) {
				vert.color_32 = precol.Bernstein3D(tile_u, bv).ToRGBA();
			} else {
				memcpy(vert.color, patch.points[0]->color, 4);
			}
		}
	}

	GEPatchPrimType prim_type = patch.primType;
	// Combine the vertices into triangles.
	for (int tile_v = 0; tile_v < tess_v; ++tile_v) {
		for (int tile_u = 0; tile_u < tess_u; ++tile_u) {
			int total = patch.index * (tess_u + 1) * (tess_v + 1);
			int idx0 = total + tile_v * (tess_u + 1) + tile_u;
			int idx1 = total + tile_v * (tess_u + 1) + tile_u + 1;
			int idx2 = total + (tile_v + 1) * (tess_u + 1) + tile_u;
			int idx3 = total + (tile_v + 1) * (tess_u + 1) + tile_u + 1;

			CopyQuadIndex(indices, prim_type, idx0, idx1, idx2, idx3);
			count += 6;
		}
	}
	dest += (tess_u + 1) * (tess_v + 1) * sizeof(SimpleVertex);
}

// Prepare mesh of one patch for "Instanced Tessellation".
static void TessellateBezierPatchHardware(u8 *&dest, u16 *indices, int &count, int tess_u, int tess_v, GEPatchPrimType primType) {
	SimpleVertex *&vertices = (SimpleVertex*&)dest;

	float inv_u = 1.0f / (float)tess_u;
	float inv_v = 1.0f / (float)tess_v;

	// Generating simple input vertices for the bezier-computing vertex shader.
	for (int tile_v = 0; tile_v < tess_v + 1; ++tile_v) {
		for (int tile_u = 0; tile_u < tess_u + 1; ++tile_u) {
			SimpleVertex &vert = vertices[tile_v * (tess_u + 1) + tile_u];

			vert.pos.x = (float)tile_u * inv_u;
			vert.pos.y = (float)tile_v * inv_v;
		}
	}

	// Combine the vertices into triangles.
	for (int tile_v = 0; tile_v < tess_v; ++tile_v) {
		for (int tile_u = 0; tile_u < tess_u; ++tile_u) {
			int idx0 = tile_v * (tess_u + 1) + tile_u;
			int idx1 = tile_v * (tess_u + 1) + tile_u + 1;
			int idx2 = (tile_v + 1) * (tess_u + 1) + tile_u;
			int idx3 = (tile_v + 1) * (tess_u + 1) + tile_u + 1;

			CopyQuadIndex(indices, primType, idx0, idx1, idx2, idx3);
			count += 6;
		}
	}
}

void TessellateBezierPatch(u8 *&dest, u16 *&indices, int &count, int tess_u, int tess_v, const BezierPatch &patch, u32 origVertType) {
	switch (g_Config.iSplineBezierQuality) {
	case LOW_QUALITY:
		_BezierPatchLowQuality(dest, indices, count, tess_u, tess_v, patch, origVertType);
		break;
	case MEDIUM_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, std::max(tess_u / 2, 1), std::max(tess_v / 2, 1), patch, origVertType);
		break;
	case HIGH_QUALITY:
		_BezierPatchHighQuality(dest, indices, count, tess_u, tess_v, patch, origVertType);
		break;
	}
}

class SimpleBufferManager {
private:
	u8 *buf_;
	size_t totalSize, maxSize_;
public:
	SimpleBufferManager(u8 *buf, size_t maxSize)
		: buf_(buf), totalSize(0), maxSize_(maxSize) {}

	u8 *Allocate(size_t size) {
		size = (size + 15) & ~15; // Align for 16 bytes

		if ((totalSize + size) > maxSize_)
			return nullptr; // No more memory

		size_t tmp = totalSize;
		totalSize += size;
		return buf_ + tmp;
	}
};

// This maps GEPatchPrimType to GEPrimitiveType.
const GEPrimitiveType primType[] = { GE_PRIM_TRIANGLES, GE_PRIM_LINES, GE_PRIM_POINTS, GE_PRIM_POINTS };

void DrawEngineCommon::SubmitSpline(const void *control_points, const void *indices, int tess_u, int tess_v, int count_u, int count_v, int type_u, int type_v, GEPatchPrimType prim_type, bool computeNormals, bool patchFacing, u32 vertType, int *bytesRead) {
	PROFILE_THIS_SCOPE("spline");
	DispatchFlush();

	// Real hardware seems to draw nothing when given < 4 either U or V.
	if (count_u < 4 || count_v < 4)
		return;

	SimpleBufferManager managedBuf(decoded, DECODED_VERTEX_BUFFER_SIZE);

	u16 index_lower_bound = 0;
	u16 index_upper_bound = count_u * count_v - 1;
	IndexConverter idxConv(vertType, indices);
	if (indices)
		GetIndexBounds(indices, count_u * count_v, vertType, &index_lower_bound, &index_upper_bound);

	VertexDecoder *origVDecoder = GetVertexDecoder((vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24));
	*bytesRead = count_u * count_v * origVDecoder->VertexSize();

	// Simplify away bones and morph before proceeding
	SimpleVertex *simplified_control_points = (SimpleVertex *)managedBuf.Allocate(sizeof(SimpleVertex) * index_upper_bound + 1);
	u8 *temp_buffer = managedBuf.Allocate(sizeof(SimpleVertex) * count_u * count_v);

	u32 origVertType = vertType;
	vertType = NormalizeVertices((u8 *)simplified_control_points, temp_buffer, (u8 *)control_points, index_lower_bound, index_upper_bound, vertType);

	VertexDecoder *vdecoder = GetVertexDecoder(vertType);

	int vertexSize = vdecoder->VertexSize();
	if (vertexSize != sizeof(SimpleVertex)) {
		ERROR_LOG(G3D, "Something went really wrong, vertex size: %i vs %i", vertexSize, (int)sizeof(SimpleVertex));
	}

	// Make an array of pointers to the control points, to get rid of indices.
	SimpleVertex **points = (SimpleVertex **)managedBuf.Allocate(sizeof(SimpleVertex *) * count_u * count_v);
	for (int idx = 0; idx < count_u * count_v; idx++)
		points[idx] = simplified_control_points + (indices ? idxConv.convert(idx) : idx);

	int count = 0;
	u8 *dest = splineBuffer;

	SplinePatchLocal patch;
	patch.tess_u = tess_u;
	patch.tess_v = tess_v;
	patch.type_u = type_u;
	patch.type_v = type_v;
	patch.count_u = count_u;
	patch.count_v = count_v;
	patch.points = points;
	patch.computeNormals = computeNormals;
	patch.primType = prim_type;
	patch.patchFacing = patchFacing;

	if (CanUseHardwareTessellation(prim_type)) {
		float *pos = (float*)managedBuf.Allocate(sizeof(float) * count_u * count_v * 4); // Size 4 float
		float *tex = (float*)managedBuf.Allocate(sizeof(float) * count_u * count_v * 4); // Size 4 float
		float *col = (float*)managedBuf.Allocate(sizeof(float) * count_u * count_v * 4); // Size 4 float
		const bool hasColor = (origVertType & GE_VTYPE_COL_MASK) != 0;
		const bool hasTexCoords = (origVertType & GE_VTYPE_TC_MASK) != 0;

		int posStride, texStride, colStride;
		tessDataTransfer->PrepareBuffers(pos, tex, col, posStride, texStride, colStride, count_u * count_v, hasColor, hasTexCoords);
		float *p = pos;
		float *t = tex;
		float *c = col;
		for (int idx = 0; idx < count_u * count_v; idx++) {
			memcpy(p, points[idx]->pos.AsArray(), 3 * sizeof(float));
			p += posStride;
			if (hasTexCoords) {
				memcpy(t, points[idx]->uv, 2 * sizeof(float));
				t += texStride;
			}
			if (hasColor) {
				memcpy(c, Vec4f::FromRGBA(points[idx]->color_32).AsArray(), 4 * sizeof(float));
				c += colStride;
			}
		}
		if (!hasColor)
			memcpy(col, Vec4f::FromRGBA(points[0]->color_32).AsArray(), 4 * sizeof(float));

		tessDataTransfer->SendDataToShader(pos, tex, col, count_u * count_v, hasColor, hasTexCoords);
		TessellateSplinePatchHardware(dest, quadIndices_, count, patch);
		numPatches = (count_u - 3) * (count_v - 3);
	} else {
		int maxVertexCount = SPLINE_BUFFER_SIZE / vertexSize;
		TessellateSplinePatch(dest, quadIndices_, count, patch, origVertType, maxVertexCount);
	}

	u32 vertTypeWithIndex16 = (vertType & ~GE_VTYPE_IDX_MASK) | GE_VTYPE_IDX_16BIT;

	UVScale prevUVScale;
	if ((origVertType & GE_VTYPE_TC_MASK) != 0) {
		// We scaled during Normalize already so let's turn it off when drawing.
		prevUVScale = gstate_c.uv;
		gstate_c.uv.uScale = 1.0f;
		gstate_c.uv.vScale = 1.0f;
		gstate_c.uv.uOff = 0.0f;
		gstate_c.uv.vOff = 0.0f;
	}

	uint32_t vertTypeID = GetVertTypeID(vertTypeWithIndex16, gstate.getUVGenMode());

	int generatedBytesRead;
	DispatchSubmitPrim(splineBuffer, quadIndices_, PatchPrimToPrim(prim_type), count, vertTypeID, &generatedBytesRead);

	DispatchFlush();

	if ((origVertType & GE_VTYPE_TC_MASK) != 0) {
		gstate_c.uv = prevUVScale;
	}
}

void DrawEngineCommon::SubmitBezier(const void *control_points, const void *indices, int tess_u, int tess_v, int count_u, int count_v, GEPatchPrimType prim_type, bool computeNormals, bool patchFacing, u32 vertType, int *bytesRead) {
	PROFILE_THIS_SCOPE("bezier");
	DispatchFlush();

	// Real hardware seems to draw nothing when given < 4 either U or V.
	// This would result in num_patches_u / num_patches_v being 0.
	if (count_u < 4 || count_v < 4)
		return;

	SimpleBufferManager managedBuf(decoded, DECODED_VERTEX_BUFFER_SIZE);

	u16 index_lower_bound = 0;
	u16 index_upper_bound = count_u * count_v - 1;
	IndexConverter idxConv(vertType, indices);
	if (indices)
		GetIndexBounds(indices, count_u*count_v, vertType, &index_lower_bound, &index_upper_bound);

	VertexDecoder *origVDecoder = GetVertexDecoder((vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24));
	*bytesRead = count_u * count_v * origVDecoder->VertexSize();

	// Simplify away bones and morph before proceeding
	// There are normally not a lot of control points so just splitting decoded should be reasonably safe, although not great.
	SimpleVertex *simplified_control_points = (SimpleVertex *)managedBuf.Allocate(sizeof(SimpleVertex) * index_upper_bound + 1);
	u8 *temp_buffer = managedBuf.Allocate(sizeof(SimpleVertex) * count_u * count_v);

	u32 origVertType = vertType;
	vertType = NormalizeVertices((u8 *)simplified_control_points, temp_buffer, (u8 *)control_points, index_lower_bound, index_upper_bound, vertType);

	VertexDecoder *vdecoder = GetVertexDecoder(vertType);

	int vertexSize = vdecoder->VertexSize();
	if (vertexSize != sizeof(SimpleVertex)) {
		ERROR_LOG(G3D, "Something went really wrong, vertex size: %i vs %i", vertexSize, (int)sizeof(SimpleVertex));
	}

	// If specified as 0, uses 1.
	if (tess_u < 1) tess_u = 1;
	if (tess_v < 1) tess_v = 1;

	// Make an array of pointers to the control points, to get rid of indices.
	SimpleVertex **points = (SimpleVertex **)managedBuf.Allocate(sizeof(SimpleVertex *) * count_u * count_v);
	for (int idx = 0; idx < count_u * count_v; idx++)
		points[idx] = simplified_control_points + (indices ? idxConv.convert(idx) : idx);

	int count = 0;
	u8 *dest = splineBuffer;
	u16 *inds = quadIndices_;

	// Bezier patches share less control points than spline patches. Otherwise they are pretty much the same (except bezier don't support the open/close thing)
	int num_patches_u = (count_u - 1) / 3;
	int num_patches_v = (count_v - 1) / 3;
	if (CanUseHardwareTessellation(prim_type)) {
		float *pos = (float*)managedBuf.Allocate(sizeof(float) * count_u * count_v * 4); // Size 4 float
		float *tex = (float*)managedBuf.Allocate(sizeof(float) * count_u * count_v * 4); // Size 4 float
		float *col = (float*)managedBuf.Allocate(sizeof(float) * count_u * count_v * 4); // Size 4 float
		const bool hasColor = (origVertType & GE_VTYPE_COL_MASK) != 0;
		const bool hasTexCoords = (origVertType & GE_VTYPE_TC_MASK) != 0;

		int posStride, texStride, colStride;
		tessDataTransfer->PrepareBuffers(pos, tex, col, posStride, texStride, colStride, count_u * count_v, hasColor, hasTexCoords);
		float *p = pos;
		float *t = tex;
		float *c = col;
		for (int idx = 0; idx < count_u * count_v; idx++) {
			memcpy(p, points[idx]->pos.AsArray(), 3 * sizeof(float));
			p += posStride;
			if (hasTexCoords) {
				memcpy(t, points[idx]->uv, 2 * sizeof(float));
				t += texStride;
			}
			if (hasColor) {
				memcpy(c, Vec4f::FromRGBA(points[idx]->color_32).AsArray(), 4 * sizeof(float));
				c += colStride;
			}
		}
		if (!hasColor)
			memcpy(col, Vec4f::FromRGBA(points[0]->color_32).AsArray(), 4 * sizeof(float));

		tessDataTransfer->SendDataToShader(pos, tex, col, count_u * count_v, hasColor, hasTexCoords);
		TessellateBezierPatchHardware(dest, inds, count, tess_u, tess_v, prim_type);
		numPatches = num_patches_u * num_patches_v;
	} else {
		BezierPatch *patches = (BezierPatch *)managedBuf.Allocate(sizeof(BezierPatch) * num_patches_u * num_patches_v);
		for (int patch_u = 0; patch_u < num_patches_u; patch_u++) {
			for (int patch_v = 0; patch_v < num_patches_v; patch_v++) {
				BezierPatch& patch = patches[patch_u + patch_v * num_patches_u];
				for (int point = 0; point < 16; ++point) {
					int idx = (patch_u * 3 + point % 4) + (patch_v * 3 + point / 4) * count_u;
					patch.points[point] = points[idx];
				}
				patch.u_index = patch_u * 3;
				patch.v_index = patch_v * 3;
				patch.index = patch_v * num_patches_u + patch_u;
				patch.primType = prim_type;
				patch.computeNormals = computeNormals;
				patch.patchFacing = patchFacing;
			}
		}
		int maxVertices = SPLINE_BUFFER_SIZE / vertexSize;
		// Downsample until it fits, in case crazy tessellation factors are sent.
		while ((tess_u + 1) * (tess_v + 1) * num_patches_u * num_patches_v > maxVertices) {
			tess_u /= 2;
			tess_v /= 2;
		}
		// We shouldn't really split up into separate 4x4 patches, instead we should do something that works
		// like the splines, so we subdivide across the whole "mega-patch".
		for (int patch_idx = 0; patch_idx < num_patches_u*num_patches_v; ++patch_idx) {
			const BezierPatch &patch = patches[patch_idx];
			TessellateBezierPatch(dest, inds, count, tess_u, tess_v, patch, origVertType);
		}
	}

	u32 vertTypeWithIndex16 = (vertType & ~GE_VTYPE_IDX_MASK) | GE_VTYPE_IDX_16BIT;

	UVScale prevUVScale;
	if (origVertType & GE_VTYPE_TC_MASK) {
		// We scaled during Normalize already so let's turn it off when drawing.
		prevUVScale = gstate_c.uv;
		gstate_c.uv.uScale = 1.0f;
		gstate_c.uv.vScale = 1.0f;
		gstate_c.uv.uOff = 0;
		gstate_c.uv.vOff = 0;
	}

	uint32_t vertTypeID = GetVertTypeID(vertTypeWithIndex16, gstate.getUVGenMode());
	int generatedBytesRead;
	DispatchSubmitPrim(splineBuffer, quadIndices_, PatchPrimToPrim(prim_type), count, vertTypeID, &generatedBytesRead);

	DispatchFlush();

	if (origVertType & GE_VTYPE_TC_MASK) {
		gstate_c.uv = prevUVScale;
	}
}
