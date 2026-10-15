#include "frame-convert.h"

#include <obs.h>

#include <dispatch/dispatch.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kVisionLongSide = 1280;

// YUV -> RGB with the frame's own matrix, in 12-bit fixed point
struct YuvToRgb {
	int m[12];
	int lo[3], hi[3];

	explicit YuvToRgb(const obs_source_frame *f)
	{
		// matrix maps (y, u, v, 1) in 0..1 to rgb in 0..1; inputs are 0..255, output 0..255
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++)
				m[r * 4 + c] = (int)std::lround(f->color_matrix[r * 4 + c] * 4096.0f);
			m[r * 4 + 3] = (int)std::lround(f->color_matrix[r * 4 + 3] * 255.0f * 4096.0f);
		}
		for (int i = 0; i < 3; i++) {
			lo[i] = f->full_range ? 0 : (int)std::lround(f->color_range_min[i] * 255.0f);
			hi[i] = f->full_range ? 255 : (int)std::lround(f->color_range_max[i] * 255.0f);
		}
	}

	inline void apply(int y, int u, int v, uint8_t *out) const
	{
		y = std::clamp(y, lo[0], hi[0]);
		u = std::clamp(u, lo[1], hi[1]);
		v = std::clamp(v, lo[2], hi[2]);
		int r = (m[0] * y + m[1] * u + m[2] * v + m[3] + 2048) >> 12;
		int g = (m[4] * y + m[5] * u + m[6] * v + m[7] + 2048) >> 12;
		int b = (m[8] * y + m[9] * u + m[10] * v + m[11] + 2048) >> 12;
		out[0] = (uint8_t)std::clamp(b, 0, 255);
		out[1] = (uint8_t)std::clamp(g, 0, 255);
		out[2] = (uint8_t)std::clamp(r, 0, 255);
		out[3] = 255;
	}
};

template <enum video_format F>
void convert(const obs_source_frame *f, const YuvToRgb &conv, const int *xs, uint8_t *dst, int dw, int dh, int stride,
	     int y0, int y1)
{
	const int sh = (int)f->height;
	const uint8_t *const *d = f->data;
	const uint32_t *ls = f->linesize;
	for (int y = y0; y < y1; y++) {
		int sy = std::min(sh - 1, (int)(((int64_t)y * sh + sh / 2) / dh));
		if (f->flip)
			sy = sh - 1 - sy;
		const uint8_t *r0 = d[0] + (size_t)sy * ls[0];
		const uint8_t *r1 = nullptr, *r2 = nullptr;
		if (F == VIDEO_FORMAT_NV12 || F == VIDEO_FORMAT_P010)
			r1 = d[1] + (size_t)(sy / 2) * ls[1];
		if (F == VIDEO_FORMAT_I420 || F == VIDEO_FORMAT_I40A || F == VIDEO_FORMAT_I010) {
			r1 = d[1] + (size_t)(sy / 2) * ls[1];
			r2 = d[2] + (size_t)(sy / 2) * ls[2];
		}
		if (F == VIDEO_FORMAT_I422 || F == VIDEO_FORMAT_I42A || F == VIDEO_FORMAT_I444 || F == VIDEO_FORMAT_YUVA) {
			r1 = d[1] + (size_t)sy * ls[1];
			r2 = d[2] + (size_t)sy * ls[2];
		}
		uint8_t *out = dst + (size_t)y * stride;
		for (int x = 0; x < dw; x++, out += 4) {
			const int sx = xs[x];
			if (F == VIDEO_FORMAT_NV12) {
				conv.apply(r0[sx], r1[(sx & ~1)], r1[(sx & ~1) + 1], out);
			} else if (F == VIDEO_FORMAT_I420 || F == VIDEO_FORMAT_I40A || F == VIDEO_FORMAT_I422 ||
				   F == VIDEO_FORMAT_I42A) {
				conv.apply(r0[sx], r1[sx >> 1], r2[sx >> 1], out);
			} else if (F == VIDEO_FORMAT_I444 || F == VIDEO_FORMAT_YUVA) {
				conv.apply(r0[sx], r1[sx], r2[sx], out);
			} else if (F == VIDEO_FORMAT_YUY2) {
				const uint8_t *p = r0 + (sx >> 1) * 4;
				conv.apply(p[(sx & 1) ? 2 : 0], p[1], p[3], out);
			} else if (F == VIDEO_FORMAT_UYVY) {
				const uint8_t *p = r0 + (sx >> 1) * 4;
				conv.apply(p[(sx & 1) ? 3 : 1], p[0], p[2], out);
			} else if (F == VIDEO_FORMAT_YVYU) {
				const uint8_t *p = r0 + (sx >> 1) * 4;
				conv.apply(p[(sx & 1) ? 2 : 0], p[3], p[1], out);
			} else if (F == VIDEO_FORMAT_P010) {
				const uint16_t *yp = (const uint16_t *)r0;
				const uint16_t *uv = (const uint16_t *)r1 + (sx & ~1);
				conv.apply(yp[sx] >> 8, uv[0] >> 8, uv[1] >> 8, out);
			} else if (F == VIDEO_FORMAT_I010) {
				conv.apply(((const uint16_t *)r0)[sx] >> 2, ((const uint16_t *)r1)[sx >> 1] >> 2,
					   ((const uint16_t *)r2)[sx >> 1] >> 2, out);
			} else if (F == VIDEO_FORMAT_BGRA || F == VIDEO_FORMAT_BGRX) {
				const uint8_t *p = r0 + sx * 4;
				out[0] = p[0];
				out[1] = p[1];
				out[2] = p[2];
				out[3] = 255;
			} else if (F == VIDEO_FORMAT_RGBA) {
				const uint8_t *p = r0 + sx * 4;
				out[0] = p[2];
				out[1] = p[1];
				out[2] = p[0];
				out[3] = 255;
			} else if (F == VIDEO_FORMAT_Y800) {
				out[0] = out[1] = out[2] = r0[sx];
				out[3] = 255;
			}
		}
	}
}

} // namespace

void vision_size(int src_w, int src_h, int *w, int *h)
{
	int long_side = std::max(src_w, src_h);
	if (long_side <= kVisionLongSide) {
		*w = src_w;
		*h = src_h;
		return;
	}
	float s = (float)kVisionLongSide / (float)long_side;
	*w = std::max(2, (int)std::lround(src_w * s));
	*h = std::max(2, (int)std::lround(src_h * s));
}

bool frame_to_bgra(const obs_source_frame *f, uint8_t *dst, int dw, int dh, int stride)
{
	const int sw = (int)f->width;
	if (sw <= 0 || f->height == 0)
		return false;
	thread_local std::vector<int> xs;
	xs.resize(dw);
	for (int x = 0; x < dw; x++)
		xs[x] = std::min(sw - 1, (int)(((int64_t)x * sw + sw / 2) / dw));
	const YuvToRgb conv(f);

	// Rows split into bands converted in parallel
	constexpr int kBands = 8;
	struct Job {
		const obs_source_frame *f;
		const YuvToRgb *conv;
		const int *xs;
		uint8_t *dst;
		int dw, dh, stride;
		void (*fn)(const obs_source_frame *, const YuvToRgb &, const int *, uint8_t *, int, int, int, int, int);
	} job{f, &conv, xs.data(), dst, dw, dh, stride, nullptr};

#define BF_CASE(fmt)                 \
	case fmt:                    \
		job.fn = convert<fmt>; \
		break;
	switch (f->format) {
		BF_CASE(VIDEO_FORMAT_NV12)
		BF_CASE(VIDEO_FORMAT_I420)
		BF_CASE(VIDEO_FORMAT_I40A)
		BF_CASE(VIDEO_FORMAT_I422)
		BF_CASE(VIDEO_FORMAT_I42A)
		BF_CASE(VIDEO_FORMAT_I444)
		BF_CASE(VIDEO_FORMAT_YUVA)
		BF_CASE(VIDEO_FORMAT_YUY2)
		BF_CASE(VIDEO_FORMAT_UYVY)
		BF_CASE(VIDEO_FORMAT_YVYU)
		BF_CASE(VIDEO_FORMAT_P010)
		BF_CASE(VIDEO_FORMAT_I010)
		BF_CASE(VIDEO_FORMAT_BGRA)
		BF_CASE(VIDEO_FORMAT_BGRX)
		BF_CASE(VIDEO_FORMAT_RGBA)
		BF_CASE(VIDEO_FORMAT_Y800)
	default:
		return false;
	}
#undef BF_CASE
	dispatch_apply_f(kBands, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &job, [](void *ctx, size_t i) {
		const Job &j = *(const Job *)ctx;
		int y0 = (int)(j.dh * i / kBands), y1 = (int)(j.dh * (i + 1) / kBands);
		j.fn(j.f, *j.conv, j.xs, j.dst, j.dw, j.dh, j.stride, y0, y1);
	});
	return true;
}
