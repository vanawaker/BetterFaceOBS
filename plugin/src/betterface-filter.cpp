// BetterFace for OBS - the video filter
//
// filter_video (async sources, graphics thread, right before the frame is rendered):
//   frame -> downscaled BGRA -> FaceTracker (Vision landmarks for every tracked face)
// video_render:
//   input -> masks (feathered polygons) -> blur/smooth passes -> slim warp + background
//   protection -> output
// Sources that are not async (scenes, groups, captures) are read back from the GPU instead,
// one frame late.

#include "frame-convert.h"
#include "geometry.h"
#include "tracker.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#ifndef BETTERFACE_VERSION
#define BETTERFACE_VERSION "dev"
#endif

namespace {

constexpr int kWarpCell = 8;           // pixels between displacement grid nodes
constexpr float kSlimMax = 0.14f;      // outline moves in by this fraction of its radius at 100
constexpr uint64_t kAsyncTimeoutNs = 500000000ULL;

struct GeomVert {
	float x, y;
	uint32_t color;
};

struct Range {
	uint32_t start = 0, count = 0;
};

inline uint32_t pack(float r, float g, float b, float a)
{
	auto c = [](float v) { return (uint32_t)std::clamp((int)std::lround(v * 255.0f), 0, 255); };
	return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

inline uint32_t gray(float v) { return pack(v, v, v, v); }

// Polygon with a soft edge: value `in_c` inside the polygon offset by `inner_off`,
// fading to `out_c` at `outer_off` (offsets are along the outward normal, in pixels)
void add_feathered(std::vector<GeomVert> &out, const std::vector<Vec2> &poly, float inner_off, float outer_off,
		   uint32_t in_c, uint32_t out_c)
{
	size_t n = poly.size();
	if (n < 3)
		return;
	float area = 0;
	for (size_t i = 0; i < n; i++) {
		const Vec2 &a = poly[i], &b = poly[(i + 1) % n];
		area += a.x * b.y - b.x * a.y;
	}
	float sgn = area > 0 ? 1.0f : -1.0f;

	std::vector<Vec2> edge_n(n);
	for (size_t i = 0; i < n; i++) {
		Vec2 e = poly[(i + 1) % n] - poly[i];
		edge_n[i] = normalize(Vec2{e.y * sgn, -e.x * sgn});
	}
	std::vector<Vec2> inner(n), outer(n);
	Vec2 c;
	for (size_t i = 0; i < n; i++) {
		Vec2 na = edge_n[(i + n - 1) % n], nb = edge_n[i];
		Vec2 vn = normalize(na + nb);
		float miter = 1.0f / std::max(dot(vn, nb), 0.5f);
		inner[i] = poly[i] + vn * (inner_off * miter);
		outer[i] = poly[i] + vn * (outer_off * miter);
		c = c + inner[i];
	}
	c = c * (1.0f / (float)n);
	for (size_t i = 0; i < n; i++) {
		size_t j = (i + 1) % n;
		out.push_back({c.x, c.y, in_c});
		out.push_back({inner[i].x, inner[i].y, in_c});
		out.push_back({inner[j].x, inner[j].y, in_c});
		out.push_back({inner[i].x, inner[i].y, in_c});
		out.push_back({outer[i].x, outer[i].y, out_c});
		out.push_back({outer[j].x, outer[j].y, out_c});
		out.push_back({inner[i].x, inner[i].y, in_c});
		out.push_back({outer[j].x, outer[j].y, out_c});
		out.push_back({inner[j].x, inner[j].y, in_c});
	}
}

uint16_t to_half(float f)
{
	_Float16 h = (_Float16)f;
	uint16_t u;
	std::memcpy(&u, &h, 2);
	return u;
}

// Timing summary in the OBS log when BETTERFACE_PROFILE is set in the environment
struct Profile {
	bool on = getenv("BETTERFACE_PROFILE") != nullptr;
	uint64_t track_ns = 0, conv_ns = 0, render_ns = 0;
	double lm_ms = 0;
	int track_n = 0, render_n = 0, faces = 0;

	void report()
	{
		if (!on || render_n < 300)
			return;
		blog(LOG_INFO, "[BetterFace] avg per frame: tracking %.2f ms (convert %.2f, landmarks %.2f), render (CPU) %.2f ms, faces %d",
		     track_n ? track_ns / 1e6 / track_n : 0.0, track_n ? conv_ns / 1e6 / track_n : 0.0,
		     track_n ? lm_ms / track_n : 0.0,
		     render_ns / 1e6 / render_n, faces);
		track_ns = conv_ns = render_ns = 0;
		lm_ms = 0;
		track_n = render_n = 0;
	}
};

struct Filter {
	obs_source_t *source = nullptr;
	Profile prof;
	gs_effect_t *effect = nullptr;
	FaceTracker tracker;
	// A detected failure bypasses this instance until it is recreated. Settings updates and
	// off/on toggles must not silently restart failing work during a live stream.
	std::atomic<const char *> failure{nullptr};
#ifdef BETTERFACE_TEST_FAULTS
	std::string test_fault;
#endif

	// settings (0..1)
	float smooth = 0.6f, slim = 0.3f, whiten = 0, rosy = 0.2f, lift = 0.5f;
	bool neck = true, debug = false;
	static constexpr bool protect = false; // Temporarily disabled, including saved configurations.

	uint64_t last_async_ns = 0;
	int frame_w = 0, frame_h = 0; // size the tracker reports landmarks in

	gs_texrender_t *tr_src = nullptr, *tr_mask_a = nullptr, *tr_mask_b = nullptr, *tr_mask_c = nullptr;
	gs_texrender_t *tr_blur = nullptr, *tr_smooth = nullptr;
	gs_texrender_t *tr_down[4] = {};
	gs_texrender_t *tr_plate[2] = {};
	int plate_idx = 0;
	bool plate_cleared = false;

	gs_texture_t *warp_tex = nullptr;
	int warp_w = 0, warp_h = 0;
	std::vector<float> warp_rg;
	std::vector<uint16_t> warp_half;

	gs_texture_t *seg_tex = nullptr;
	int seg_w = 0, seg_h = 0;
	uint64_t seg_serial = 0;
	SegMask seg;

	gs_vertbuffer_t *vb = nullptr;
	size_t vb_cap = 0;
	std::vector<GeomVert> verts;

	// Per-frame pass inputs. gs_technique_end() clears every effect parameter, so each pass
	// re-applies all of them from here before drawing.
	struct Ctx {
		gs_texture_t *src = nullptr, *mask_a = nullptr, *mask_b = nullptr, *mask_c = nullptr, *blur = nullptr;
		gs_texture_t *lowfreq = nullptr, *smoothed = nullptr, *warp = nullptr, *seg = nullptr, *plate = nullptr;
		uint32_t cx = 1, cy = 1;
		int gw = 1, gh = 1;
		bool use_seg = false, use_warp = false, use_lift = false, protect = false;
	} ctx;

	// GPU readback for non-async sources
	gs_texrender_t *tr_vision = nullptr;
	gs_stagesurf_t *stage[2] = {};
	bool stage_full[2] = {};
	int stage_idx = 0, stage_w = 0, stage_h = 0;
};

bool fail_open(Filter *f, const char *reason)
{
	const char *expected = nullptr;
	if (f->failure.compare_exchange_strong(expected, reason))
		blog(LOG_ERROR, "[BetterFace] safety bypass: %s; original video only until filter recreation or OBS restart", reason);
	return false;
}

// Fault injection exists only in the separate test build, never in the released plugin.
bool test_fault(Filter *f, const char *point)
{
#ifdef BETTERFACE_TEST_FAULTS
	return f->test_fault == point;
#else
	return false;
#endif
}

struct RenderState {
	bool srgb = gs_framebuffer_srgb_enabled();
	bool active = true;
	RenderState()
	{
		gs_blend_state_push();
		gs_reset_blend_state();
		gs_enable_blending(false);
		gs_enable_framebuffer_srgb(false);
	}
	void restore()
	{
		if (!active)
			return;
		gs_enable_framebuffer_srgb(srgb);
		gs_blend_state_pop();
		active = false;
	}
	~RenderState() { restore(); }
};

void destroy_gpu(Filter *f)
{
	auto tr = [](gs_texrender_t *&t) {
		if (t)
			gs_texrender_destroy(t);
		t = nullptr;
	};
	tr(f->tr_src);
	tr(f->tr_mask_a);
	tr(f->tr_mask_b);
	tr(f->tr_mask_c);
	tr(f->tr_blur);
	tr(f->tr_smooth);
	for (auto &t : f->tr_down)
		tr(t);
	for (auto &t : f->tr_plate)
		tr(t);
	tr(f->tr_vision);
	for (auto &s : f->stage) {
		if (s)
			gs_stagesurface_destroy(s);
		s = nullptr;
	}
	if (f->warp_tex)
		gs_texture_destroy(f->warp_tex);
	f->warp_tex = nullptr;
	if (f->seg_tex)
		gs_texture_destroy(f->seg_tex);
	f->seg_tex = nullptr;
	if (f->vb)
		gs_vertexbuffer_destroy(f->vb);
	f->vb = nullptr;
	if (f->effect)
		gs_effect_destroy(f->effect);
	f->effect = nullptr;
}

gs_effect_t *load_effect()
{
	char *path = obs_module_file("betterface.effect");
	if (!path)
		return nullptr;
	// Read the file ourselves: gs_effect_create_from_file caches by path, which would keep
	// a stale shader across filter re-creation while developing
	char *text = os_quick_read_utf8_file(path);
	gs_effect_t *eff = nullptr;
	if (text) {
		char *err = nullptr;
		eff = gs_effect_create(text, path, &err);
		if (!eff)
			blog(LOG_ERROR, "[BetterFace] effect error: %s", err ? err : "(unknown)");
		bfree(err);
		bfree(text);
	}
	bfree(path);
	return eff;
}

const char *bf_get_name(void *) { return "BetterFace for OBS"; }

void bf_update(void *data, obs_data_t *s)
{
	Filter *f = (Filter *)data;
	f->smooth = (float)obs_data_get_int(s, "smooth") / 100.0f;
	f->slim = (float)obs_data_get_int(s, "slim") / 100.0f;
	f->whiten = (float)obs_data_get_int(s, "whiten") / 100.0f;
	f->rosy = (float)obs_data_get_int(s, "rosy") / 100.0f;
	f->lift = (float)obs_data_get_int(s, "lift") / 100.0f;
	f->neck = obs_data_get_bool(s, "neck");
	// Do not let an old scene collection or a websocket settings update re-enable it.
	obs_data_set_bool(s, "protect", false);
	f->debug = obs_data_get_bool(s, "debug");
#ifdef BETTERFACE_TEST_FAULTS
	f->test_fault = obs_data_get_string(s, "_test_fault");
#endif
}

void bf_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "smooth", 60);
	obs_data_set_default_int(s, "slim", 30);
	obs_data_set_default_int(s, "whiten", 0);
	obs_data_set_default_int(s, "rosy", 20);
	obs_data_set_default_int(s, "lift", 50);
	obs_data_set_default_bool(s, "neck", true);
	obs_data_set_default_bool(s, "protect", false);
	obs_data_set_default_bool(s, "debug", false);
}

obs_properties_t *bf_properties(void *data)
{
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int_slider(p, "smooth", "磨皮", 0, 100, 1);
	obs_properties_add_int_slider(p, "slim", "瘦脸（连下巴）", 0, 100, 1);
	obs_properties_add_int_slider(p, "whiten", "美白", 0, 100, 1);
	obs_properties_add_int_slider(p, "rosy", "红润", 0, 100, 1);
	obs_properties_add_int_slider(p, "lift", "淡化黑眼圈和法令纹", 0, 100, 1);
	obs_properties_add_bool(p, "neck", "脖子和耳朵也磨皮");
	obs_properties_add_text(p, "background_notice", "背景保护暂时停用，旧设置也不会启用。", OBS_TEXT_INFO);
	obs_properties_add_bool(p, "debug", "显示美颜范围（调试用）");
	obs_properties_add_text(p, "version", "BetterFace for OBS " BETTERFACE_VERSION, OBS_TEXT_INFO);
	Filter *f = (Filter *)data;
	if (f && f->failure.load())
		obs_properties_add_text(p, "safety_notice", "美颜已因故障停用，当前输出原画。请在直播结束后重启 OBS。", OBS_TEXT_INFO);
	return p;
}

void *bf_create(obs_data_t *settings, obs_source_t *source)
{
	Filter *f = new Filter;
	f->source = source;
	obs_enter_graphics();
	f->effect = load_effect();
	obs_leave_graphics();
	bf_update(f, settings);
	return f;
}

void bf_destroy(void *data)
{
	Filter *f = (Filter *)data;
	obs_enter_graphics();
	destroy_gpu(f);
	obs_leave_graphics();
	delete f;
}

bool want_segmentation(const Filter *f) { return (f->protect && f->slim > 0.0f) || f->neck; }

obs_source_frame *bf_filter_video(void *data, obs_source_frame *frame)
{
	Filter *f = (Filter *)data;
	if (!frame || f->failure.load())
		return frame;
	if (!frame->width || !frame->height)
		return frame;
	uint64_t t0 = os_gettime_ns();
	f->last_async_ns = t0;
	int vw, vh;
	vision_size((int)frame->width, (int)frame->height, &vw, &vh);
	int stride = 0;
	uint8_t *buf = f->tracker.begin_frame(vw, vh, &stride);
	bool ok = buf && !test_fault(f, "conversion") && frame_to_bgra(frame, buf, vw, vh, stride);
	if (!ok) {
		fail_open(f, "frame conversion or tracking buffer unavailable");
		return frame;
	}
	f->prof.conv_ns += os_gettime_ns() - t0;
	if (ok) {
		f->frame_w = (int)frame->width;
		f->frame_h = (int)frame->height;
		f->tracker.end_frame((float)frame->width, (float)frame->height, (double)os_gettime_ns() / 1e9,
				     want_segmentation(f));
	}
	f->prof.track_ns += os_gettime_ns() - t0;
	f->prof.lm_ms += f->tracker.last_landmark_ms;
	f->prof.track_n++;
	f->prof.faces = (int)f->tracker.faces().size();
	return frame;
}

gs_texrender_t *ensure_tr(gs_texrender_t *&t, gs_color_format fmt)
{
	if (t && gs_texrender_get_format(t) != fmt) {
		gs_texrender_destroy(t);
		t = nullptr;
	}
	if (!t)
		t = gs_texrender_create(fmt, GS_ZS_NONE);
	return t;
}

bool render_input(Filter *f, obs_source_t *target, obs_source_t *parent, uint32_t cx, uint32_t cy)
{
	if (!ensure_tr(f->tr_src, GS_RGBA) || test_fault(f, "input"))
		return false;
	gs_texrender_reset(f->tr_src);
	if (!gs_texrender_begin_with_color_space(f->tr_src, cx, cy, GS_CS_SRGB))
		return false;
	uint32_t parent_flags = obs_source_get_output_flags(parent);
	bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
	vec4 clear;
	vec4_zero(&clear);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);
	gs_blend_state_pop();
	gs_texrender_end(f->tr_src);
	return gs_texrender_get_texture(f->tr_src) != nullptr;
}

void apply_ctx(Filter *f);

bool draw_fullscreen(Filter *f, const char *tech, gs_texture_t *tex, uint32_t cx, uint32_t cy)
{
	gs_technique_t *t = gs_effect_get_technique(f->effect, tech);
	if (!t || test_fault(f, "technique"))
		return false;
	size_t passes = gs_technique_begin(t);
	bool ok = passes != 0;
	for (size_t i = 0; i < passes; i++) {
		if (test_fault(f, "pass_begin") || !gs_technique_begin_pass(t, i)) {
			ok = false;
			break;
		}
		gs_draw_sprite(tex, 0, cx, cy);
		gs_technique_end_pass(t);
	}
	gs_technique_end(t);
	return ok;
}

// Fullscreen pass into a texrender with the given technique
template <typename SetParams>
bool run_pass(Filter *f, gs_texrender_t *tr, uint32_t w, uint32_t h, uint32_t cx, uint32_t cy, const char *tech,
	      SetParams set)
{
	if (!tr || test_fault(f, tech))
		return false;
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	apply_ctx(f);
	set();
	bool ok = draw_fullscreen(f, tech, nullptr, cx, cy);
	gs_texrender_end(tr);
	return ok && gs_texrender_get_texture(tr) != nullptr;
}

void set_tex(Filter *f, const char *name, gs_texture_t *tex)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(f->effect, name);
	if (p)
		gs_effect_set_texture(p, tex);
}

void set_float(Filter *f, const char *name, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(f->effect, name);
	if (p)
		gs_effect_set_float(p, v);
}

void set_bool(Filter *f, const char *name, bool v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(f->effect, name);
	if (p)
		gs_effect_set_bool(p, v);
}

void set_vec2(Filter *f, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(f->effect, name);
	if (p) {
		vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

void apply_ctx(Filter *f)
{
	const Filter::Ctx &c = f->ctx;
	auto tex = [&](gs_texture_t *t) { return t ? t : c.src; };
	set_tex(f, "image", c.src);
	set_tex(f, "mask_a", tex(c.mask_a));
	set_tex(f, "mask_b", tex(c.mask_b));
	set_tex(f, "mask_c", tex(c.mask_c));
	set_tex(f, "blur_tex", tex(c.blur));
	set_tex(f, "lowfreq", tex(c.lowfreq));
	set_tex(f, "smooth_tex", tex(c.smoothed));
	set_tex(f, "warp_tex", tex(c.warp));
	set_tex(f, "seg_tex", tex(c.seg));
	set_tex(f, "plate_tex", tex(c.plate));
	set_vec2(f, "texel", 1.0f / (float)c.cx, 1.0f / (float)c.cy);
	set_vec2(f, "src_size", (float)c.cx, (float)c.cy);
	set_vec2(f, "warp_scale", 1.0f / (float)(kWarpCell * c.gw), 1.0f / (float)(kWarpCell * c.gh));
	set_vec2(f, "warp_offset", 0.5f / (float)c.gw, 0.5f / (float)c.gh);
	set_float(f, "smooth_amt", f->smooth);
	set_float(f, "whiten_amt", f->whiten);
	set_float(f, "rosy_amt", f->rosy);
	set_float(f, "lift_amt", f->lift);
	set_float(f, "plate_rate", 0.15f);
	set_bool(f, "use_seg", c.use_seg);
	set_bool(f, "use_warp", c.use_warp);
	set_bool(f, "use_lift", c.use_lift);
	set_bool(f, "protect_bg", c.protect);
	set_bool(f, "debug_on", f->debug);
}

// Read back the input for sources whose frames never pass through filter_video
bool track_from_gpu(Filter *f, gs_texture_t *src, uint32_t cx, uint32_t cy)
{
	int vw, vh;
	vision_size((int)cx, (int)cy, &vw, &vh);
	if (vw != f->stage_w || vh != f->stage_h) {
		for (auto &s : f->stage) {
			if (s)
				gs_stagesurface_destroy(s);
			s = test_fault(f, "stage_create") ? nullptr : gs_stagesurface_create(vw, vh, GS_BGRA);
			if (!s)
				return false;
		}
		f->stage_full[0] = f->stage_full[1] = false;
		f->stage_w = vw;
		f->stage_h = vh;
	}
	if (!ensure_tr(f->tr_vision, GS_BGRA))
		return false;

	// Map last frame's copy (one frame late, so the GPU never stalls)
	int prev = f->stage_idx ^ 1;
	if (f->stage_full[prev]) {
		uint8_t *data;
		uint32_t linesize;
		if (!test_fault(f, "stage_map") && gs_stagesurface_map(f->stage[prev], &data, &linesize)) {
			int stride = 0;
			uint8_t *buf = f->tracker.begin_frame(vw, vh, &stride);
			bool valid = buf && stride >= vw * 4 && data && linesize >= (uint32_t)vw * 4;
			if (valid) {
				for (int y = 0; y < vh; y++)
					std::memcpy(buf + (size_t)y * stride, data + (size_t)y * linesize, (size_t)vw * 4);
				f->frame_w = (int)cx;
				f->frame_h = (int)cy;
				f->tracker.end_frame((float)cx, (float)cy, (double)os_gettime_ns() / 1e9,
						     want_segmentation(f));
			}
			gs_stagesurface_unmap(f->stage[prev]);
			if (!valid)
				return false;
		} else {
			return false;
		}
		f->stage_full[prev] = false;
	}

	if (!run_pass(f, f->tr_vision, vw, vh, cx, cy, "Draw", [&] { set_tex(f, "image", src); }))
		return false;
	gs_stage_texture(f->stage[f->stage_idx], gs_texrender_get_texture(f->tr_vision));
	f->stage_full[f->stage_idx] = true;
	f->stage_idx ^= 1;
	return true;
}

void draw_range(const Range &r)
{
	if (r.count)
		gs_draw(GS_TRIS, r.start, r.count);
}

bool render_frame(Filter *f);

void bf_render(void *data, gs_effect_t *)
{
	Filter *f = (Filter *)data;
	uint64_t t0 = os_gettime_ns();
	// OBS_SOURCE_SRGB keeps OBS from changing the caller's linear-sRGB state before
	// entering this callback. Preserve the original beauty pipeline's non-linear mode,
	// then restore the caller's mode *before* a bypass so raw video matches a disabled filter.
	const bool previous_linear = gs_get_linear_srgb();
	gs_set_linear_srgb(false);
	bool rendered = render_frame(f);
	gs_set_linear_srgb(previous_linear);
	// render_frame has unwound all temporary render state before OBS draws the source.
	if (!rendered)
		obs_source_skip_video_filter(f->source);
	f->prof.render_ns += os_gettime_ns() - t0;
	f->prof.render_n++;
	f->prof.report();
}

bool render_frame(Filter *f)
{
	if (f->failure.load())
		return false;
	obs_source_t *target = obs_filter_get_target(f->source);
	obs_source_t *parent = obs_filter_get_parent(f->source);
	if (!f->effect)
		return fail_open(f, "beauty shader unavailable");
	if (!target || !parent)
		return false;
	uint32_t cx = obs_source_get_base_width(target);
	uint32_t cy = obs_source_get_base_height(target);
	if (!cx || !cy)
		return false;

	// Async sources (cameras, media) deliver frames through filter_video even when they pause;
	// anything else is read back from the GPU
	bool async_frames = (obs_source_get_output_flags(parent) & OBS_SOURCE_ASYNC) != 0 ||
			    os_gettime_ns() - f->last_async_ns < kAsyncTimeoutNs;
	if (async_frames && f->tracker.faces().empty() && !f->debug)
		return false;
	if (!render_input(f, target, parent, cx, cy))
		return fail_open(f, "input render target unavailable");
	gs_texture_t *src = gs_texrender_get_texture(f->tr_src);
	f->ctx = Filter::Ctx{};
	f->ctx.src = src;
	f->ctx.cx = cx;
	f->ctx.cy = cy;

	RenderState render_state;

	if (!async_frames && !track_from_gpu(f, src, cx, cy))
		return fail_open(f, "GPU tracking readback unavailable");

	// Face shapes in source pixels
	const std::vector<FaceInfo> &faces = f->tracker.faces();
	float sx = f->frame_w > 0 ? (float)cx / (float)f->frame_w : 1.0f;
	float sy = f->frame_h > 0 ? (float)cy / (float)f->frame_h : 1.0f;
	std::vector<FaceShape> shapes;
	std::vector<const FaceInfo *> infos;
	for (const FaceInfo &fi : faces) {
		Landmarks lm = fi.lm;
		for (Vec2 &p : lm.p) {
			p.x *= sx;
			p.y *= sy;
		}
		FaceShape s;
		if (build_face_shape(lm, s)) {
			shapes.push_back(s);
			infos.push_back(&fi);
		}
	}

	if (shapes.empty() && !f->debug)
		return false;

	// Mask geometry
	f->verts.clear();
	Range r_core, r_cand, r_lift, r_radius, r_excl, r_model;
	auto begin = [&](Range &r) { r.start = (uint32_t)f->verts.size(); };
	auto end = [&](Range &r) { r.count = (uint32_t)f->verts.size() - r.start; };

	begin(r_core);
	for (size_t i = 0; i < shapes.size(); i++) {
		const FaceShape &s = shapes[i];
		float pr = infos[i]->presence;
		add_feathered(f->verts, s.core, -0.05f * s.fw, 0.015f * s.fw, gray(pr), gray(0));
	}
	end(r_core);

	begin(r_cand);
	for (size_t i = 0; i < shapes.size(); i++) {
		const FaceShape &s = shapes[i];
		float pr = infos[i]->presence;
		add_feathered(f->verts, s.forehead, -0.04f * s.fw, 0.03f * s.fw, gray(pr), gray(0));
		if (f->neck) {
			add_feathered(f->verts, s.neck, -0.04f * s.fw, 0.04f * s.fw, gray(pr), gray(0));
			add_feathered(f->verts, s.ear_l, -0.02f * s.fw, 0.04f * s.fw, gray(pr), gray(0));
			add_feathered(f->verts, s.ear_r, -0.02f * s.fw, 0.04f * s.fw, gray(pr), gray(0));
		}
	}
	end(r_cand);

	begin(r_lift);
	if (f->lift > 0.0f) {
		for (size_t i = 0; i < shapes.size(); i++) {
			const FaceShape &s = shapes[i];
			float pr = infos[i]->presence;
			add_feathered(f->verts, s.under_eye_l, -0.01f * s.fw, 0.04f * s.fw, gray(pr), gray(0));
			add_feathered(f->verts, s.under_eye_r, -0.01f * s.fw, 0.04f * s.fw, gray(pr), gray(0));
			add_feathered(f->verts, s.fold_l, -0.01f * s.fw, 0.03f * s.fw, gray(pr), gray(0));
			add_feathered(f->verts, s.fold_r, -0.01f * s.fw, 0.03f * s.fw, gray(pr), gray(0));
		}
	}
	end(r_lift);

	// Per-face values that must not fade at the edges: blur radius and skin model
	auto add_flat_regions = [&](const FaceShape &s, uint32_t color) {
		float m = 0.06f * s.fw;
		add_feathered(f->verts, s.core, 0, m, color, color);
		add_feathered(f->verts, s.forehead, 0, m, color, color);
		add_feathered(f->verts, s.neck, 0, m, color, color);
		add_feathered(f->verts, s.ear_l, 0, m, color, color);
		add_feathered(f->verts, s.ear_r, 0, m, color, color);
	};
	begin(r_radius);
	for (const FaceShape &s : shapes)
		add_flat_regions(s, gray(std::min(s.blur_radius / 64.0f, 1.0f)));
	end(r_radius);

	begin(r_excl);
	for (const FaceShape &s : shapes) {
		auto eye = [&](const std::vector<Vec2> &e) {
			float ew = 0;
			for (const Vec2 &p : e)
				for (const Vec2 &q : e)
					ew = std::max(ew, length(p - q));
			add_feathered(f->verts, e, 0.12f * ew + 1.0f, 0.4f * ew + 2.0f, gray(0), gray(1));
		};
		eye(s.eye_l);
		eye(s.eye_r);
		add_feathered(f->verts, s.brow_l, 0.02f * s.fw, 0.06f * s.fw, gray(0), gray(1));
		add_feathered(f->verts, s.brow_r, 0.02f * s.fw, 0.06f * s.fw, gray(0), gray(1));
		add_feathered(f->verts, s.lips, 0.005f * s.fw, 0.03f * s.fw, gray(0), gray(1));
	}
	end(r_excl);

	begin(r_model);
	for (size_t i = 0; i < shapes.size(); i++) {
		const SkinModel &m = infos[i]->skin;
		uint32_t c = m.valid ? pack(m.cb + 0.5f, m.cr + 0.5f, m.sigma * 4.0f, m.luma) : pack(0.5f, 0.5f, 0, 0);
		add_flat_regions(shapes[i], c);
	}
	end(r_model);

	// Upload geometry
	if (!f->vb || f->vb_cap < f->verts.size()) {
		if (f->vb)
			gs_vertexbuffer_destroy(f->vb);
		f->vb = nullptr;
		f->vb_cap = 0;
		size_t cap = std::max<size_t>(16384, f->verts.size() * 2);
		gs_vb_data *vbd = gs_vbdata_create();
		if (!vbd)
			return fail_open(f, "geometry data allocation failed");
		vbd->num = cap;
		vbd->points = (vec3 *)bzalloc(sizeof(vec3) * cap);
		vbd->colors = (uint32_t *)bzalloc(sizeof(uint32_t) * cap);
		if (!vbd->points || !vbd->colors) {
			gs_vbdata_destroy(vbd);
			return fail_open(f, "geometry storage allocation failed");
		}
		// The graphics backend consumes vbd even when buffer creation fails.
		if (test_fault(f, "vb_create"))
			gs_vbdata_destroy(vbd);
		else
			f->vb = gs_vertexbuffer_create(vbd, GS_DYNAMIC);
		if (!f->vb)
			return fail_open(f, "GPU vertex buffer creation failed");
		f->vb_cap = cap;
	}
	{
		gs_vb_data *vbd = test_fault(f, "vb_data") ? nullptr : gs_vertexbuffer_get_data(f->vb);
		if (!vbd || !vbd->points || !vbd->colors || vbd->num < f->verts.size())
			return fail_open(f, "GPU vertex buffer data unavailable");
		for (size_t i = 0; i < f->verts.size(); i++) {
			vec3_set(&vbd->points[i], f->verts[i].x, f->verts[i].y, 0.0f);
			vbd->colors[i] = f->verts[i].color;
		}
		gs_vertexbuffer_flush(f->vb);
	}

	uint32_t mw = std::max(1u, cx / 2), mh = std::max(1u, cy / 2);
	ensure_tr(f->tr_mask_a, GS_RGBA);
	ensure_tr(f->tr_mask_b, GS_RGBA);
	ensure_tr(f->tr_mask_c, GS_RGBA);
	gs_technique_t *geom = gs_effect_get_technique(f->effect, "Geom");
	auto geom_pass = [&](gs_texrender_t *tr, float clear_to, auto draw) {
		if (!tr || !geom || test_fault(f, "mask"))
			return false;
		gs_texrender_reset(tr);
		if (!gs_texrender_begin(tr, mw, mh))
			return false;
		vec4 clear;
		vec4_set(&clear, clear_to, clear_to, clear_to, clear_to);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_load_vertexbuffer(f->vb);
		gs_load_indexbuffer(nullptr);
		size_t passes = gs_technique_begin(geom);
		bool ok = passes != 0;
		for (size_t i = 0; i < passes; i++) {
			if (test_fault(f, "geom_pass") || !gs_technique_begin_pass(geom, i)) {
				ok = false;
				break;
			}
			draw();
			gs_technique_end_pass(geom);
		}
		gs_technique_end(geom);
		gs_load_vertexbuffer(nullptr);
		gs_texrender_end(tr);
		return ok && gs_texrender_get_texture(tr) != nullptr;
	};
	if (!geom_pass(f->tr_mask_a, 0.0f, [&] {
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		gs_blend_op(GS_BLEND_OP_MAX);
		gs_enable_color(true, false, false, false);
		draw_range(r_core);
		gs_enable_color(false, true, false, false);
		draw_range(r_cand);
		gs_enable_color(false, false, true, false);
		draw_range(r_lift);
		gs_enable_color(false, false, false, true);
		draw_range(r_radius);
		gs_enable_color(true, true, true, true);
		gs_blend_op(GS_BLEND_OP_ADD);
		gs_enable_blending(false);
	}))
		return fail_open(f, "face mask rendering failed");
	if (!geom_pass(f->tr_mask_b, 0.0f, [&] { draw_range(r_model); }))
		return fail_open(f, "skin model rendering failed");
	// Eyes, brows and lips: kept out of smoothing and the lift only. Whitening and rosy cover the
	// whole face; with holes there, every landmark wobble showed as a dark ring moving around them.
	if (!geom_pass(f->tr_mask_c, 1.0f, [&] {
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		gs_blend_op(GS_BLEND_OP_MIN);
		draw_range(r_excl);
		gs_blend_op(GS_BLEND_OP_ADD);
		gs_enable_blending(false);
	}))
		return fail_open(f, "exclusion mask rendering failed");
	gs_texture_t *mask_a = gs_texrender_get_texture(f->tr_mask_a);
	gs_texture_t *mask_b = gs_texrender_get_texture(f->tr_mask_b);
	gs_texture_t *mask_c = gs_texrender_get_texture(f->tr_mask_c);

	// Segmentation mask
	bool seg_ok = false;
	if (want_segmentation(f)) {
		if (f->tracker.latest_seg(f->seg, f->seg_serial)) {
			if (!f->seg_tex || f->seg_w != f->seg.w || f->seg_h != f->seg.h) {
				if (f->seg_tex)
					gs_texture_destroy(f->seg_tex);
				f->seg_tex = test_fault(f, "seg_texture") ? nullptr :
					gs_texture_create(f->seg.w, f->seg.h, GS_R8, 1, nullptr, GS_DYNAMIC);
				if (!f->seg_tex)
					return fail_open(f, "segmentation texture creation failed");
				f->seg_w = f->seg.w;
				f->seg_h = f->seg.h;
			}
			if (f->seg_tex)
				gs_texture_set_image(f->seg_tex, f->seg.data.data(), f->seg.w, false);
			f->seg_serial = f->seg.serial;
		}
		seg_ok = f->seg_tex != nullptr;
	}

	// Slim warp grid
	bool use_warp = f->slim > 0.0f && !shapes.empty();
	int gw = (int)(cx + kWarpCell - 1) / kWarpCell + 1;
	int gh = (int)(cy + kWarpCell - 1) / kWarpCell + 1;
	if (use_warp) {
		std::vector<float> k;
		for (const FaceInfo *fi : infos)
			k.push_back(f->slim * kSlimMax * fi->presence);
		build_warp_grid(shapes, k, (int)cx, (int)cy, kWarpCell, gw, gh, f->warp_rg);
		f->warp_half.resize((size_t)gw * gh * 4);
		for (size_t i = 0; i < (size_t)gw * gh; i++) {
			f->warp_half[i * 4 + 0] = to_half(f->warp_rg[i * 2 + 0]);
			f->warp_half[i * 4 + 1] = to_half(f->warp_rg[i * 2 + 1]);
			f->warp_half[i * 4 + 2] = 0;
			f->warp_half[i * 4 + 3] = 0;
		}
		if (!f->warp_tex || f->warp_w != gw || f->warp_h != gh) {
			if (f->warp_tex)
				gs_texture_destroy(f->warp_tex);
			f->warp_tex = test_fault(f, "warp_texture") ? nullptr :
				gs_texture_create(gw, gh, GS_RGBA16F, 1, nullptr, GS_DYNAMIC);
			f->warp_w = gw;
			f->warp_h = gh;
		}
		if (!f->warp_tex)
			return fail_open(f, "slimming texture creation failed");
		gs_texture_set_image(f->warp_tex, (const uint8_t *)f->warp_half.data(), gw * 8, false);
	}

	Filter::Ctx &c = f->ctx;
	c.mask_a = mask_a;
	c.mask_b = mask_b;
	c.mask_c = mask_c;
	c.seg = seg_ok ? f->seg_tex : nullptr;
	c.use_seg = seg_ok;
	c.gw = gw;
	c.gh = gh;
	c.use_warp = use_warp;
	c.warp = use_warp ? f->warp_tex : nullptr;

	// Horizontal blur
	ensure_tr(f->tr_blur, GS_RGBA16F);
	if (!run_pass(f, f->tr_blur, cx, cy, cx, cy, "BlurH", [] {}))
		return fail_open(f, "horizontal blur rendering failed");
	c.blur = gs_texrender_get_texture(f->tr_blur);

	// Low-frequency image for the lift
	c.use_lift = f->lift > 0.0f;
	if (c.use_lift) {
		gs_texture_t *in = src;
		uint32_t w = cx, h = cy;
		for (auto &t : f->tr_down) {
			uint32_t iw = w, ih = h;
			w = std::max(1u, w / 2);
			h = std::max(1u, h / 2);
			ensure_tr(t, GS_RGBA);
			if (!run_pass(f, t, w, h, cx, cy, "Down", [&] {
				set_tex(f, "image", in);
				set_vec2(f, "texel", 1.0f / (float)iw, 1.0f / (float)ih);
			}))
				return fail_open(f, "low frequency rendering failed");
			in = gs_texrender_get_texture(t);
		}
		c.lowfreq = in;
	}

	// Vertical blur + smoothing
	ensure_tr(f->tr_smooth, GS_RGBA16F);
	if (!run_pass(f, f->tr_smooth, cx, cy, cx, cy, "Smooth", [] {}))
		return fail_open(f, "smoothing rendering failed");
	c.smoothed = gs_texrender_get_texture(f->tr_smooth);

	// Background protection is disabled for this release, even with legacy settings.
	c.protect = false;

	render_state.restore();

	// Final composite into the parent's render target
	{
		const bool prev2 = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		apply_ctx(f);
		bool ok = !test_fault(f, "Final") && draw_fullscreen(f, "Final", src, cx, cy);
		gs_enable_framebuffer_srgb(prev2);
		if (!ok)
			return fail_open(f, "final composite rendering failed");
	}
	return true;
}

enum gs_color_space bf_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	Filter *f = (Filter *)data;
	if (f->failure.load()) {
		obs_source_t *target = obs_filter_get_target(f->source);
		if (target)
			return obs_source_get_color_space(target, count, preferred);
	}
	return GS_CS_SRGB;
}

} // namespace

void register_betterface_filter()
{
	obs_source_info info = {};
	info.id = "betterface_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = bf_get_name;
	info.create = bf_create;
	info.destroy = bf_destroy;
	info.update = bf_update;
	info.get_defaults = bf_defaults;
	info.get_properties = bf_properties;
	info.filter_video = bf_filter_video;
	info.video_render = bf_render;
	info.video_get_color_space = bf_color_space;
	obs_register_source(&info);
}
