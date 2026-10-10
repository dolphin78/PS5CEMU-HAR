// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: the port's boot log, /data/ps5cemu/logs/boot.log. Every line also goes to stdout, which
// the console's klog shows. Cemu's own log (log.txt) is separate and lives in /data/ps5cemu. Open
// keeps the four sessions before: boot.prev.log, boot.2.log to boot.4.log, each with Cemu's log of
// it as cemu.prev.txt, cemu.2.txt and so on.

#pragma once

#include <fmt/format.h>
#include <string_view>

namespace ps5log
{
	void Open(const char* folder);
	void Write(std::string_view line);
	const char* Path();
	// The Vulkan driver's messages go to stderr, which only the klog shows: its submission
	// timings every 10 s ("radv/ps5 submissions: ..."), the VideoOut mode it took ("wsi/videoout:
	// ..."), its errors. From this call on they are in the boot log too, as "[driver] ..." lines;
	// the rest of stderr still goes where it went. Call it once, before the driver starts.
	void ForwardDriverMessages();

	template<typename... Args>
	void Line(fmt::format_string<Args...> format, Args&&... args)
	{
		Write(fmt::format(format, std::forward<Args>(args)...));
	}
}
