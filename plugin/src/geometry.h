// Face geometry derived from Apple Vision landmarks (76-point constellation).
// All coordinates are source pixels, x right, y down.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

struct Vec2 {
	float x = 0, y = 0;
};

inline Vec2 v2(float x, float y) { return Vec2{x, y}; }
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 a) { return std::sqrt(dot(a, a)); }
inline Vec2 normalize(Vec2 a)
{
	float l = length(a);
	return l > 1e-6f ? a * (1.0f / l) : Vec2{0, -1};
}
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

// Landmark regions in the order we store them
enum Region {
	R_CONTOUR,
	R_LEYE,
	R_REYE,
	R_LBROW,
	R_RBROW,
	R_NOSE,
	R_NOSECREST,
	R_MEDIAN,
	R_OUTERLIPS,
	R_INNERLIPS,
	R_LPUPIL,
	R_RPUPIL,
	R_COUNT
};

constexpr int kRegionSize[R_COUNT] = {17, 6, 6, 6, 6, 8, 6, 10, 14, 6, 1, 1};
constexpr int kNumPoints = 87;

constexpr int region_offset(int r)
{
	int o = 0;
	for (int i = 0; i < r; i++)
		o += kRegionSize[i];
	return o;
}

struct Landmarks {
	Vec2 p[kNumPoints];
	const Vec2 *region(Region r) const { return p + region_offset(r); }
};

// Per-face skin color model measured from the frame (chroma in the shader's YCbCr space)
struct SkinModel {
	float cb = 0, cr = 0, sigma = 0.02f, luma = 0.6f;
	bool valid = false;
};

struct FaceInfo {
	int id = 0;
	float presence = 0; // 0..1 fade in/out
	Landmarks lm;
	SkinModel skin;
};

constexpr int kRadialBins = 128;

// Everything the masks and the warp need, derived from one face's landmarks
struct FaceShape {
	Vec2 up, right;          // face axes (up = toward forehead)
	Vec2 chin, nose_tip, brow_mid, eye_level, mouth;
	float fw = 1, fh = 1;    // temple-to-temple width, chin-to-brow height
	float blur_radius = 4;   // smoothing radius in pixels

	std::vector<Vec2> core;        // contour + brow band (always skin, minus features)
	std::vector<Vec2> forehead;    // up to the estimated hairline (color-gated)
	std::vector<Vec2> neck;        // below the jaw (color-gated)
	std::vector<Vec2> ear_l, ear_r;
	std::vector<Vec2> eye_l, eye_r, brow_l, brow_r, lips;
	std::vector<Vec2> under_eye_l, under_eye_r, fold_l, fold_r;

	// Radial table around nose_tip used by the slim warp: radius of the face outline per angle
	float radius[kRadialBins];
	float max_radius = 0;
};

bool build_face_shape(const Landmarks &lm, FaceShape &out);

// Inverse displacement (sampling offset, pixels) for the slim warp at point p
Vec2 slim_offset(const FaceShape &s, Vec2 p, float k);

// Fill a grid of sampling offsets (RG per node, nodes every `cell` pixels)
void build_warp_grid(const std::vector<FaceShape> &shapes, const std::vector<float> &strength, int width, int height,
		     int cell, int gw, int gh, std::vector<float> &out_rg);

// Measure a skin color model from a BGRA image (vision scale)
SkinModel measure_skin(const Landmarks &lm, const FaceShape &s, const uint8_t *bgra, int w, int h, int stride,
		       float scale_x, float scale_y);
