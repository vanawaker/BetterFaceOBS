// warpcheck DUMP.csv (one face's rows): how much the slim warp at slim 100 changes from frame to frame
// at 15 points inside the jaw and cheeks. Prints: frame, mean change px, max change px, nose_tip x, y.
// A jump that the landmarks do not show (e.g. the slim center flipping) shows up here. Built by
// tools/build_replay.sh.
#include "geometry.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: warpcheck DUMP.csv\n");
		return 2;
	}
	std::ifstream in(argv[1]);
	std::string line;
	std::vector<Vec2> prev;
	while (std::getline(in, line)) {
		std::vector<std::string> c;
		std::stringstream ss(line);
		std::string x;
		while (std::getline(ss, x, ','))
			c.push_back(x);
		if (c.size() < 11 + kNumPoints * 4)
			continue;
		Landmarks lm;
		for (int k = 0; k < kNumPoints; k++)
			lm.p[k] = {std::stof(c[11 + k * 4 + 2]), std::stof(c[11 + k * 4 + 3])};
		FaceShape s;
		if (!build_face_shape(lm, s))
			continue;
		Vec2 center = (lm.p[0] + lm.p[16]) * 0.5f;
		std::vector<Vec2> d;
		for (int i = 1; i <= 15; i++)
			d.push_back(slim_offset(s, lerp(center, lm.p[i], 0.85f), 0.14f));
		float mean = 0, mx = 0;
		if (!prev.empty())
			for (size_t i = 0; i < d.size(); i++) {
				float v = length(d[i] - prev[i]);
				mean += v / (float)d.size();
				mx = std::max(mx, v);
			}
		printf("%s %.2f %.2f %.1f %.1f\n", c[0].c_str(), mean, mx, s.nose_tip.x, s.nose_tip.y);
		prev = d;
	}
	return 0;
}
