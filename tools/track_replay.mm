// Run the plugin's FaceTracker over a video file, outside OBS, at the video's own pace.
//
//   track_replay VIDEO DUMP [--fast]
//
// Writes DUMP.1.csv (same format as BETTERFACE_DUMP in OBS) with raw and smoothed landmarks per
// face per frame, plus DUMP.frames.csv (frame number, video time). Built by tools/build_replay.sh.
// --fast waits for each background detection instead of pacing at real time (deterministic).

#include "../plugin/src/tracker.h"

#import <AVFoundation/AVFoundation.h>
#import <Accelerate/Accelerate.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static void vision_size(int src_w, int src_h, int *w, int *h)
{
	const int long_side = 1280; // same as frame-convert.cpp
	int l = std::max(src_w, src_h);
	if (l <= long_side) {
		*w = src_w;
		*h = src_h;
		return;
	}
	float s = (float)long_side / (float)l;
	*w = std::max(2, (int)std::lround(src_w * s));
	*h = std::max(2, (int)std::lround(src_h * s));
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: track_replay VIDEO DUMP [--fast]\n");
		return 2;
	}
	bool fast = argc > 3 && std::string(argv[3]) == "--fast";
	setenv("BETTERFACE_DUMP", argv[2], 1);
	std::string frames_path = std::string(argv[2]) + ".frames.csv";

	@autoreleasepool {
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
		AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
		AVAssetTrack *track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
		if (!track) {
			fprintf(stderr, "no video track\n");
			return 1;
		}
		NSError *err = nil;
		AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&err];
		AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput
			assetReaderTrackOutputWithTrack:track
					 outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
		[reader addOutput:out];
		[reader startReading];

		FaceTracker tracker;
		FILE *ff = fopen(frames_path.c_str(), "w");
		auto start = std::chrono::steady_clock::now();
		double t0 = -1;
		int n = 0;
		while (CMSampleBufferRef sb = [out copyNextSampleBuffer]) {
			double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
			if (t0 < 0)
				t0 = pts;
			CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
			CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
			int sw = (int)CVPixelBufferGetWidth(pb), sh = (int)CVPixelBufferGetHeight(pb);
			int vw, vh;
			vision_size(sw, sh, &vw, &vh);
			if (!fast) {
				auto due = start + std::chrono::duration<double>(pts - t0);
				std::this_thread::sleep_until(due);
			}
			int stride = 0;
			uint8_t *dst = tracker.begin_frame(vw, vh, &stride);
			if (dst) {
				vImage_Buffer src = {CVPixelBufferGetBaseAddress(pb), (vImagePixelCount)sh, (vImagePixelCount)sw,
						     CVPixelBufferGetBytesPerRow(pb)};
				vImage_Buffer d = {dst, (vImagePixelCount)vh, (vImagePixelCount)vw, (size_t)stride};
				vImageScale_ARGB8888(&src, &d, nullptr, kvImageHighQualityResampling);
				// Video time as the clock, so the One Euro filters see the real frame spacing
				tracker.end_frame((float)sw, (float)sh, pts - t0, false);
				fprintf(ff, "%d,%.4f\n", ++n, pts - t0);
			}
			CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
			CFRelease(sb);
			if (fast)
				std::this_thread::sleep_for(std::chrono::milliseconds(15)); // let detection finish
		}
		fclose(ff);
		printf("%d frames\n", n);
	}
	return 0;
}
