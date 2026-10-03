// SPDX-License-Identifier: GPL-3.0-or-later
#include "gamepad.h"
#include "browser.h"
#include "jpeg.h"
#include "websocket.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace ps5stream
{
namespace
{
	using Clock = std::chrono::steady_clock;
	std::mutex inputMutex, frameMutex;
	std::condition_variable frameChanged;
	std::thread worker, encoder;
	std::atomic<bool> running{false}, demand{false};
	std::atomic<int> videoClients{0};
	std::atomic<uint64_t> nextOwner{0};
	std::string status = "Off";
	struct RawFrame { std::vector<uint8_t> pixels; int width, height; };
	std::shared_ptr<const RawFrame> rawFrame;
	std::shared_ptr<const std::vector<uint8_t>> jpeg;
	uint64_t frameSerial = 0, touchOwner = 0;
	struct Pointer { bool down = false; float x = 0, y = 0; } pointer;
	std::deque<Pointer> edges;
	Clock::time_point lastTouch{}, lastCapture{};

	bool Alive() { return running.load(); }

	void Stop()
	{
		running = false;
		frameChanged.notify_all();
		if (worker.joinable()) worker.join();
		if (encoder.joinable()) encoder.join();
	}

	bool Reply(int fd, int code, const char* type, const std::vector<uint8_t>& body)
	{
		const std::string header = "HTTP/1.1 " + std::to_string(code) + " Response\r\n"
			"Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Type: " +
			type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
		return SendBytes(fd, header.data(), header.size(), Alive) && SendBytes(fd, body.data(), body.size(), Alive);
	}

	void EncodeFrames()
	{
		while (running)
		{
			std::shared_ptr<const RawFrame> source;
			{
				std::unique_lock lock(frameMutex);
				frameChanged.wait(lock, [] { return !running || rawFrame; });
				if (!running) break;
				source = std::move(rawFrame);
			}
			auto encoded = EncodeJPEG(source->pixels, source->width, source->height);
			if (encoded.empty()) continue;
			auto frame = std::make_shared<const std::vector<uint8_t>>(std::move(encoded));
			{
				std::lock_guard lock(frameMutex);
				jpeg = std::move(frame);
				++frameSerial;
			}
			frameChanged.notify_all();
		}
	}

	bool UpdateTouch(const std::string& values, const char* format, uint64_t owner)
	{
		int pressed = 0, used = 0;
		float x = 0, y = 0;
		if (sscanf(values.c_str(), format, &pressed, &x, &y, &used) != 3 || used != int(values.size()) ||
			(pressed != 0 && pressed != 1) || !std::isfinite(x) || !std::isfinite(y) ||
			x < 0 || x > 1 || y < 0 || y > 1)
			return false;
		std::lock_guard lock(inputMutex);
		if (owner && owner != touchOwner) return true;
		const Pointer next{pressed != 0, x, y};
		// Preserve quick taps across controller polls; coalesce only movement.
		if (next.down != pointer.down)
		{
			if (edges.size() >= 32) edges.clear();
			edges.push_back(next);
		}
		pointer = next;
		lastTouch = Clock::now();
		return true;
	}

	void InputStream(WebSocket& socket)
	{
		const auto owner = ++nextOwner;
		{
			std::lock_guard lock(inputMutex);
			touchOwner = owner;
			pointer.down = false;
			edges.clear();
		}
		while (running)
		{
			std::string message;
			const auto result = socket.Receive(message, 100);
			if (result == WebSocket::Result::Idle) continue;
			if (result != WebSocket::Result::Text || !UpdateTouch(message, "%d,%f,%f%n", owner)) break;
		}
		std::lock_guard lock(inputMutex);
		if (touchOwner == owner)
		{
			pointer.down = false;
			edges.clear();
			touchOwner = 0;
		}
	}

	void VideoStream(WebSocket& socket)
	{
		++videoClients;
		bool ready = false;
		uint64_t sentSerial = 0;
		auto lastReady = Clock::now();
		while (running)
		{
			std::string message;
			const auto result = socket.Receive(message, ready ? 1 : 50);
			if (result == WebSocket::Result::Text && message == "ready")
			{
				ready = true;
				lastReady = Clock::now();
			}
			else if (result != WebSocket::Result::Idle) break;
			if (!ready)
			{
				if (Clock::now() - lastReady > std::chrono::seconds(5)) break;
				continue;
			}
			std::shared_ptr<const std::vector<uint8_t>> frame;
			uint64_t serial;
			{
				std::unique_lock lock(frameMutex);
				frameChanged.wait_for(lock, std::chrono::milliseconds(10), [&] { return !running || frameSerial != sentSerial; });
				serial = frameSerial;
				if (serial != sentSerial) frame = jpeg;
			}
			if (!frame) continue;
			// Only one image may be in flight. Slow viewers get the newest completed
			// frame after decoding, rather than a queue of old images.
			if (!socket.Send(2, frame->data(), frame->size())) break;
			sentSerial = serial;
			ready = false;
			lastReady = Clock::now();
		}
		--videoClients;
	}

	void HandleClient(int fd)
	{
		ConfigureSocket(fd);
		std::string request;
		const auto deadline = Clock::now() + std::chrono::milliseconds(500);
		while (running && Clock::now() < deadline && request.size() < 4096 &&
			request.find("\r\n\r\n") == std::string::npos)
		{
			pollfd wait{fd, POLLIN, 0};
			if (poll(&wait, 1, 20) <= 0) continue;
			char buffer[1024];
			const auto n = recv(fd, buffer, sizeof(buffer), 0);
			if (n == 0) return;
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
				return;
			}
			request.append(buffer, n);
		}
		if (request.find("\r\n\r\n") == std::string::npos) return;
		const auto end = request.find(" HTTP/1.1\r\n");
		const std::string path = end != std::string::npos && request.rfind("GET ", 0) == 0
			? request.substr(4, end - 4) : "";
		if (path == "/")
		{
			const std::string page = kBrowser;
			Reply(fd, 200, "text/html; charset=utf-8", {page.begin(), page.end()});
		}
		else if (path == "/input" || path == "/video")
		{
			WebSocket socket(fd, Alive);
			if (!socket.Upgrade(request)) { Reply(fd, 400, "text/plain", {}); return; }
			if (path == "/input") InputStream(socket); else VideoStream(socket);
		}
		// Snapshot and legacy touch routes also remain independent of encoding.
		else if (path == "/frame")
		{
			std::shared_ptr<const std::vector<uint8_t>> frame;
			{ std::lock_guard lock(frameMutex); frame = jpeg; }
			demand = true;
			Reply(fd, frame ? 200 : 204, "image/jpeg", frame ? *frame : std::vector<uint8_t>{});
		}
		else if (path.rfind("/touch?down=", 0) == 0)
			Reply(fd, UpdateTouch(path.substr(12), "%d&x=%f&y=%f%n", 0) ? 204 : 400, "text/plain", {});
		else Reply(fd, 404, "text/plain", {});
	}

	void Serve(int listener)
	{
		struct Client { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
		std::vector<Client> clients;
		while (running)
		{
			for (auto i = clients.begin(); i != clients.end();)
			{
				if (*i->done) { i->thread.join(); i = clients.erase(i); } else ++i;
			}
			pollfd wait{listener, POLLIN, 0};
			if (poll(&wait, 1, 20) <= 0) continue;
			const int fd = accept(listener, nullptr, nullptr);
			if (fd < 0) continue;
			// Bound resources: enough for several viewers and independent touch sockets.
			if (clients.size() >= 12) { close(fd); continue; }
			auto done = std::make_shared<std::atomic<bool>>(false);
			clients.push_back({std::thread([fd, done] { HandleClient(fd); close(fd); *done = true; }), done});
		}
		close(listener);
		for (auto& client : clients) client.thread.join();
	}

	struct Cleanup { ~Cleanup() { Stop(); } } cleanup;
}

void Configure(bool enabled)
{
	Stop();
	{
		std::lock_guard lock(inputMutex);
		pointer.down = false;
		edges.clear();
		touchOwner = 0;
		status = "Off";
	}
	{
		std::lock_guard lock(frameMutex);
		rawFrame.reset();
		jpeg.reset();
		frameSerial = 0;
	}
	demand = false;
	if (!enabled) return;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	int yes = 1;
	if (fd >= 0)
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	int port = 8080;
	bool bound = false;
	if (fd >= 0)
	{
		for (; port <= 65535; ++port)
		{
			address.sin_port = htons(static_cast<uint16_t>(port));
			if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
			{
				bound = true;
				break;
			}
			if (errno != EADDRINUSE)
				break;
		}
	}
	if (!bound || listen(fd, 8) < 0)
	{
		if (fd >= 0)
			close(fd);
		std::lock_guard lock(inputMutex);
		status = "Cannot open a browser streaming port (8080-65535)";
		return;
	}
	std::string host;
	ifaddrs* interfaces = nullptr;
	if (getifaddrs(&interfaces) == 0)
	{
		for (auto i = interfaces; i; i = i->ifa_next)
		{
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
				continue;
			auto a = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
			if ((ntohl(a->sin_addr.s_addr) >> 24) == 127 || a->sin_addr.s_addr == 0)
				continue;
			char ip[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip)))
			{
				host = ip;
				break;
			}
		}
		freeifaddrs(interfaces);
	}
	{
		std::lock_guard lock(inputMutex);
		status = host.empty() ? "No LAN IPv4 address; connect PS5 to Wi-Fi or Ethernet"
			: "http://" + host + ":" + std::to_string(port) + "/";
	}
	running = true;
	encoder = std::thread(EncodeFrames);
	worker = std::thread(Serve, fd);
}

std::string Status()
{
	std::lock_guard lock(inputMutex);
	return status;
}

bool WantFrame()
{
	if (!running || (!videoClients && !demand)) return false;
	const auto now = Clock::now();
	if (now - lastCapture < std::chrono::milliseconds(33)) return false;
	lastCapture = now;
	demand = false;
	return true;
}

void SubmitFrame(const std::vector<uint8_t>& rgb, int w, int h)
{
	if (!running || w <= 0 || h <= 0 || w > 4096 || h > 4096 || rgb.size() != size_t(w) * h * 3) return;
	// Copy outside locks; input never waits for the image buffer.
	auto frame = std::make_shared<const RawFrame>(RawFrame{rgb, w, h});
	{
		std::lock_guard lock(frameMutex);
		rawFrame = std::move(frame);
	}
	frameChanged.notify_all();
}

bool Touch(float& x, float& y)
{
	std::lock_guard lock(inputMutex);
	if (!running || Clock::now() - lastTouch >= std::chrono::seconds(1))
	{
		pointer.down = false;
		edges.clear();
	}
	Pointer state = pointer;
	if (!edges.empty()) { state = edges.front(); edges.pop_front(); }
	x = state.x; y = state.y;
	return state.down;
}
}
