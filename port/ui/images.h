// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: pictures (docs/UI-REDESIGN.md, 7.4 and 9.2). Covers, icons and backdrops are
// decoded on a worker thread (TGA, PNG and JPEG, with stb_image), handed to the GPU a few a frame,
// and let go of, least recently drawn first, past a limit. Each picture also gets two ambient
// colours (its most frequent saturated one and a dark companion, from a 32 x 32 copy), for the
// backdrop and the cover's glow; and a picture can be asked for softened, as a backdrop is drawn.
// The worker is joined by Stop, before a game.

#pragma once

#include "gfx.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ui
{
	struct Picture
	{
		TextureId texture = 0; // 0 until it is on the GPU
		int width = 0, height = 0;
		uint32_t ambient[2] = {}; // RGBA; 0 until known
		bool failed = false;	  // it could not be read
	};

	class Images
	{
	public:
		explicit Images(Gfx& gfx);
		~Images();

		// A picture by its path, asked for when not known yet. blurred: shrunk and softened, for a
		// backdrop. Call each frame it is drawn (that keeps it).
		const Picture& Get(const std::string& path, bool blurred = false);
		// The decoded pictures onto the GPU (a few a frame), and the least recently drawn let go of
		void Update();
		// The worker stopped and joined, every texture destroyed
		void Stop();
		// While false, nothing new is uploaded (the launch's flight: no uploads during motion)
		void SetUploads(bool on) { m_uploads = on; }

	private:
		struct Entry
		{
			Picture picture;
			bool requested = false;
			uint64_t used = 0; // the frame it was last asked for
		};
		struct Job
		{
			std::string key, path;
			bool blurred;
		};
		struct Done
		{
			std::string key;
			int width = 0, height = 0;
			std::vector<uint8_t> rgba;
			uint32_t ambient[2] = {};
			bool failed = false;
		};

		void Work();
		static Done Decode(const Job& job);

		Gfx& m_gfx;
		std::unordered_map<std::string, Entry> m_entries;
		uint64_t m_frame = 0;
		bool m_uploads = true;
		std::mutex m_lock;
		std::condition_variable m_wake;
		std::deque<Job> m_jobs;
		std::deque<Done> m_done;
		bool m_stopping = false;
		std::thread m_worker;
	};
}
