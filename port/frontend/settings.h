// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the launcher's settings, /data/ps5cemu/ps5cemu.json. Cemu's own settings stay in its
// settings.xml; the launcher writes the few it manages (game folder, volume, overlay, upscaling
// filter) into both. Azahar's are here (n3ds), and given to it when a 3DS game starts.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps5settings
{
	// Azahar's side
	struct N3ds
	{
		std::string gamesFolder = "/data/ps5cemu/azahar/games";
		int resolution = 6;			  // the internal resolution, times the 3DS's 400x240
		int layout = 2;				  // the screens' layout (ps5azahar::Layout): the top one large
		int textureFilter = 0;		  // none
		int volume = 100;			  // percent
		bool motion = true;			  // the DualSense's motion sensors as the 3DS's
		int deadzone = 15;			  // the sticks', percent
		bool performance = false;	  // the frame rate and speed over the game (its in-game menu)
		int region = -1;			  // the emulated 3DS's region: -1 automatic (the game's), else Azahar's region_value
		int language = -1;			  // the emulated 3DS's language: -1 as Azahar sets it, else its SystemLanguage (0 to 11)
		bool customTextures = false;  // texture packs from azahar/load/textures/<title ID> (Azahar's custom_textures)
		int border = 0;				  // artwork around the screens: 0 none, else ps5ingame3ds::kBorderNames' theme
		int cpuClock = 100;			  // the emulated CPU's speed, percent of the 3DS's (Azahar's cpu_clock_percentage)
		std::string articAddress;	  // the last Artic Base server's IPv4 address, a 3DS on the network
		int gameCount = -1;			  // the library's games when last looked for (the start screen's); -1: never
		std::map<std::string, std::string> buttons; // 3DS button: DualSense input, where not the default
		uint64_t lastGame = 0;		  // title ID, or one made from the path
		std::vector<uint64_t> recent; // newest first, at most four
	};

	struct Launcher
	{
		std::string gamesFolder = "/data/ps5cemu/games";
		int upscaleFilter = 1;		  // how the game's picture is scaled to 4K: Cemu's upscale_filter
		bool highFrameRate = false;	  // the 119.88 Hz mode where the display has it
		bool gamePadStream = false; // local browser screen and touch input
		bool overlay = false;		  // Cemu's performance overlay from the start
		bool asyncShaders = true;	  // Cemu's async_compile (Settings > Video, and the in-game menu's Graphics)
		bool gamePadSpeaker = false;  // the GamePad's sound on player 1's DualSense speaker (Settings > Audio)
		bool rumble = true;
		bool pinCpuThreads = false;	  // an experiment (ps5/threads.h): only in ps5cemu.json
		// only in ps5cemu.json: RADV_DEBUG for the Vulkan driver (e.g. "nongg,nohiz"), to narrow down
		// a GPU hang by turning hardware features off; empty by default
		std::string radvDebug;
		int volume = 100;			  // the TV sound, in percent
		int gameCount = -1;			  // the Wii U library's games when last looked for (the start screen's); -1: never
		uint64_t lastGame = 0;		  // title ID
		std::vector<uint64_t> recent; // newest first, at most four
		N3ds n3ds;
		// The launcher's own sound, on both sides (frontend/sound.h): its music ("shop", "setup" or
		// "off") and the music's volume in percent, and the menu's sounds.
		std::string music = "shop";
		int musicVolume = 50;
		bool boxArt = true;			  // box art from GameTDB (Settings > Online and updates)
		bool menuSounds = true;
		// The emulator the launcher opens on, the one last played ("wiiu" or "3ds"); empty: the
		// start screen, to choose.
		std::string side;
		// Why the last game did not start, when that needed a fresh process to show (the launcher
		// shows it once, then clears it).
		std::string launchError;
	};

	Launcher Load();
	bool Save(const Launcher& settings);
	// The game played last and first among the recent ones (at most four).
	void AddRecent(uint64_t& lastGame, std::vector<uint64_t>& recent, uint64_t titleId);
	inline void AddRecent(Launcher& settings, uint64_t titleId) { AddRecent(settings.lastGame, settings.recent, titleId); }
}
