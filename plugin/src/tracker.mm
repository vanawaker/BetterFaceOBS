#include "tracker.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#include <util/base.h>

namespace {

constexpr int kDetectEvery = 3;       // frames between background face detections
constexpr int kMissedDetections = 3;  // detections without a match before a face is dropped
constexpr int kLandmarkFailures = 4;  // consecutive bad landmark frames before a face is dropped
constexpr float kFadeSeconds = 0.25f;
constexpr float kMinLandmarkConfidence = 0.2f;
constexpr int kMaxFaces = 6;

// Vision fits the face into the box it is given: a box that is too small squeezes all landmarks
// into it, and a box that follows those landmarks then shrinks every frame until it collapses
// (seen with a hand over the mouth). A box that is too large is harmless, so detections are enlarged
// and the box never gets smaller than the last detection.
constexpr float kBoxScale = 1.1f;
constexpr float kMinBoxOfDetection = 0.85f;
// Every distinct input box gives landmarks that differ by ~0.5 % of the face width (more on the
// contour), so the box moves only for real motion, and each move is made bumpless (see Track::corr).
constexpr float kBoxMoveTol = 0.04f;  // fraction of the box width
constexpr float kBoxSizeTol = 0.08f;
constexpr float kCorrSeconds = 0.5f;  // time constant of the bumpless correction
// Mean per-point precision estimate from Vision: ~0.008 on a clear face, ~0.02 when a hand covers
// the mouth, 0.03 and more on a box with no face in it (Vision's confidence stays ~0.7 there, so it
// cannot tell). Between clear and covered, the last clear face shape is blended in, moved rigidly
// with the head. A covered face survives detector misses for a while: the detector often loses it.
constexpr float kSigmaClear = 0.013f, kSigmaOccluded = 0.02f, kSigmaWeak = 0.024f, kSigmaNoFace = 0.028f;
constexpr int kMissedDetectionsCovered = 15;

// One Euro filter (Casiez et al.) in pixels. Only the speed that opens the cutoff is normalized
// by face size, so near and far faces get the same smoothing.
struct OneEuro {
	float min_cutoff = 1.0f, beta = 4.0f, d_cutoff = 1.0f;
	float x_prev = 0, dx_prev = 0;
	bool init = false;

	static float alpha(float cutoff, float dt)
	{
		float tau = 1.0f / (2.0f * 3.14159265f * cutoff);
		return 1.0f / (1.0f + tau / dt);
	}

	float filter(float x, float dt, float size, float cutoff_scale)
	{
		if (!init || dt <= 0.0f) {
			init = true;
			x_prev = x;
			dx_prev = 0;
			return x;
		}
		float dx = (x - x_prev) / dt;
		float ad = alpha(d_cutoff, dt);
		dx_prev = dx_prev + ad * (dx - dx_prev);
		float a = alpha((min_cutoff + beta * std::fabs(dx_prev) / size) * cutoff_scale, dt);
		x_prev = x_prev + a * (x - x_prev);
		return x_prev;
	}
};

struct Track {
	int id = 0;
	CGRect box = CGRectZero; // Vision normalized coordinates, origin bottom-left
	CGRect prev_box = CGRectZero;
	bool box_changed = false; // next frame also runs prev_box to measure the jump
	float det_w = 0;          // last matched detection width (normalized, enlarged)
	bool rel_valid = false;
	int anchors = 0; // times the box was re-anchored to a detection (BETTERFACE_DUMP)
	float rx = 0, ry = 0, rw = 1, rh = 1; // box relative to the landmark extents
	Landmarks smooth;
	Landmarks raw;          // last Vision result, for BETTERFACE_DUMP
	bool raw_new = false;
	bool has_lm = false;
	// Landmarks jump when the input box changes. The jump is measured by running the old box once
	// more on the same frame, subtracted, and faded out over kCorrSeconds.
	Landmarks corr;
	// Last clear shape, used while the face is partly covered
	Landmarks ref;
	bool ref_valid = false;
	float sigma = 0;
	float clarity = 1; // 1 = clear face, 0 = covered (from sigma, smoothed)
	OneEuro filt[kNumPoints * 2];
	float presence = 0;
	bool alive = true;
	int missed_dets = 0;
	int lm_fail = 0;
	SkinModel skin;
};

float iou(CGRect a, CGRect b)
{
	CGRect i = CGRectIntersection(a, b);
	if (CGRectIsNull(i))
		return 0;
	float inter = (float)(i.size.width * i.size.height);
	float uni = (float)(a.size.width * a.size.height + b.size.width * b.size.height) - inter;
	return uni > 0 ? inter / uni : 0;
}

CGRect scaled(CGRect r, float s)
{
	float w = (float)r.size.width * s, h = (float)r.size.height * s;
	return CGRectMake(CGRectGetMidX(r) - w * 0.5f, CGRectGetMidY(r) - h * 0.5f, w, h);
}

// Detection d belongs to box b: overlapping, or centers close relative to the larger box
bool same_face(CGRect d, CGRect b)
{
	if (iou(d, b) > 0.2f)
		return true;
	float dx = (float)(CGRectGetMidX(d) - CGRectGetMidX(b)), dy = (float)(CGRectGetMidY(d) - CGRectGetMidY(b));
	float s = (float)std::max(d.size.width, b.size.width);
	return std::hypot(dx, dy) < 0.35f * s;
}

// Least-squares similarity transform (scale, rotation, translation) taking a onto b
struct Similarity {
	Vec2 ca, cb;
	float c = 1, s = 0;
	Vec2 apply(Vec2 p) const
	{
		Vec2 d = p - ca;
		return cb + Vec2{c * d.x - s * d.y, s * d.x + c * d.y};
	}
};

Similarity fit_similarity(const Landmarks &a, const Landmarks &b)
{
	Similarity t;
	for (int k = 0; k < kNumPoints; k++) {
		t.ca = t.ca + a.p[k];
		t.cb = t.cb + b.p[k];
	}
	t.ca = t.ca * (1.0f / kNumPoints);
	t.cb = t.cb * (1.0f / kNumPoints);
	float num_c = 0, num_s = 0, den = 0;
	for (int k = 0; k < kNumPoints; k++) {
		Vec2 p = a.p[k] - t.ca, q = b.p[k] - t.cb;
		num_c += p.x * q.x + p.y * q.y;
		num_s += p.x * q.y - p.y * q.x;
		den += dot(p, p);
	}
	if (den > 1e-6f) {
		t.c = num_c / den;
		t.s = num_s / den;
	}
	return t;
}

bool read_region(VNFaceLandmarkRegion2D *region, int expected, CGSize size, Vec2 *out)
{
	if (!region || (int)region.pointCount != expected)
		return false;
	const CGPoint *pts = [region pointsInImageOfSize:size];
	for (int i = 0; i < expected; i++)
		out[i] = {(float)pts[i].x, (float)(size.height - pts[i].y)};
	return true;
}

// sigma: mean of Vision's per-point precision estimates (larger = less certain)
bool read_landmarks(VNFaceObservation *obs, float w, float h, Landmarks &lm, float *sigma = nullptr)
{
	VNFaceLandmarks2D *l = obs.landmarks;
	if (!l || l.confidence < kMinLandmarkConfidence)
		return false;
	CGSize size = CGSizeMake(w, h);
	VNFaceLandmarkRegion2D *regions[R_COUNT] = {l.faceContour, l.leftEye,  l.rightEye,  l.leftEyebrow,
						    l.rightEyebrow, l.nose,    l.noseCrest, l.medianLine,
						    l.outerLips,    l.innerLips, l.leftPupil, l.rightPupil};
	for (int r = 0; r < R_COUNT; r++)
		if (!read_region(regions[r], kRegionSize[r], size, lm.p + region_offset(r)))
			return false;
	if (sigma) {
		NSArray<NSNumber *> *prec = l.allPoints.precisionEstimatesPerPoint;
		float sum = 0;
		for (NSNumber *v in prec)
			sum += v.floatValue;
		*sigma = prec.count ? sum / (float)prec.count : 0.0f;
	}
	return true;
}

} // namespace

struct FaceTracker::Impl {
	CVPixelBufferPoolRef pool = nullptr;
	int pool_w = 0, pool_h = 0;
	CVPixelBufferRef cur = nullptr;
	bool cur_locked = false;

	dispatch_queue_t det_q;
	dispatch_queue_t seg_q;
	std::mutex mu;

	bool det_busy = false;
	uint64_t det_serial = 0, det_used = 0;
	std::vector<CGRect> det_boxes;
	int frames_since_det = kDetectEvery;

	bool seg_busy = false;
	SegMask seg;

	std::vector<Track> tracks;
	int next_id = 1;
	double last_t = -1;

	// BETTERFACE_DUMP=<file>: one CSV line per face per frame (raw and smoothed landmarks)
	FILE *dump = nullptr;
	uint64_t frame_no = 0;

	Impl()
	{
		det_q = dispatch_queue_create("betterface.detect", DISPATCH_QUEUE_SERIAL);
		seg_q = dispatch_queue_create("betterface.segment", DISPATCH_QUEUE_SERIAL);
		static int instance = 0;
		if (const char *path = getenv("BETTERFACE_DUMP")) {
			char name[1024];
			snprintf(name, sizeof(name), "%s.%d.csv", path, ++instance);
			dump = fopen(name, "w");
		}
	}

	~Impl()
	{
		dispatch_sync(det_q, ^{
				      });
		dispatch_sync(seg_q, ^{
				      });
		if (cur)
			CVPixelBufferRelease(cur);
		if (pool)
			CVPixelBufferPoolRelease(pool);
		if (dump)
			fclose(dump);
	}

	void write_dump(double now)
	{
		frame_no++;
		for (const Track &t : tracks) {
			fprintf(dump, "%llu,%.4f,%d,%d,%.3f,%d,%.5f,%.5f,%.5f,%.5f,%d", (unsigned long long)frame_no, now,
				t.id, t.alive ? 1 : 0, t.presence, t.raw_new ? 1 : 0, t.box.origin.x, t.box.origin.y,
				t.box.size.width, t.box.size.height, t.anchors);
			for (int k = 0; k < kNumPoints; k++)
				fprintf(dump, ",%.2f,%.2f,%.2f,%.2f", t.raw.p[k].x, t.raw.p[k].y, t.smooth.p[k].x,
					t.smooth.p[k].y);
			float corr = 0;
			for (int k = 0; k < kNumPoints; k++)
				corr += length(t.corr.p[k]);
			fprintf(dump, ",%.4f,%.3f,%.2f\n", t.sigma, t.clarity, corr / kNumPoints);
		}
	}

	bool ensure_pool(int w, int h)
	{
		if (pool && pool_w == w && pool_h == h)
			return true;
		if (pool)
			CVPixelBufferPoolRelease(pool);
		pool = nullptr;
		NSDictionary *attrs = @{
			(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
			(id)kCVPixelBufferWidthKey: @(w),
			(id)kCVPixelBufferHeightKey: @(h),
			(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		};
		if (CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attrs, &pool) != kCVReturnSuccess)
			return false;
		pool_w = w;
		pool_h = h;
		return true;
	}

	void merge_detections()
	{
		std::vector<CGRect> boxes;
		{
			std::lock_guard<std::mutex> lock(mu);
			if (det_serial == det_used)
				return;
			det_used = det_serial;
			boxes = det_boxes;
		}
		std::vector<bool> matched(tracks.size(), false);
		for (CGRect raw_d : boxes) {
			CGRect d = scaled(raw_d, kBoxScale);
			// Prefer live tracks; a fading one is revived rather than replaced by a new track,
			// which would cross-fade two copies of the effect (the old one frozen in place)
			int best = -1;
			float best_score = 0;
			for (size_t i = 0; i < tracks.size(); i++) {
				if (matched[i] || !same_face(d, tracks[i].box))
					continue;
				float score = iou(d, tracks[i].box) + (tracks[i].alive ? 1.0f : 0.0f);
				if (best < 0 || score > best_score) {
					best_score = score;
					best = (int)i;
				}
			}
			if (best >= 0) {
				Track &t = tracks[best];
				t.det_w = (float)d.size.width;
				// Re-anchor only when the tracked box has drifted; swapping boxes every
				// detection makes the landmarks wobble on a still face
				if (!t.alive || iou(d, t.box) < 0.5f) {
					set_box(t, d);
					t.rel_valid = false;
					t.anchors++;
				}
				t.alive = true;
				t.lm_fail = 0;
				t.missed_dets = 0;
				matched[best] = true;
			} else if (tracks.size() < (size_t)kMaxFaces) {
				Track t;
				t.id = next_id++;
				t.box = d;
				t.det_w = (float)d.size.width;
				tracks.push_back(t);
				matched.push_back(true);
			}
		}
		for (size_t i = 0; i < matched.size() && i < tracks.size(); i++) {
			Track &t = tracks[i];
			// A clearly seen face stays even when the detector misses it
			if (matched[i] || !t.alive || t.sigma < kSigmaClear)
				continue;
			int limit = t.sigma < kSigmaWeak ? kMissedDetectionsCovered : kMissedDetections;
			if (++t.missed_dets >= limit)
				t.alive = false;
		}
	}

	// Change the input box; the next frame measures the landmark jump this causes (bumpless)
	void set_box(Track &t, CGRect b)
	{
		if (t.has_lm && t.alive && !t.box_changed) {
			t.prev_box = t.box;
			t.box_changed = true;
		}
		t.box = b;
	}

	void run_landmarks(float src_w, float src_h, float dt)
	{
		std::vector<int> owners;
		for (size_t i = 0; i < tracks.size(); i++)
			if (tracks[i].alive)
				owners.push_back((int)i);
		if (owners.empty())
			return;

		// One request per face, run in parallel (each face costs ~1.4 ms on its own). A face whose
		// box just changed also runs its previous box, to measure the jump.
		size_t n = owners.size();
		std::vector<VNFaceObservation *> results(n * 2, nil);
		std::vector<CGRect> boxes(n * 2, CGRectNull);
		for (size_t k = 0; k < n; k++) {
			const Track &t = tracks[owners[k]];
			boxes[k] = t.box;
			if (t.box_changed)
				boxes[n + k] = t.prev_box;
		}
		CVPixelBufferRef pb = cur;
		VNFaceObservation *__strong *out = results.data();
		const CGRect *in = boxes.data();
		dispatch_apply(n * 2, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t k) {
			if (CGRectIsNull(in[k]))
				return;
			@autoreleasepool {
				VNFaceObservation *face =
					[VNFaceObservation faceObservationWithRequestRevision:VNDetectFaceRectanglesRequestRevision3
										boundingBox:in[k]
										       roll:nil
											yaw:nil
										      pitch:nil];
				VNDetectFaceLandmarksRequest *req = [[VNDetectFaceLandmarksRequest alloc] init];
				req.constellation = VNRequestFaceLandmarksConstellation76Points;
				req.inputFaceObservations = @[face];
				VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:pb
													   options:@{}];
				if ([handler performRequests:@[req] error:nil] && req.results.count > 0)
					out[k] = req.results.firstObject;
			}
		});

		std::vector<bool> updated(tracks.size(), false);
		for (Track &t : tracks)
			t.raw_new = false;
		for (size_t k = 0; k < n; k++) {
			Track &t = tracks[owners[k]];
			Landmarks raw;
			float sigma = 0;
			bool ok = results[k] && read_landmarks(results[k], src_w, src_h, raw, &sigma);
			Landmarks before;
			bool have_before = t.box_changed && results[n + k] &&
					   read_landmarks(results[n + k], src_w, src_h, before);
			t.box_changed = false;
			if (ok && sigma > kSigmaNoFace) {
				t.sigma = sigma;
				ok = false;
			}
			if (!ok)
				continue;
			updated[owners[k]] = true;
			t.lm_fail = 0;
			t.raw = raw;
			t.raw_new = true;
			t.sigma = sigma;
			float target = std::clamp((kSigmaOccluded - sigma) / (kSigmaOccluded - kSigmaClear), 0.0f, 1.0f);
			t.clarity = t.has_lm ? t.clarity + (target - t.clarity) * 0.3f : target;

			// Bumpless box change: carry the jump the new box caused, then let it fade
			float fw = std::max(length(raw.p[0] - raw.p[16]), 1.0f);
			float fade = std::exp(-dt / kCorrSeconds);
			for (Vec2 &c : t.corr.p)
				c = c * fade;
			if (have_before) {
				float mean = 0;
				for (int i = 0; i < kNumPoints; i++)
					mean += length(raw.p[i] - before.p[i]);
				// A huge difference means the old box had lost the face: take the new one as is
				if (mean / kNumPoints < 0.1f * fw)
					for (int i = 0; i < kNumPoints; i++)
						t.corr.p[i] = t.corr.p[i] + (raw.p[i] - before.p[i]);
			}
			Landmarks in;
			for (int i = 0; i < kNumPoints; i++)
				in.p[i] = raw.p[i] - t.corr.p[i];

			// Partly covered face: Vision guesses the hidden part differently every frame and with
			// every box. Blend toward the last clear shape, moved with the head.
			if (t.ref_valid && t.clarity < 1.0f) {
				Similarity sim = fit_similarity(t.ref, in);
				for (int i = 0; i < kNumPoints; i++)
					in.p[i] = lerp(sim.apply(t.ref.p[i]), in.p[i], t.clarity);
			}

			// Filtering x / box_width and multiplying back made every box resize jump all points
			// by (position * resize), up to tens of pixels for faces far from the top-left corner
			float size = std::max((float)t.box.size.width * src_w, 1.0f);
			float cutoff = 0.4f + 0.6f * t.clarity;
			for (int i = 0; i < kNumPoints; i++) {
				t.smooth.p[i].x = t.filt[i * 2].filter(in.p[i].x, dt, size, cutoff);
				t.smooth.p[i].y = t.filt[i * 2 + 1].filter(in.p[i].y, dt, size, cutoff);
			}
			t.has_lm = true;
			if (t.clarity > 0.95f) {
				t.ref = t.smooth;
				t.ref_valid = true;
			}
			follow(t, src_w, src_h);
		}
		for (int i : owners) {
			if (!updated[i] && ++tracks[i].lm_fail >= kLandmarkFailures)
				tracks[i].alive = false;
		}
	}

	// Box follows the smoothed landmarks between detections, only while the face is clearly seen
	// (a covered face gives landmarks that would drag the box around) and only for real motion
	void follow(Track &t, float src_w, float src_h)
	{
		if (t.clarity < 0.5f)
			return;
		float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
		for (int k = 0; k < region_offset(R_NOSE); k++) {
			float nx = t.smooth.p[k].x / src_w, ny = 1.0f - t.smooth.p[k].y / src_h;
			minx = std::min(minx, nx);
			maxx = std::max(maxx, nx);
			miny = std::min(miny, ny);
			maxy = std::max(maxy, ny);
		}
		float ew = std::max(maxx - minx, 1e-4f), eh = std::max(maxy - miny, 1e-4f);
		if (!t.rel_valid) {
			t.rx = ((float)t.box.origin.x - minx) / ew;
			t.ry = ((float)t.box.origin.y - miny) / eh;
			t.rw = (float)t.box.size.width / ew;
			t.rh = (float)t.box.size.height / eh;
			t.rel_valid = true;
			return;
		}
		CGRect nb = CGRectMake(minx + t.rx * ew, miny + t.ry * eh, t.rw * ew, t.rh * eh);
		float bw = (float)t.box.size.width;
		bool move = std::fabs(CGRectGetMidX(nb) - CGRectGetMidX(t.box)) > kBoxMoveTol * bw ||
			    std::fabs(CGRectGetMidY(nb) - CGRectGetMidY(t.box)) > kBoxMoveTol * bw ||
			    std::fabs((float)nb.size.width - bw) > kBoxSizeTol * bw;
		if (!move)
			return;
		float min_w = kMinBoxOfDetection * t.det_w;
		if ((float)nb.size.width < min_w)
			nb = scaled(nb, min_w / (float)nb.size.width);
		set_box(t, nb);
	}

	void drop_duplicates()
	{
		for (size_t i = 0; i < tracks.size(); i++)
			for (size_t j = i + 1; j < tracks.size(); j++)
				if (tracks[i].alive && tracks[j].alive && iou(tracks[i].box, tracks[j].box) > 0.5f)
					tracks[j].alive = false;
	}

	void kick_detection()
	{
		std::lock_guard<std::mutex> lock(mu);
		bool due = frames_since_det >= kDetectEvery || (tracks.empty() && frames_since_det >= 1);
		if (det_busy || !due)
			return;
		det_busy = true;
		frames_since_det = 0;
		CVPixelBufferRef pb = CVPixelBufferRetain(cur);
		dispatch_async(det_q, ^{
			std::vector<CGRect> boxes;
			@autoreleasepool {
				VNDetectFaceRectanglesRequest *r = [[VNDetectFaceRectanglesRequest alloc] init];
				VNImageRequestHandler *h = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:pb options:@{}];
				if ([h performRequests:@[r] error:nil]) {
					for (VNFaceObservation *o in r.results)
						boxes.push_back(o.boundingBox);
				}
			}
			CVPixelBufferRelease(pb);
			std::lock_guard<std::mutex> l(mu);
			det_boxes = std::move(boxes);
			det_serial++;
			det_busy = false;
		});
	}

	void kick_segmentation()
	{
		std::lock_guard<std::mutex> lock(mu);
		if (seg_busy)
			return;
		seg_busy = true;
		CVPixelBufferRef pb = CVPixelBufferRetain(cur);
		dispatch_async(seg_q, ^{
			SegMask m;
			@autoreleasepool {
				VNGeneratePersonSegmentationRequest *r = [[VNGeneratePersonSegmentationRequest alloc] init];
				r.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelBalanced;
				r.outputPixelFormat = kCVPixelFormatType_OneComponent8;
				VNImageRequestHandler *h = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:pb options:@{}];
				if ([h performRequests:@[r] error:nil] && r.results.count > 0) {
					CVPixelBufferRef out = r.results.firstObject.pixelBuffer;
					CVPixelBufferLockBaseAddress(out, kCVPixelBufferLock_ReadOnly);
					m.w = (int)CVPixelBufferGetWidth(out);
					m.h = (int)CVPixelBufferGetHeight(out);
					size_t stride = CVPixelBufferGetBytesPerRow(out);
					const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddress(out);
					m.data.resize((size_t)m.w * m.h);
					for (int y = 0; y < m.h; y++)
						memcpy(&m.data[(size_t)y * m.w], base + y * stride, m.w);
					CVPixelBufferUnlockBaseAddress(out, kCVPixelBufferLock_ReadOnly);
				}
			}
			CVPixelBufferRelease(pb);
			std::lock_guard<std::mutex> l(mu);
			if (m.w > 0) {
				m.serial = seg.serial + 1;
				seg = std::move(m);
			}
			seg_busy = false;
		});
	}
};

FaceTracker::FaceTracker() : impl_(new Impl) {}

FaceTracker::~FaceTracker() = default;

uint8_t *FaceTracker::begin_frame(int w, int h, int *stride)
{
	Impl &d = *impl_;
	if (d.cur) {
		if (d.cur_locked)
			CVPixelBufferUnlockBaseAddress(d.cur, 0);
		d.cur_locked = false;
		CVPixelBufferRelease(d.cur);
		d.cur = nullptr;
	}
	if (!d.ensure_pool(w, h))
		return nullptr;
	if (CVPixelBufferPoolCreatePixelBuffer(nullptr, d.pool, &d.cur) != kCVReturnSuccess) {
		d.cur = nullptr;
		return nullptr;
	}
	CVPixelBufferLockBaseAddress(d.cur, 0);
	d.cur_locked = true;
	*stride = (int)CVPixelBufferGetBytesPerRow(d.cur);
	return (uint8_t *)CVPixelBufferGetBaseAddress(d.cur);
}

void FaceTracker::end_frame(float src_w, float src_h, double now, bool want_seg)
{
	Impl &d = *impl_;
	if (!d.cur || !d.cur_locked)
		return;

	float dt = d.last_t < 0 ? 1.0f / 30.0f : (float)std::clamp(now - d.last_t, 0.001, 0.25);
	d.last_t = now;
	d.frames_since_det++;

	@autoreleasepool {
		d.merge_detections();
		CVPixelBufferUnlockBaseAddress(d.cur, 0);
		d.cur_locked = false;
		CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
		d.run_landmarks(src_w, src_h, dt);
		last_landmark_ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000.0;
		d.drop_duplicates();
	}

	// Skin color per face, measured on the vision-scale image
	CVPixelBufferLockBaseAddress(d.cur, kCVPixelBufferLock_ReadOnly);
	const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddress(d.cur);
	int vw = (int)CVPixelBufferGetWidth(d.cur), vh = (int)CVPixelBufferGetHeight(d.cur);
	int stride = (int)CVPixelBufferGetBytesPerRow(d.cur);
	for (Track &t : d.tracks) {
		if (!t.alive || !t.has_lm)
			continue;
		FaceShape shape;
		if (!build_face_shape(t.smooth, shape))
			continue;
		SkinModel m = measure_skin(t.smooth, shape, base, vw, vh, stride, vw / src_w, vh / src_h);
		if (!m.valid)
			continue;
		if (!t.skin.valid) {
			t.skin = m;
		} else {
			const float a = 0.2f;
			t.skin.cb += (m.cb - t.skin.cb) * a;
			t.skin.cr += (m.cr - t.skin.cr) * a;
			t.skin.sigma += (m.sigma - t.skin.sigma) * a;
			t.skin.luma += (m.luma - t.skin.luma) * a;
		}
	}
	CVPixelBufferUnlockBaseAddress(d.cur, kCVPixelBufferLock_ReadOnly);

	// Fade in/out and forget faces that are gone
	for (Track &t : d.tracks) {
		float step = dt / kFadeSeconds;
		t.presence = std::clamp(t.presence + ((t.alive && t.has_lm) ? step : -step), 0.0f, 1.0f);
	}
	d.tracks.erase(std::remove_if(d.tracks.begin(), d.tracks.end(),
				      [](const Track &t) { return !t.alive && t.presence <= 0.0f; }),
		       d.tracks.end());

	if (d.dump)
		d.write_dump(now);

	faces_.clear();
	for (const Track &t : d.tracks) {
		if (!t.has_lm || t.presence <= 0.0f)
			continue;
		FaceInfo f;
		f.id = t.id;
		f.presence = t.presence;
		f.lm = t.smooth;
		f.skin = t.skin;
		faces_.push_back(f);
	}

	d.kick_detection();
	if (want_seg)
		d.kick_segmentation();
}

void FaceTracker::reset()
{
	impl_->tracks.clear();
	impl_->last_t = -1;
	faces_.clear();
}

bool FaceTracker::latest_seg(SegMask &out, uint64_t have_serial)
{
	std::lock_guard<std::mutex> lock(impl_->mu);
	if (impl_->seg.serial == 0 || impl_->seg.serial == have_serial)
		return false;
	out = impl_->seg;
	return true;
}
