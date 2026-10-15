// Convert an async OBS video frame to a downscaled BGRA image for Vision.
#pragma once

#include <cstdint>

struct obs_source_frame;

// Returns false for formats we cannot read
bool frame_to_bgra(const obs_source_frame *frame, uint8_t *dst, int dst_w, int dst_h, int dst_stride);

// Vision input size for a source of the given size (long side capped)
void vision_size(int src_w, int src_h, int *w, int *h);
