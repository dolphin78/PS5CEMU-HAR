// SPDX-License-Identifier: GPL-3.0-or-later
#include "jpeg.h"
#include <cstdio>
#include <jpeglib.h>
#include <csetjmp>
#include <cstdlib>
#include <memory>
#include <algorithm>

namespace ps5stream
{
namespace
{
struct JpegError { jpeg_error_mgr base; std::jmp_buf jump; };
void Failed(j_common_ptr info) { std::longjmp(reinterpret_cast<JpegError*>(info->err)->jump, 1); }
struct JpegJob
{
	jpeg_compress_struct info{};
	JpegError error{};
	unsigned char* output = nullptr;
	unsigned long size = 0;
	~JpegJob() { if (info.mem) jpeg_destroy_compress(&info); std::free(output); }
};
}
std::vector<uint8_t> EncodeJPEG(const std::vector<uint8_t>& rgb, int width, int height)
{
	if (width <= 0 || height <= 0 || rgb.size() != size_t(width) * height * 3) return {};
	const double scale = std::min({1.0, 854.0 / width, 480.0 / height});
	const int w = std::max(1, int(width * scale)), h = std::max(1, int(height * scale));
	std::vector<uint8_t> resized;
	const uint8_t* pixels = rgb.data();
	if (w != width || h != height)
	{
		resized.resize(size_t(w) * h * 3);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				std::copy_n(&rgb[(size_t(y * height / h) * width + x * width / w) * 3], 3, &resized[(size_t(y) * w + x) * 3]);
		pixels = resized.data();
	}
	// Keep mutable libjpeg state on the heap so it remains valid after longjmp.
	const auto job = std::make_unique<JpegJob>();
	job->info.err = jpeg_std_error(&job->error.base);
	job->error.base.error_exit = Failed;
	if (setjmp(job->error.jump)) return {};
	jpeg_create_compress(&job->info);
	jpeg_mem_dest(&job->info, &job->output, &job->size);
	job->info.image_width = w; job->info.image_height = h;
	job->info.input_components = 3; job->info.in_color_space = JCS_RGB;
	jpeg_set_defaults(&job->info);
	jpeg_set_quality(&job->info, 75, TRUE);
	job->info.dct_method = JDCT_FASTEST;
	jpeg_start_compress(&job->info, TRUE);
	while (job->info.next_scanline < job->info.image_height)
	{
		auto row = const_cast<JSAMPROW>(pixels + size_t(job->info.next_scanline) * w * 3);
		jpeg_write_scanlines(&job->info, &row, 1);
	}
	jpeg_finish_compress(&job->info);
	return {job->output, job->output + job->size};
}
}
