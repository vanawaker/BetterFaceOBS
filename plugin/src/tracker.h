// Multi-face tracking with Apple Vision.
//
// Landmarks run synchronously every frame on the tracked boxes (about 0.5 ms per face);
// full face detection and person segmentation run on background queues and are merged
// when they finish.
#pragma once

#include "geometry.h"

#include <cstdint>
#include <memory>
#include <vector>

struct SegMask {
	std::vector<uint8_t> data;
	int w = 0, h = 0;
	uint64_t serial = 0;
};

class FaceTracker {
public:
	FaceTracker();
	~FaceTracker();

	// Writable BGRA buffer for the next frame (vision scale). Returns nullptr on failure.
	uint8_t *begin_frame(int w, int h, int *stride);
	// Track faces in the buffer filled after begin_frame. Landmarks are reported in a
	// src_w x src_h pixel space.
	void end_frame(float src_w, float src_h, double now, bool want_seg);

	void reset();
	const std::vector<FaceInfo> &faces() const { return faces_; }
	// Copies the newest segmentation mask if it is newer than have_serial
	bool latest_seg(SegMask &out, uint64_t have_serial);

	double last_landmark_ms = 0; // time spent in Vision landmarks on the last frame

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::vector<FaceInfo> faces_;
};
