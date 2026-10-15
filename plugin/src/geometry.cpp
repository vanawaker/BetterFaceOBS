#include "geometry.h"

#include <algorithm>

namespace {

constexpr float kPi = 3.14159265358979f;

// Slim warp shape (see slim_offset)
constexpr float kInnerStart = 0.35f; // fraction of the outline radius where compression starts
constexpr float kFalloff = 0.5f;     // how far past the outline the warp fades out (fraction of radius)
constexpr float kChinWeight = 0.7f;  // chin moves up a bit less than the jaw moves in

float smoothstep(float a, float b, float x)
{
	float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

Vec2 mean(const Vec2 *p, int n)
{
	Vec2 m;
	for (int i = 0; i < n; i++)
		m = m + p[i];
	return m * (1.0f / (float)n);
}

// Extreme points of a region along an axis
Vec2 arg_min(const Vec2 *p, int n, Vec2 axis)
{
	int best = 0;
	for (int i = 1; i < n; i++)
		if (dot(p[i], axis) < dot(p[best], axis))
			best = i;
	return p[best];
}

Vec2 arg_max(const Vec2 *p, int n, Vec2 axis) { return arg_min(p, n, axis * -1.0f); }

// Extreme of a region as a weighted mean leaning on the most extreme points (tau in pixels).
// arg_max jumps between two nearly tied points, e.g. the upper and lower point of a nose wing.
Vec2 soft_max(const Vec2 *p, int n, Vec2 axis, float tau)
{
	float best = -1e9f;
	for (int i = 0; i < n; i++)
		best = std::max(best, dot(p[i], axis));
	Vec2 acc;
	float wsum = 0;
	for (int i = 0; i < n; i++) {
		float w = std::exp((dot(p[i], axis) - best) / std::max(tau, 1e-3f));
		acc = acc + p[i] * w;
		wsum += w;
	}
	return acc * (1.0f / wsum);
}

Vec2 soft_min(const Vec2 *p, int n, Vec2 axis, float tau) { return soft_max(p, n, axis * -1.0f, tau); }

std::vector<Vec2> ellipse(Vec2 c, Vec2 ax, Vec2 ay, float a, float b, int n)
{
	std::vector<Vec2> out;
	out.reserve(n);
	for (int i = 0; i < n; i++) {
		float t = 2.0f * kPi * (float)i / (float)n;
		out.push_back(c + ax * (a * std::cos(t)) + ay * (b * std::sin(t)));
	}
	return out;
}

std::vector<Vec2> capsule(Vec2 a, Vec2 b, float half_w, int n_half)
{
	Vec2 d = normalize(b - a);
	Vec2 nrm = {-d.y, d.x};
	std::vector<Vec2> out;
	for (int i = 0; i <= n_half; i++) {
		float t = kPi * (float)i / (float)n_half;
		out.push_back(b + nrm * (half_w * std::cos(t)) + d * (half_w * std::sin(t)));
	}
	for (int i = 0; i <= n_half; i++) {
		float t = kPi * (float)i / (float)n_half;
		out.push_back(a - nrm * (half_w * std::cos(t)) - d * (half_w * std::sin(t)));
	}
	return out;
}

float ray_polygon(Vec2 o, Vec2 dir, const std::vector<Vec2> &poly)
{
	float best = -1.0f;
	size_t n = poly.size();
	for (size_t i = 0; i < n; i++) {
		Vec2 a = poly[i] - o;
		Vec2 b = poly[(i + 1) % n] - o;
		Vec2 e = b - a;
		float den = dir.x * e.y - dir.y * e.x;
		if (std::fabs(den) < 1e-9f)
			continue;
		float t = (a.x * e.y - a.y * e.x) / den; // distance along dir
		float u = (a.x * dir.y - a.y * dir.x) / den; // position along edge
		if (t > 0.0f && u >= 0.0f && u <= 1.0f)
			best = std::max(best, t);
	}
	return best;
}

} // namespace

bool build_face_shape(const Landmarks &lm, FaceShape &s)
{
	const Vec2 *C = lm.region(R_CONTOUR);
	const Vec2 *eye_l = lm.region(R_LEYE);
	const Vec2 *eye_r = lm.region(R_REYE);
	const Vec2 *brow_l = lm.region(R_LBROW);
	const Vec2 *brow_r = lm.region(R_RBROW);
	const Vec2 *nose = lm.region(R_NOSE);
	const Vec2 *crest = lm.region(R_NOSECREST);
	const Vec2 *lips = lm.region(R_OUTERLIPS);

	s.chin = C[8];
	s.brow_mid = (mean(brow_l, 6) + mean(brow_r, 6)) * 0.5f;
	s.up = normalize(s.brow_mid - s.chin);
	s.right = {-s.up.y, s.up.x};
	s.eye_level = (C[0] + C[16]) * 0.5f;
	s.mouth = mean(lips, 14);
	// Nose crest: 0-3 run down the bridge to the tip, 4 and 5 are the two nose wings at about the
	// same height. Taking the lowest crest point flipped between the wings whenever the head turned
	// through frontal, and the whole slim warp jumped sideways with it.
	s.nose_tip = (crest[3] + crest[4] + crest[5]) * (1.0f / 3.0f);
	s.fw = length(C[0] - C[16]);
	s.fh = dot(s.brow_mid - s.chin, s.up);
	if (s.fw < 8.0f || s.fh < 8.0f)
		return false;
	s.blur_radius = std::clamp(s.fw * 0.035f, 2.0f, 40.0f);

	const Vec2 up = s.up, right = s.right;
	const float fw = s.fw, fh = s.fh;
	const float brow_h = dot(s.brow_mid - s.eye_level, up);

	// Lower face plus the brow band
	float top = brow_h + 0.12f * fh;
	s.core.assign(C, C + 17);
	s.core.push_back(C[16] + up * top);
	s.core.push_back(C[0] + up * top);

	// Forehead up to an estimated hairline, gated by skin color in the shader
	{
		float a = fw * 0.5f;
		float b = brow_h + 0.55f * fh;
		s.forehead.clear();
		for (int i = 0; i <= 12; i++) {
			float t = kPi * (float)i / 12.0f;
			s.forehead.push_back(s.eye_level + right * (a * std::cos(t)) + up * (b * std::sin(t)));
		}
		s.forehead.push_back(s.eye_level - up * (0.1f * fh) - right * a);
		s.forehead.push_back(s.eye_level - up * (0.1f * fh) + right * a);
	}

	// Ears: beside the contour from eye level down to the nose base
	s.ear_r = {C[0] + up * (0.15f * fh), C[0] + up * (0.15f * fh) + right * (0.2f * fw),
		   C[3] + right * (0.16f * fw), C[3], C[2], C[1]};
	s.ear_l = {C[16] + up * (0.15f * fh), C[16] + up * (0.15f * fh) - right * (0.2f * fw),
		   C[13] - right * (0.16f * fw), C[13], C[14], C[15]};

	// Neck: below the jaw
	s.neck.assign(C + 4, C + 13);
	s.neck.push_back(s.chin - up * (0.95f * fh) - right * (0.4f * fw));
	s.neck.push_back(s.chin - up * (0.95f * fh) + right * (0.4f * fw));

	s.eye_l.assign(eye_l, eye_l + 6);
	s.eye_r.assign(eye_r, eye_r + 6);
	s.brow_l.assign(brow_l, brow_l + 6);
	s.brow_r.assign(brow_r, brow_r + 6);
	s.lips.assign(lips, lips + 14);

	// Under-eye (dark circles)
	auto under_eye = [&](const Vec2 *e) {
		Vec2 in = arg_min(e, 6, right), out = arg_max(e, 6, right);
		float ew = length(out - in);
		Vec2 mid = (in + out) * 0.5f;
		Vec2 bottom = arg_min(e, 6, up);
		float b = 0.055f * fh;
		Vec2 c = mid + up * (dot(bottom - mid, up) - 0.012f * fh - b);
		return ellipse(c, right, up, 0.62f * ew, b, 16);
	};
	s.under_eye_l = under_eye(eye_l);
	s.under_eye_r = under_eye(eye_r);

	// Nasolabial folds: nose wing to just outside the mouth corner
	{
		float tau = 0.015f * fw;
		Vec2 wing_r = soft_max(nose, 8, right, tau), wing_l = soft_min(nose, 8, right, tau);
		Vec2 mc_r = soft_max(lips, 14, right, tau), mc_l = soft_min(lips, 14, right, tau);
		float hw = 0.035f * fw;
		s.fold_r = capsule(wing_r + right * (0.05f * fw) - up * (0.01f * fh),
				   mc_r + right * (0.05f * fw) - up * (0.04f * fh), hw, 6);
		s.fold_l = capsule(wing_l - right * (0.05f * fw) - up * (0.01f * fh),
				   mc_l - right * (0.05f * fw) - up * (0.04f * fh), hw, 6);
	}

	// Outline radius per angle around the nose tip (contour below, forehead arc above)
	std::vector<Vec2> outline(C, C + 17);
	for (int i = 11; i >= 1; i--) {
		float t = kPi * (float)i / 12.0f;
		outline.push_back(s.eye_level + right * (fw * 0.5f * std::cos(t)) + up * ((brow_h + 0.55f * fh) * std::sin(t)));
	}
	s.max_radius = 0;
	float last = fw * 0.5f;
	for (int b = 0; b < kRadialBins; b++) {
		float th = -kPi + 2.0f * kPi * ((float)b + 0.5f) / (float)kRadialBins;
		float r = ray_polygon(s.nose_tip, {std::cos(th), std::sin(th)}, outline);
		if (r <= 0.0f)
			r = last;
		s.radius[b] = r;
		last = r;
		s.max_radius = std::max(s.max_radius, r);
	}
	return true;
}

Vec2 slim_offset(const FaceShape &s, Vec2 p, float k)
{
	Vec2 v = p - s.nose_tip;
	float r = length(v);
	if (r < 1e-3f || k <= 0.0f)
		return {};

	// Angle from the chin direction: 0 = chin, pi/2 = cheek at nose height, pi = forehead
	float lx = dot(v, s.right), ly = dot(v, s.up);
	float alpha = std::atan2(std::fabs(lx), -ly);
	const float deg = kPi / 180.0f;
	float m = (kChinWeight + (1.0f - kChinWeight) * smoothstep(0.0f, 35.0f * deg, alpha)) *
		  (1.0f - smoothstep(80.0f * deg, 115.0f * deg, alpha));
	if (m <= 0.0f)
		return {};

	float th = std::atan2(v.y, v.x);
	float fb = (th + kPi) / (2.0f * kPi) * (float)kRadialBins - 0.5f;
	int b0 = (int)std::floor(fb);
	float t = fb - (float)b0;
	int i0 = (b0 % kRadialBins + kRadialBins) % kRadialBins;
	int i1 = (i0 + 1) % kRadialBins;
	float R = s.radius[i0] * (1.0f - t) + s.radius[i1] * t;
	if (R < 1.0f)
		return {};

	float rho = r / R;
	float phi = rho < 1.0f ? smoothstep(kInnerStart, 1.0f, rho) : 1.0f - smoothstep(1.0f, 1.0f + kFalloff, rho);
	if (phi <= 0.0f)
		return {};
	return v * (k * m * R * phi / r);
}

void build_warp_grid(const std::vector<FaceShape> &shapes, const std::vector<float> &strength, int width, int height,
		     int cell, int gw, int gh, std::vector<float> &out)
{
	out.assign((size_t)gw * gh * 2, 0.0f);
	for (size_t f = 0; f < shapes.size(); f++) {
		float k = strength[f];
		if (k <= 0.0f)
			continue;
		const FaceShape &s = shapes[f];
		float reach = s.max_radius * (1.0f + kFalloff) + (float)cell;
		int x0 = std::max(0, (int)std::floor((s.nose_tip.x - reach) / (float)cell));
		int x1 = std::min(gw - 1, (int)std::ceil((s.nose_tip.x + reach) / (float)cell));
		int y0 = std::max(0, (int)std::floor((s.nose_tip.y - reach) / (float)cell));
		int y1 = std::min(gh - 1, (int)std::ceil((s.nose_tip.y + reach) / (float)cell));
		for (int j = y0; j <= y1; j++) {
			for (int i = x0; i <= x1; i++) {
				Vec2 d = slim_offset(s, {(float)(i * cell), (float)(j * cell)}, k);
				float *o = &out[((size_t)j * gw + i) * 2];
				o[0] += d.x;
				o[1] += d.y;
			}
		}
	}
	(void)width;
	(void)height;
}

SkinModel measure_skin(const Landmarks &lm, const FaceShape &s, const uint8_t *bgra, int w, int h, int stride,
		       float sx, float sy)
{
	const Vec2 *eye_l = lm.region(R_LEYE);
	const Vec2 *eye_r = lm.region(R_REYE);
	const Vec2 *lips = lm.region(R_OUTERLIPS);
	const Vec2 *crest = lm.region(R_NOSECREST);
	Vec2 mc_r = soft_max(lips, 14, s.right, 0.015f * s.fw), mc_l = soft_min(lips, 14, s.right, 0.015f * s.fw);

	std::vector<Vec2> pts;
	auto grid = [&](Vec2 c, float step) {
		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++)
				pts.push_back(c + s.right * (step * (float)i) + s.up * (step * (float)j));
	};
	float st = 0.04f * s.fw;
	grid(lerp(mean(eye_r, 6), mc_l, 0.55f) - s.right * (0.06f * s.fw), st);
	grid(lerp(mean(eye_l, 6), mc_r, 0.55f) + s.right * (0.06f * s.fw), st);
	grid(s.brow_mid + s.up * (0.14f * s.fh), st);
	grid(lerp(s.mouth, s.chin, 0.55f), st * 0.6f);
	pts.push_back(crest[2]);
	pts.push_back(crest[3]);

	struct Sample {
		float y, cb, cr;
	};
	std::vector<Sample> samples;
	for (Vec2 p : pts) {
		int cx = (int)(p.x * sx), cy = (int)(p.y * sy);
		if (cx < 1 || cy < 1 || cx >= w - 1 || cy >= h - 1)
			continue;
		float r = 0, g = 0, b = 0;
		for (int j = -1; j <= 1; j++) {
			const uint8_t *row = bgra + (size_t)(cy + j) * stride;
			for (int i = -1; i <= 1; i++) {
				const uint8_t *px = row + (size_t)(cx + i) * 4;
				b += px[0];
				g += px[1];
				r += px[2];
			}
		}
		r /= 9.0f * 255.0f;
		g /= 9.0f * 255.0f;
		b /= 9.0f * 255.0f;
		float y = 0.299f * r + 0.587f * g + 0.114f * b;
		samples.push_back({y, (b - y) * 0.564f, (r - y) * 0.713f});
	}

	SkinModel m;
	if (samples.size() < 8)
		return m;

	auto median = [](std::vector<float> v) {
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		return v[v.size() / 2];
	};
	std::vector<float> ys, cbs, crs;
	for (auto &q : samples) {
		ys.push_back(q.y);
		cbs.push_back(q.cb);
		crs.push_back(q.cr);
	}
	float mcb = median(cbs), mcr = median(crs);

	// Keep the 70 % of samples closest to the median chroma (drops beard, glasses, hair)
	std::vector<std::pair<float, size_t>> dist;
	for (size_t i = 0; i < samples.size(); i++)
		dist.push_back({std::hypot(samples[i].cb - mcb, samples[i].cr - mcr), i});
	std::sort(dist.begin(), dist.end());
	size_t keep = std::max<size_t>(6, dist.size() * 7 / 10);
	float acb = 0, acr = 0;
	for (size_t i = 0; i < keep; i++) {
		acb += samples[dist[i].second].cb;
		acr += samples[dist[i].second].cr;
	}
	acb /= (float)keep;
	acr /= (float)keep;
	float var = 0;
	for (size_t i = 0; i < keep; i++) {
		float dcb = samples[dist[i].second].cb - acb, dcr = samples[dist[i].second].cr - acr;
		var += dcb * dcb + dcr * dcr;
	}
	m.cb = acb;
	m.cr = acr;
	m.sigma = std::clamp(std::sqrt(var / (float)keep), 0.008f, 0.05f);
	m.luma = median(ys);
	m.valid = true;
	return m;
}
