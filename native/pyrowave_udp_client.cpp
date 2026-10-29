#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vulkan/vulkan.h>
#include "pyrowave.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <emmintrin.h>
#include "pyrowave_test_renderer.h"
#include "stage_gpu_probe.h"
#include "pyrowave_gpu_present.h"
#include <memory>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#include "pyrowave_live_stats_writer.h"
#include "video_crypto.h"
#include "remote_cursor.h"
#include "pyrowave_rio_receiver.h"
#include "pyrowave_coalesced_receiver.h"
#include "display_timing_map.h"

namespace {
VideoCrypto videoCrypto;
bool encryptedVideo=true;
bool snapshotCaptureTimestamps=false;
bool receiveRio=false;
bool receiveCoalesced=false;
int receiveCpu=-1;std::atomic<int> appliedReceiveCpu{-1};
std::unique_ptr<RioDatagramReader> rioReader;
std::unique_ptr<CoalescedDatagramReader> coalescedReader;
void CloseNetworkSocket(SOCKET socket){if(rioReader)rioReader.reset();else closesocket(socket);}
ULONG trustedPeer=0;
// Client cursor: host pointer arrives as separate datagrams and drives the
// real system cursor over the video window (see remote_cursor.h).
bool clientCursor=false;
RemoteCursorReceiver remoteCursor;
HWND cursorWindow=nullptr;
float cursorVideoX=0,cursorVideoY=0,cursorScale=1;
std::atomic<int64_t> cursorHostOffsetMs{0};std::atomic<bool> cursorCalibrated{false};
int Width = 2560;
int Height = 1440;
constexpr uint32_t DatagramMagic = 0x50575258u;
// Partial frame: packet 0 is an 8-byte region descriptor {x,y,w,h} (uint16).
constexpr uint32_t PartialDatagramMagic = 0x50575259u;
constexpr size_t MaxQueuedPartialFrames = 16;
constexpr size_t MaxDatagramBytes = 1472;
constexpr size_t MaxPacketBytes = 64 * 1024;
constexpr uint16_t MaxPacketsPerFrame = 4096;
using Clock = std::chrono::steady_clock;

#pragma pack(push, 1)
struct DatagramHeader {
	uint32_t magic;
	uint32_t frame;
	uint64_t captureUtcMs;
	uint16_t index;
	uint16_t count;
	uint16_t fragment;
	uint16_t fragments;
	uint32_t packetBytes;
};
#pragma pack(pop)
static_assert(sizeof(DatagramHeader) == 28, "unexpected PyroWave UDP v5 header size");
constexpr size_t FragmentPayloadBytes = MaxDatagramBytes - 32 - sizeof(DatagramHeader);
#pragma pack(push, 1)
struct ClockMessage {uint32_t magic,version;uint64_t hostSent,laptopReceived,laptopSent;uint32_t nonce,pid;};
#pragma pack(pop)
static_assert(sizeof(ClockMessage)==40,"unexpected clock message size");

struct FrameBuffer {
	std::vector<std::vector<uint8_t>> packets;
	std::vector<uint32_t> packetSizes;
	uint32_t frame = 0;
	uint32_t magic = DatagramMagic;
	uint64_t captureUtcMs = 0;
	uint16_t packetCount = 0;
 uint64_t firstReceivedUs=0,completeUs=0;
};

struct NetworkStats {
	std::atomic<uint64_t> datagrams{0};
	std::atomic<uint64_t> bytes{0};
	std::atomic<uint64_t> completeFrames{0};
	std::atomic<uint64_t> incompletePackets{0};
	std::atomic<uint64_t> startedFrames{0},incompleteFrames{0};
	std::atomic<uint64_t> freshnessDrops{0};
	std::atomic<uint64_t> fecRecovered{0};
	std::atomic<uint64_t> cryptoRejected{0},socketErrors{0},lateAbandonedData{0},lateAbandonedParity{0};
 std::atomic<uint64_t> wireDatagrams{0},wireBytes{0},peerRejected{0};std::atomic<uint32_t> lastRejectedPeer{0};
};

struct ReceiverStats {
	uint64_t presentedFrames = 0;
	uint64_t selectedFrames=0,invalidFrames=0,notReadyFrames=0,decodeFailedFrames=0;
	uint64_t validLatencySamples = 0;
	double decodeMs = 0.0;
	double drawMs = 0.0;
	double latencyMs = 0.0;
	std::deque<double> recentLatency;
	std::deque<double> displayLatency;
	uint64_t displaySamples=0;
};

std::atomic<bool> running{true};
std::wstring overlayText = L"FPS --   LAT -- ms";
POINT lastLocalPointer{};
bool localPointerInitialized = false;
bool localPointerVisible = false;
bool localPointerApplied = false;
uint64_t localPointerLastMove = 0;
constexpr uint64_t LocalPointerIdleMs = 2000;

void UpdateLocalPointer(HWND window)
{
	if (clientCursor) return; // the remote cursor owns the pointer over the window
	POINT point{};
	if (!GetCursorPos(&point)) return;
	if (!localPointerInitialized)
	{
		lastLocalPointer = point;
		localPointerInitialized = true;
	}
	else if (point.x != lastLocalPointer.x || point.y != lastLocalPointer.y)
	{
		localPointerLastMove = GetTickCount64();
		lastLocalPointer = point;
	}
	const bool visible = localPointerLastMove &&
		GetTickCount64() - localPointerLastMove < LocalPointerIdleMs;
	// This only changes the local cursor over our receiver. The host pointer
	// remains part of the transmitted picture; other laptop windows keep theirs.
	if (visible != localPointerVisible || !localPointerApplied)
	{
		localPointerVisible = visible;
		if (WindowFromPoint(point) == window)
		{
			SetCursor(visible ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
			localPointerApplied = true;
		}
	}
}

bool WaitForeground(HWND window, int milliseconds)
{
	for (int waited = 0; waited < milliseconds && GetForegroundWindow() != window; waited += 25)
		Sleep(25);
	return GetForegroundWindow() == window;
}

// The window covers the screen and is topmost, so the cursor is normally over it: a click there is
// delivered as real user input and activates it without moving the pointer. Only click when the
// window really is under the cursor, so the click can never land on another application.
bool ClickToActivate(HWND window)
{
	POINT cursor{};
	if (!GetCursorPos(&cursor))
	{
		fprintf(stderr, "foreground click: GetCursorPos failed %lu\n", GetLastError());
		return false;
	}
	HWND under = WindowFromPoint(cursor);
	char underClass[64] = {};
	if (under)
		GetClassNameA(under, underClass, sizeof(underClass));
	RECT rect{};
	GetWindowRect(window, &rect);
	const bool ours = under && GetAncestor(under, GA_ROOT) == window;
	fprintf(stderr, "foreground click: cursor=%ld,%ld under=%s ours=%d window=%ld,%ld,%ld,%ld visible=%d\n",
		cursor.x, cursor.y, underClass, ours ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom, IsWindowVisible(window) ? 1 : 0);
	if (!ours)
		return false;
	INPUT click[2] = {};
	click[0].type = INPUT_MOUSE;
	click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
	click[1].type = INPUT_MOUSE;
	click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
	const UINT sent = SendInput(2, click, sizeof(INPUT));
	fprintf(stderr, "foreground click: SendInput sent=%u error=%lu\n", sent, GetLastError());
	return sent == 2;
}

// A background process is refused SetForegroundWindow, and without the foreground the window is
// visible but never becomes the active application. Try progressively stronger requests: the
// synthetic Alt press is blocked while an elevated window (Task Manager) is in front, so the
// last resort is a click that the system treats as genuine input.
void TakeForeground(HWND window)
{
	char before[64] = {};
	GetClassNameA(GetForegroundWindow(), before, sizeof(before));
	const char *method = "already";
	if (GetForegroundWindow() != window)
	{
		method = "refused";
		SetForegroundWindow(window);
		if (WaitForeground(window, 100))
			method = "set_foreground";
		else
		{
			SwitchToThisWindow(window, TRUE);
			if (WaitForeground(window, 400))
				method = "switch_to_this_window";
			else
			{
				keybd_event(VK_MENU, 0, 0, 0);
				SetForegroundWindow(window);
				keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
				if (WaitForeground(window, 200))
					method = "alt_key";
				else if (ClickToActivate(window) && WaitForeground(window, 400))
					method = "click";
			}
		}
	}
	fprintf(stderr, "foreground request: previous=%s result=%s\n", before, method);
}

bool IsDesktopLayerWindow(HWND window)
{
	wchar_t name[32] = {};
	if (!GetClassNameW(window, name, 32))
		return false;
	return wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0;
}

// When another program takes focus, keep the display above every window except the one the user just
// switched to: it goes directly under that foreground window (so the clicked program is drawn over it and
// all other windows are covered by it). If the foreground window is itself topmost (taskbar, Start menu,
// Alt+Tab switcher) or is the desktop, drop to the top of the ordinary windows instead; placing it under a
// topmost window would keep it topmost, and placing it under the desktop would hide it.
void PlaceBelowForeground(HWND window, HWND foreground)
{
	if (!foreground || foreground == window)
		return;
	const bool topmostForeground = (GetWindowLongW(foreground, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	SetWindowPos(window, topmostForeground || IsDesktopLayerWindow(foreground) ? HWND_NOTOPMOST : foreground,
		0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	wchar_t foregroundClass[64] = L"(none)";
	GetClassNameW(foreground, foregroundClass, 64);
	fprintf(stderr, "display layer: below foreground class=%ls topmost=%d\n", foregroundClass, topmostForeground ? 1 : 0);
}

// If every foreground request is refused the window stays topmost without ever being activated, so no
// WM_ACTIVATE arrives to let it step aside. Poll for the foreground window changing; every time the user
// switches to another window, re-seat the display directly under it.
void YieldTopmostIfForegroundChanged(HWND window, HWND &seenForeground)
{
	static uint64_t lastCheck = 0;
	const uint64_t now = GetTickCount64();
	if (now - lastCheck < 250)
		return;
	lastCheck = now;
	const HWND foreground = GetForegroundWindow();
	if (foreground == window || foreground == seenForeground)
		return;
	seenForeground = foreground;
	PlaceBelowForeground(window, foreground);
}

uint64_t UtcMs()
{
	FILETIME ft = {};
	GetSystemTimePreciseAsFileTime(&ft);
	ULARGE_INTEGER value = {};
	value.LowPart = ft.dwLowDateTime;
	value.HighPart = ft.dwHighDateTime;
	return (value.QuadPart - 116444736000000000ULL) / 10000ULL;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && clientCursor)
	{
		HCURSOR shape = remoteCursor.cursor.load();
		SetCursor(remoteCursor.visible.load() ? (shape ? shape : LoadCursor(nullptr, IDC_ARROW)) : nullptr);
		return TRUE;
	}
	if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT)
	{
		UpdateLocalPointer(hwnd);
		SetCursor(localPointerVisible ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
		return TRUE;
	}
	if (message == WM_ACTIVATE)
	{
		// Cover the taskbar while active; once another window takes focus, move directly under that
		// window so only the program the user switched to (or the Start menu) is drawn over the display.
		if (LOWORD(wparam) != WA_INACTIVE)
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		else
			PlaceBelowForeground(hwnd, lparam ? reinterpret_cast<HWND>(lparam) : GetForegroundWindow());
		return 0;
	}
	if (message == WM_DESTROY)
	{
		POINT point{};
		if (GetCursorPos(&point) && WindowFromPoint(point) == hwnd)
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
		running.store(false, std::memory_order_relaxed);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NCHITTEST)
		return HTTRANSPARENT;
	if (message == WM_ERASEBKGND)
		return 1;
	if (message == WM_PAINT)
	{
		PAINTSTRUCT paint = {};
		HDC dc = BeginPaint(hwnd, &paint);
		RECT rect = {};
		GetClientRect(hwnd, &rect);
		HBRUSH background = CreateSolidBrush(RGB(14, 18, 24));
		FillRect(dc, &rect, background);
		DeleteObject(background);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(245, 248, 255));
		HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		HGDIOBJ oldFont = SelectObject(dc, font);
		rect.left += 7;
		rect.right -= 5;
		DrawTextW(dc, overlayText.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, oldFont);
		DeleteObject(font);
		EndPaint(hwnd, &paint);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

void NetworkReceiveLoop(SOCKET socket, HANDLE frameReady, std::mutex &mailboxMutex,
	std::deque<FrameBuffer> &mailbox, bool &mailboxReady, NetworkStats &networkStats,StageTraceWriter&trace)
{
	SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
 if(receiveCpu>=0){if(!SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<receiveCpu)){std::fprintf(stderr,"Receive affinity failed: %lu\n",GetLastError());}else appliedReceiveCpu.store(receiveCpu);}
	std::vector<std::vector<uint8_t>> packetData;
	std::vector<std::vector<uint8_t>> fragmentSeen;
	std::vector<uint16_t> fragmentCounts;
	std::vector<uint16_t> receivedFragmentCounts;
	std::vector<uint32_t> packetSizes;
	std::vector<std::vector<uint8_t>> parityData;
	uint32_t frameId = 0;
	uint32_t frameMagic = DatagramMagic;
	uint64_t captureUtcMs = 0;
	uint16_t expected = 0;
	uint32_t receivedPackets = 0;
	bool assembling = false;
	bool haveFrameId = false;
	uint32_t lastFrameId = 0;
	std::array<uint32_t,64> abandonedIds{};std::array<bool,64> abandonedValid{};
	char datagram[MaxDatagramBytes];
 uint64_t firstReceivedUs=0;

	while (running.load(std::memory_order_relaxed))
	{
		sockaddr_in peer{};int peerBytes=sizeof(peer);
		int received = rioReader?rioReader->Receive(datagram,sizeof(datagram),peer):coalescedReader?coalescedReader->Receive(datagram,sizeof(datagram),peer):recvfrom(socket, datagram, sizeof(datagram), 0, reinterpret_cast<sockaddr*>(&peer), &peerBytes);
		if (received == SOCKET_ERROR)
		{
			const int error = WSAGetLastError();
			if (!running.load(std::memory_order_relaxed) || error == WSAETIMEDOUT || error == WSAEWOULDBLOCK)
				continue;
			networkStats.socketErrors.fetch_add(1,std::memory_order_relaxed);
			continue;
		}
  networkStats.wireDatagrams.fetch_add(1,std::memory_order_relaxed);networkStats.wireBytes.fetch_add(uint64_t(received),std::memory_order_relaxed);
		if(peer.sin_addr.s_addr!=trustedPeer){networkStats.peerRejected.fetch_add(1,std::memory_order_relaxed);networkStats.lastRejectedPeer.store(ntohl(peer.sin_addr.s_addr),std::memory_order_relaxed);continue;}
  const uint64_t receivedUs=trace.Enabled()?StageUtcUs():0;
		unsigned char plain[MaxDatagramBytes];
		received=videoCrypto.Open(reinterpret_cast<unsigned char*>(datagram),size_t(received),plain);
		if(received<=0)networkStats.cryptoRejected.fetch_add(1,std::memory_order_relaxed);
		if(received>0)std::memcpy(datagram,plain,size_t(received));
		if(clientCursor&&received>0&&remoteCursor.Handle(plain,size_t(received),cursorWindow,cursorVideoX,cursorVideoY,cursorScale,cursorHostOffsetMs.load(std::memory_order_relaxed),cursorCalibrated.load(std::memory_order_relaxed)))
			continue;
		if (received < int(sizeof(DatagramHeader) + 1))
			continue;

		DatagramHeader header = {};
		std::memcpy(&header, datagram, sizeof(header));
		const size_t payloadBytes = size_t(received) - sizeof(header);
		if ((header.magic != DatagramMagic && header.magic != PartialDatagramMagic) || !header.count || header.count > MaxPacketsPerFrame ||
			header.index >= header.count || !header.packetBytes || header.packetBytes > MaxPacketBytes ||
			!header.fragments || header.fragment > header.fragments || payloadBytes > FragmentPayloadBytes)
			continue;
		const uint32_t expectedFragments = uint32_t((header.packetBytes + FragmentPayloadBytes - 1) / FragmentPayloadBytes);
		if (header.fragment > expectedFragments)
			continue;
		const bool isParity=header.fragment==expectedFragments;
		const size_t fragmentOffset = size_t(header.fragment) * FragmentPayloadBytes;
		const size_t expectedPayloadBytes = isParity?FragmentPayloadBytes:std::min(FragmentPayloadBytes, size_t(header.packetBytes) - fragmentOffset);
		if (header.fragments != expectedFragments || payloadBytes < expectedPayloadBytes ||
			(payloadBytes != expectedPayloadBytes && payloadBytes != FragmentPayloadBytes))
			continue;
		if (haveFrameId)
		{
			const int32_t frameDelta = int32_t(header.frame - lastFrameId);
			if (frameDelta < 0 || (frameDelta == 0 && !assembling)){
				const size_t slot=header.frame%abandonedIds.size();
				if(abandonedValid[slot]&&abandonedIds[slot]==header.frame){
					(isParity?networkStats.lateAbandonedParity:networkStats.lateAbandonedData).fetch_add(1,std::memory_order_relaxed);
				}
				continue;
			}
		}

		if (!assembling || header.frame != frameId)
		{
			if (assembling && receivedPackets < expected){
				networkStats.incompletePackets.fetch_add(expected - receivedPackets, std::memory_order_relaxed);
				networkStats.incompleteFrames.fetch_add(1,std::memory_order_relaxed);
				const size_t slot=frameId%abandonedIds.size();abandonedIds[slot]=frameId;abandonedValid[slot]=true;
			}
			networkStats.startedFrames.fetch_add(1,std::memory_order_relaxed);
			frameId = header.frame;
			frameMagic = header.magic;
   firstReceivedUs=receivedUs;
			captureUtcMs = header.captureUtcMs;
			expected = header.count;
			receivedPackets = 0;
			packetData.resize(expected);
			fragmentSeen.resize(expected);
			fragmentCounts.assign(expected, 0);
			receivedFragmentCounts.assign(expected, 0);
			packetSizes.assign(expected, 0);
			parityData.resize(expected);
			for (size_t i = 0; i < expected; ++i)
			{
				packetData[i].clear();
				fragmentSeen[i].clear();
				parityData[i].clear();
			}
			assembling = true;
			haveFrameId = true;
			lastFrameId = frameId;
		}
		if (header.frame != frameId || header.magic != frameMagic || header.count != expected || header.captureUtcMs != captureUtcMs)
			continue;

		auto &packet = packetData[header.index];
		auto &seenFragments = fragmentSeen[header.index];
		if (packet.empty())
		{
			packet.resize(header.packetBytes);
			seenFragments.assign(header.fragments, 0);
			fragmentCounts[header.index] = header.fragments;
			packetSizes[header.index] = header.packetBytes;
		}
		if (packetSizes[header.index] != header.packetBytes || fragmentCounts[header.index] != header.fragments)
			continue;
		networkStats.datagrams.fetch_add(1, std::memory_order_relaxed);
		networkStats.bytes.fetch_add(uint64_t(received), std::memory_order_relaxed);
		const uint16_t fragmentsBefore=receivedFragmentCounts[header.index];
		if(isParity){parityData[header.index].assign(reinterpret_cast<uint8_t*>(datagram+sizeof(header)),reinterpret_cast<uint8_t*>(datagram+received));}
		else if (!seenFragments[header.fragment])
		{
			std::memcpy(packet.data() + fragmentOffset, datagram + sizeof(header), expectedPayloadBytes);
			seenFragments[header.fragment] = 1;
			++receivedFragmentCounts[header.index];
		}
		if(!parityData[header.index].empty()&&receivedFragmentCounts[header.index]+1==header.fragments){
			auto recovered=parityData[header.index];size_t missing=0;
			for(size_t f=0;f<header.fragments;++f){
				if(!seenFragments[f]){missing=f;continue;}
				size_t offset=f*FragmentPayloadBytes,length=std::min(FragmentPayloadBytes,packet.size()-offset);
				for(size_t b=0;b<length;++b)recovered[b]^=packet[offset+b];
			}
			size_t offset=missing*FragmentPayloadBytes,length=std::min(FragmentPayloadBytes,packet.size()-offset);
			std::memcpy(packet.data()+offset,recovered.data(),length);seenFragments[missing]=1;
			++receivedFragmentCounts[header.index];networkStats.fecRecovered.fetch_add(1,std::memory_order_relaxed);
		}
		if(fragmentsBefore<header.fragments&&receivedFragmentCounts[header.index]==header.fragments)++receivedPackets;

		if (receivedPackets == expected)
		{
			networkStats.completeFrames.fetch_add(1, std::memory_order_relaxed);
			{
				FrameBuffer done;
				done.packets.swap(packetData);
				done.packetSizes.swap(packetSizes);
				done.frame = frameId;
				done.magic = frameMagic;
				done.captureUtcMs = captureUtcMs;
				done.packetCount = expected;
    done.firstReceivedUs=firstReceivedUs;done.completeUs=trace.Enabled()?StageUtcUs():0;
				std::lock_guard<std::mutex> lock(mailboxMutex);
				// A full frame supersedes everything pending. Partial frames only
				// update regions, so they queue in order instead of overwriting.
				if (frameMagic != PartialDatagramMagic)
				{
					networkStats.freshnessDrops.fetch_add(mailbox.size(), std::memory_order_relaxed);
					mailbox.clear();
				}
				else if (mailbox.size() >= MaxQueuedPartialFrames)
				{
					networkStats.freshnessDrops.fetch_add(1, std::memory_order_relaxed);
					mailbox.pop_front();
				}
				mailbox.push_back(std::move(done));
				mailboxReady = true;
				SetEvent(frameReady);
			}
			assembling = false;
		}
	}

	if (assembling && receivedPackets < expected){
		networkStats.incompletePackets.fetch_add(expected - receivedPackets, std::memory_order_relaxed);
		networkStats.incompleteFrames.fetch_add(1,std::memory_order_relaxed);
	}
}

}

int wmain(int argc, wchar_t **argv)
{
	// Match swap-chain pixels to the physical panel, including 150% scaling.
	SetProcessDPIAware();
 wchar_t runtimeBuffer[32768]{};
 DWORD runtimeLength=GetEnvironmentVariableW(L"PYROSCREEN_RUNTIME",runtimeBuffer,32768);
 std::wstring runtimeRoot;
 if(runtimeLength&&runtimeLength<32768)runtimeRoot=runtimeBuffer;
 else{wchar_t legacy[MAX_PATH]{};if(GetEnvironmentVariableW(L"LOCALAPPDATA",legacy,MAX_PATH))runtimeRoot=std::wstring(legacy)+L"\\PyroScreen\\runtime";else runtimeRoot=L".";}
 {
  std::wstring logPath=runtimeRoot+L"\\receiver.stderr.txt";int descriptor=-1;
  if(_wsopen_s(&descriptor,logPath.c_str(),_O_WRONLY|_O_CREAT|_O_TRUNC|_O_TEXT,_SH_DENYNO,_S_IREAD|_S_IWRITE)==0){_dup2(descriptor,_fileno(stderr));_close(descriptor);}
  setvbuf(stderr,nullptr,_IONBF,0);
 }
	unsigned short port = 55165;
	unsigned durationSeconds = 3600;
	int64_t hostOffsetMs = 0;
	int64_t clockErrorMs = 0;
	bool haveClockCalibration = false;
	bool forceCpu = false;
	bool waitablePresent = false;
	UINT preferredRenderer = 0;
	bool chroma444 = false;
	bool traceStages=false;
 bool allowTearing=false;
 bool flushBeforePresent=false,fragmentDecode=false;
#ifdef PYROSCREEN_TEST_FLUSH_PRESENT_DEFAULT
 flushBeforePresent=true;
#endif
#ifdef PYROSCREEN_TEST_FRAGMENT_DEFAULT
 fragmentDecode=true;
#endif
#ifdef PYROSCREEN_TEST_TEARING_DEFAULT
 allowTearing=true;
#endif
#ifdef PYROSCREEN_TEST_STAGE_TRACE_DEFAULT
 traceStages=true;
#endif
	std::wstring secretFile;
	for (int i = 1; i < argc; ++i)
	{
		if (!wcscmp(argv[i], L"--port") && i + 1 < argc)
			port = (unsigned short)_wtoi(argv[++i]);
		else if (!wcscmp(argv[i], L"--width") && i+1<argc) Width=_wtoi(argv[++i]);
		else if (!wcscmp(argv[i], L"--height") && i+1<argc) Height=_wtoi(argv[++i]);
		else if (!wcscmp(argv[i], L"--unencrypted-video")) encryptedVideo=false;
		else if (!wcscmp(argv[i], L"--capture-snapshot-timestamps")) snapshotCaptureTimestamps=true;
  else if (!wcscmp(argv[i], L"--trace-stages")) traceStages=true;
  else if (!wcscmp(argv[i], L"--allow-tearing")) allowTearing=true;
		else if (!wcscmp(argv[i], L"--receive-rio")) receiveRio=true;
		else if (!wcscmp(argv[i], L"--receive-coalesced")) receiveCoalesced=true;
  else if (!wcscmp(argv[i], L"--receive-cpu") && i+1<argc){receiveCpu=_wtoi(argv[++i]);if(receiveCpu<-1||receiveCpu>=int(sizeof(DWORD_PTR)*8))return 1;}
		else if (!wcscmp(argv[i], L"--secret-file") && i+1<argc) secretFile=argv[++i];
		else if (!wcscmp(argv[i], L"--peer") && i+1<argc){IN_ADDR ip{};if(InetPtonW(AF_INET,argv[++i],&ip)!=1)return 1;trustedPeer=ip.s_addr;}
		else if (!wcscmp(argv[i], L"--duration") && i + 1 < argc)
			durationSeconds = (unsigned)_wtoi(argv[++i]);
		else if (!wcscmp(argv[i], L"--host-offset-ms") && i + 1 < argc)
		{
			hostOffsetMs = _wtoi64(argv[++i]);
			haveClockCalibration = true;
		}
		else if (!wcscmp(argv[i], L"--clock-error-ms") && i + 1 < argc)
			clockErrorMs = _wtoi64(argv[++i]);
		else if (!wcscmp(argv[i], L"--cpu-decode")) forceCpu = true;
		else if (!wcscmp(argv[i], L"--renderer-vendor") && i+1<argc) preferredRenderer=wcstoul(argv[++i],nullptr,16);
		else if (!wcscmp(argv[i], L"--chroma444")) chroma444=true;
		else if (!wcscmp(argv[i], L"--waitable-present")) waitablePresent=true;
  else if (!wcscmp(argv[i],L"--flush-before-present"))flushBeforePresent=true;
  else if (!wcscmp(argv[i],L"--decode-fragment"))fragmentDecode=true;
  else if (!wcscmp(argv[i],L"--client-cursor"))clientCursor=true;
		else
		{
			fwprintf(stderr, L"usage: pyrowave-udp-client [--port N] [--duration seconds] [--host-offset-ms N --clock-error-ms N]\n");
			return 1;
		}
	}

#ifdef PYROSCREEN_TEST_RIO_RECEIVE_DEFAULT
 receiveRio=true;
#endif
#ifdef PYROSCREEN_TEST_COALESCED_RECEIVE_DEFAULT
 receiveCoalesced=true;
#endif
 if(receiveRio&&receiveCoalesced){fprintf(stderr,"Test receive backends independently\n");return 1;}
 if(receiveCpu>=0){DWORD_PTR allowed=0,system=0;if(!GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system)||!(allowed&(DWORD_PTR(1)<<receiveCpu))){fprintf(stderr,"Requested receive processor is unavailable\n");return 1;}}
#ifdef PYROSCREEN_TEST_NVIDIA_DEFAULT
 preferredRenderer=0x10de;
#endif
	if(Width<640||Width>3840||Height<480||Height>2160||Width%2||Height%2||secretFile.empty()||!trustedPeer)return 1;
	try{videoCrypto.SetEncrypted(encryptedVideo);videoCrypto.Load(secretFile.c_str());}catch(const std::exception&e){fprintf(stderr,"%s\n",e.what());return 1;}
	WSADATA wsa = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa))
		return 2;
	SOCKET socket = receiveRio?WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_REGISTERED_IO) : ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (socket == INVALID_SOCKET)
		return 2;
	if(receiveCoalesced){coalescedReader.reset(new CoalescedDatagramReader());if(!coalescedReader->Init(socket)){fprintf(stderr,"UDP receive coalescing unavailable: %d; using ordinary receive\n",WSAGetLastError());coalescedReader.reset();}}
	int receiveBuffer = 4 * 1024 * 1024;
	setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&receiveBuffer), sizeof(receiveBuffer));
	DWORD timeoutMs = 20;
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
	sockaddr_in local = {};
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons(port);
	if (bind(socket, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR)
	{
		fprintf(stderr, "UDP bind failed %d\n", WSAGetLastError());
		CloseNetworkSocket(socket);
		WSACleanup();
		return 3;
	}

	WNDCLASSW windowClass = {};
	if(receiveRio){rioReader.reset(new RioDatagramReader());rioReader->Init(socket);}
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = L"PyrowaveTestReceiver";
	RegisterClassW(&windowClass);
	const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
	DEVMODEW screenMode{}; screenMode.dmSize = sizeof(screenMode);
	EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &screenMode);
	HWND window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName,
		L"PyroScreen display", WS_POPUP, 0, 0, screenWidth, screenHeight,
		nullptr, nullptr, windowClass.hInstance, nullptr);
	if (!window)
	{
		CloseNetworkSocket(socket);
		WSACleanup();
		return 4;
	}
	ShowWindow(window, SW_SHOW);
	TakeForeground(window);
	HWND seenForeground = GetForegroundWindow();
	UpdateLocalPointer(window);

	WNDCLASSW overlayClass = {};
	overlayClass.lpfnWndProc = OverlayProc;
	overlayClass.hInstance = windowClass.hInstance;
	overlayClass.lpszClassName = L"PyrowaveStatsOverlay";
	RegisterClassW(&overlayClass);
	HWND overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
		WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, overlayClass.lpszClassName,
		L"PyroScreen live stats", WS_POPUP, 6, std::max(6, screenHeight - 36), 178, 30,
		window, nullptr, windowClass.hInstance, nullptr);
	if (!overlay)
	{
		DestroyWindow(window);
		CloseNetworkSocket(socket);
		WSACleanup();
		return 4;
	}
	// The HUD is blended into the video swap chain, allowing independent flip.

	Nv12Renderer renderer;
	try
	{
		renderer.Init(window, screenWidth, screenHeight,preferredRenderer,waitablePresent,Width,Height,allowTearing);
	}
	catch (HRESULT hr)
	{
		fprintf(stderr, "renderer init failed 0x%08lx\n", (unsigned long)hr);
		DestroyWindow(overlay);
		DestroyWindow(window);
		CloseNetworkSocket(socket);
		WSACleanup();
		return 5;
	}
	// Same letterboxed placement as the renderer, in physical screen pixels.
	cursorScale=std::min(float(screenWidth)/Width,float(screenHeight)/Height);
	cursorVideoX=(screenWidth-Width*cursorScale)/2;cursorVideoY=(screenHeight-Height*cursorScale)/2;cursorWindow=window;
	pyrowave_device device = nullptr;
	std::unique_ptr<PyrowaveGpuPresent> gpuPresent;
	pyrowave_luid luid{};std::memcpy(luid.luid,&renderer.adapterLuid,sizeof(renderer.adapterLuid));
	pyrowave_result gpuResult=PYROWAVE_ERROR_GENERIC;
	if(!forceCpu){
		gpuResult=pyrowave_create_device_by_compat(renderer.adapterVendor,0,nullptr,nullptr,&luid,VK_QUEUE_GLOBAL_PRIORITY_MEDIUM,&device);
		fprintf(stderr,"D3D actual adapter: %ls, vendor %04x, LUID %08lx:%08lx; Vulkan matching result %d\n",renderer.adapterName.c_str(),renderer.adapterVendor,(unsigned long)renderer.adapterLuid.HighPart,(unsigned long)renderer.adapterLuid.LowPart,int(gpuResult));
		fprintf(stderr,"D3D adapter memory: dedicated %.1f MiB, shared system %.1f MiB\n",renderer.dedicatedVideoBytes/(1024.0*1024.0),renderer.sharedSystemBytes/(1024.0*1024.0));
		if(gpuResult!=PYROWAVE_SUCCESS){
			gpuResult=pyrowave_create_device_by_compat(renderer.adapterVendor,0,nullptr,nullptr,nullptr,VK_QUEUE_GLOBAL_PRIORITY_MEDIUM,&device);
			fprintf(stderr,"Vulkan vendor-only matching result %d\n",int(gpuResult));
		}
	}
	if(!forceCpu && gpuResult==PYROWAVE_SUCCESS)
	{
		try{gpuPresent.reset(new PyrowaveGpuPresent());gpuPresent->Init(renderer,device,chroma444,Width,Height,fragmentDecode);}
		catch(HRESULT hr){fprintf(stderr,"GPU interop unavailable 0x%08lx; using CPU fallback\n",(unsigned long)hr);gpuPresent.reset();pyrowave_device_destroy(device);device=nullptr;}
	}
	if (!device && pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, nullptr, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM, &device) != PYROWAVE_SUCCESS)
	{
		fprintf(stderr, "Vulkan device unavailable\n");
		DestroyWindow(overlay);
		DestroyWindow(window);
		CloseNetworkSocket(socket);
		WSACleanup();
		return 6;
	}
	pyrowave_decoder_create_info decoderInfo = {};
	decoderInfo.device = device;
	decoderInfo.width = Width;
	decoderInfo.height = Height;
	if(chroma444&&!gpuPresent){fprintf(stderr,"444 presentation requires shared GPU planes\n");pyrowave_device_destroy(device);return 7;}
	decoderInfo.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
	decoderInfo.fragment_path = pyrowave_decoder_device_prefers_fragment_path(device);
	if(gpuPresent)decoderInfo.fragment_path=fragmentDecode;
	fprintf(stderr,"decode path: %s; renderer vendor: %04x; physical screen: %dx%d/%lu\n",gpuPresent?"shared_gpu_planes":"cpu_readback",renderer.adapterVendor,screenWidth,screenHeight,(unsigned long)screenMode.dmDisplayFrequency);
	pyrowave_decoder decoder = nullptr;
	if (pyrowave_decoder_create(&decoderInfo, &decoder) != PYROWAVE_SUCCESS)
	{
		fprintf(stderr, "decoder init failed\n");
		pyrowave_device_destroy(device);
		DestroyWindow(overlay);
		DestroyWindow(window);
		CloseNetworkSocket(socket);
		WSACleanup();
		return 7;
	}

	std::vector<uint8_t> y(size_t(Width) * Height);
	std::vector<uint8_t> u(size_t(Width) * Height / 4);
	std::vector<uint8_t> v(size_t(Width) * Height / 4);
	std::vector<uint8_t> nv12(size_t(Width) * Height * 3 / 2);
	pyrowave_cpu_buffer output = {};
	output.data[0] = y.data();
	output.data[1] = u.data();
	output.data[2] = v.data();
	output.row_stride_in_bytes[0] = Width;
	output.row_stride_in_bytes[1] = output.row_stride_in_bytes[2] = Width / 2;
	output.plane_size_in_bytes[0] = y.size();
	output.plane_size_in_bytes[1] = u.size();
	output.plane_size_in_bytes[2] = v.size();
	output.width = Width;
	output.height = Height;
	output.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

	HANDLE frameReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!frameReady)
	{
		pyrowave_decoder_destroy(decoder);
		pyrowave_device_destroy(device);
		DestroyWindow(overlay);
		DestroyWindow(window);
		CloseNetworkSocket(socket);
		WSACleanup();
		return 8;
	}
	NetworkStats networkStats;
	ReceiverStats stats;
	std::deque<FrameBuffer> mailbox;
	std::vector<FrameBuffer> pendingFrames;
	FrameBuffer currentFrame;
	std::mutex mailboxMutex;
	bool mailboxReady = false;
	// Partial frames decode with one cached decoder/plane set per region size
	// and are composed onto the renderer canvas. The canvas starts from the
	// last fully decoded frame, which the full-size shared planes still hold.
	struct PartialSlot{int w=0,h=0;uint64_t used=0;pyrowave_decoder decoder=nullptr;std::unique_ptr<PyrowaveGpuPresent> planes;};
	std::deque<PartialSlot> partialSlots;uint64_t partialUseClock=0;
	bool haveFullPlanes=false,canvasBase=false;
	uint64_t partialDecoded=0,partialSkipped=0,partialInvalid=0;
	auto start = Clock::now();
	auto liveReport = start;
	auto lastReport = start;
	auto lastOverlay = start;
	auto lastValidLatency = Clock::time_point{};auto lastDisplayLatency = Clock::time_point{};
	uint64_t livePresented = 0;
	auto lastSubmittedAt = Clock::time_point{};
	std::vector<double> intervalPresentGaps;
	intervalPresentGaps.reserve(256);
	double liveFps = 0.0;
	double instantaneousFps = 0.0;
	double displayedFps = 0.0;
	uint64_t liveDisplayed = 0;
	UINT lastDisplayId = 0;
	bool haveDisplayFeedback = false;
	struct SubmittedFrame{UINT id;uint64_t sourceUtcMs;uint32_t frame;};
	std::deque<SubmittedFrame> submitted;
	double liveLatencyMs = 0.0;
	double liveNetworkFps = 0.0;
	std::wstring statsPath;
	std::wstring calibrationPath;
	const std::wstring snapshotRequest=runtimeRoot+L"\\snapshot.request";
	const std::wstring snapshotPath=runtimeRoot+L"\\receiver-frame.bmp";
	uint64_t calibrationUtcMs = 0;
	std::string calibrationMethod="tls_ntp";
 statsPath=runtimeRoot+L"\\receiver-stats.json";
 calibrationPath=runtimeRoot+L"\\clock-calibration.txt";
	PyrowaveLiveStatsWriter liveStatsWriter(statsPath);
 DisplayTimingMap displayTiming(runtimeRoot);DisplayTimingMap::Feedback etwFeedback{};
 StageTraceWriter stageTrace(traceStages?runtimeRoot+L"\\receiver-stages.jsonl":L"");
 StageGpuProbe gpuProbe(renderer.Device(),renderer.Context(),stageTrace);
 StageFenceProbe fenceProbe(stageTrace);
 StageFenceProbe renderFenceProbe(stageTrace,"render_fence");
	auto partialSlotFor=[&](int w,int h)->PartialSlot*{
		for(auto&s:partialSlots)if(s.w==w&&s.h==h){s.used=++partialUseClock;return &s;}
		// Bound GPU memory: evict the least recently used size. D3D11 and the
		// decoder's device keep in-flight resources alive until the GPU is done.
		if(partialSlots.size()>=24){
			auto oldest=std::min_element(partialSlots.begin(),partialSlots.end(),[](const PartialSlot&a,const PartialSlot&b){return a.used<b.used;});
			if(oldest->decoder)pyrowave_decoder_destroy(oldest->decoder);
			partialSlots.erase(oldest);
		}
		PartialSlot s;s.w=w;s.h=h;s.used=++partialUseClock;
		try{s.planes.reset(new PyrowaveGpuPresent());s.planes->Init(renderer,device,chroma444,w,h,decoderInfo.fragment_path);}catch(HRESULT){return nullptr;}
		pyrowave_decoder_create_info info=decoderInfo;info.width=w;info.height=h;
		if(pyrowave_decoder_create(&info,&s.decoder)!=PYROWAVE_SUCCESS)return nullptr;
		partialSlots.push_back(std::move(s));return &partialSlots.back();
	};
	enum class FrameResult{Invalid,NotReady,Failed,Skipped,Drawn};
	// Decodes one assembled frame. Full frames draw directly until the first
	// partial frame arrives; afterwards every frame updates the canvas.
	auto decodeFrame=[&](FrameBuffer&f,uint64_t*t5,uint64_t*t5Return,uint64_t*t7,bool traced)->FrameResult{
		const bool partial=f.magic==PartialDatagramMagic;
		pyrowave_decoder target=decoder;PyrowaveGpuPresent*planes=gpuPresent.get();
		int x=0,y=0,w=Width,h=Height;uint16_t first=0;
		if(partial){
			if(!gpuPresent){++partialSkipped;return FrameResult::Skipped;}
			if(f.packetCount<2||f.packetSizes[0]!=8||f.packets[0].size()!=8){++partialInvalid;return FrameResult::Invalid;}
			uint16_t d[4];std::memcpy(d,f.packets[0].data(),sizeof(d));x=d[0];y=d[1];w=d[2];h=d[3];
			if(w<128||h<128||(w&1)||(h&1)||x+w>Width||y+h>Height){++partialInvalid;return FrameResult::Invalid;}
			if(!canvasBase){
				if(!haveFullPlanes){++partialSkipped;return FrameResult::Skipped;}
				ID3D11ShaderResourceView* base[3];gpuPresent->Views(base);renderer.DrawGpuPlanesToCanvas(base,0,0,Width,Height);canvasBase=true;
			}
			PartialSlot*slot=partialSlotFor(w,h);
			if(!slot){++partialSkipped;return FrameResult::Failed;}
			target=slot->decoder;planes=slot->planes.get();first=1;
		}
		// The outer monotonic 32-bit frame ID has already rejected old data.
		// These assembled intra frames are independent. Keeping the codec's
		// 3-bit sequence history can reject good frames after a loss gap >= 4.
		pyrowave_decoder_clear(target);
		for(uint16_t i=first;i<f.packetCount;++i){
			const uint32_t packetSize=f.packetSizes[i];
			if(!packetSize||f.packets[i].size()!=packetSize||pyrowave_decoder_push_packet(target,f.packets[i].data(),packetSize)!=PYROWAVE_SUCCESS)return FrameResult::Invalid;
		}
		if(!pyrowave_decoder_decode_is_ready(target,false))return FrameResult::NotReady;
		if(traced){*t5=StageUtcUs();gpuProbe.Begin(f.frame);}
		if(!planes)return pyrowave_decoder_decode_cpu_buffer_synchronous(decoder,&output)==PYROWAVE_SUCCESS?FrameResult::Drawn:FrameResult::Failed;
		if(!planes->Decode(target,traced?&gpuProbe:nullptr,traced?&fenceProbe:nullptr,f.frame,traced?t5Return:nullptr,traced?t7:nullptr,traced?t5:nullptr))return FrameResult::Failed;
		ID3D11ShaderResourceView* views[3];planes->Views(views);
		if(partial){renderer.DrawGpuPlanesToCanvas(views,x,y,w,h);++partialDecoded;}
		else{haveFullPlanes=true;if(canvasBase)renderer.DrawGpuPlanesToCanvas(views,0,0,Width,Height);else renderer.DrawGpuPlanes(views);}
		return FrameResult::Drawn;
	};

	auto writeStats = [&]() {
		const double elapsed = std::max(0.001, std::chrono::duration<double>(Clock::now() - start).count());
		std::vector<double> latency(stats.recentLatency.begin(), stats.recentLatency.end());
		std::sort(latency.begin(), latency.end());
		const double p50 = latency.empty() ? 0.0 : latency[(latency.size() - 1) * 50 / 100];
		const double p95 = latency.empty() ? 0.0 : latency[(latency.size() - 1) * 95 / 100];
		std::vector<double> displayLatency(stats.displayLatency.begin(),stats.displayLatency.end());std::sort(displayLatency.begin(),displayLatency.end());
		const double displayP50=etwFeedback.valid?etwFeedback.p50:(displayLatency.empty()?0.0:displayLatency[(displayLatency.size()-1)*50/100]);
		const double displayP95=etwFeedback.valid?etwFeedback.p95:(displayLatency.empty()?0.0:displayLatency[(displayLatency.size()-1)*95/100]);
		char json[8192];
		int length = snprintf(json, sizeof(json),
			"{\"width\":%d,\"height\":%d,\"elapsed_s\":%.3f,\"datagrams\":%llu,\"bytes\":%llu,\"complete_frames\":%llu,\"presented_frames\":%llu,\"incomplete_packets\":%llu,\"freshness_dropped_frames\":%llu,\"receive_fps\":%.3f,\"present_fps\":%.3f,\"network_complete_fps_1s\":%.3f,\"present_fps_1s\":%.3f,\"receive_mbps\":%.3f,\"decode_mean_ms\":%.3f,\"decode_plus_present_mean_ms\":%.3f,\"full_chain_timestamp_frames\":%llu,\"full_chain_latency_mean_ms\":%.3f,\"full_chain_latency_recent_p50_ms\":%.3f,\"full_chain_latency_recent_p95_ms\":%.3f,\"clock_error_bound_ms\":%lld,\"clock_offset_ms\":%lld,\"clock_calibration_age_ms\":%llu,\"display_width\":%d,\"display_height\":%d,\"display_refresh_hz\":%lu,\"latency_endpoint\":\"%s\"}\n",
			Width, Height, elapsed,
			(unsigned long long)networkStats.datagrams.load(std::memory_order_relaxed),
			(unsigned long long)networkStats.bytes.load(std::memory_order_relaxed),
			(unsigned long long)networkStats.completeFrames.load(std::memory_order_relaxed),
			(unsigned long long)stats.presentedFrames,
			(unsigned long long)networkStats.incompletePackets.load(std::memory_order_relaxed),
			(unsigned long long)networkStats.freshnessDrops.load(std::memory_order_relaxed),
			networkStats.completeFrames.load(std::memory_order_relaxed) / elapsed,
			stats.presentedFrames / elapsed, liveNetworkFps, instantaneousFps,
			double(networkStats.bytes.load(std::memory_order_relaxed)) * 8.0 / elapsed / 1000000.0,
			stats.presentedFrames ? stats.decodeMs / stats.presentedFrames : 0.0,
			stats.presentedFrames ? (stats.decodeMs + stats.drawMs) / stats.presentedFrames : 0.0,
			(unsigned long long)stats.validLatencySamples,
			stats.validLatencySamples ? stats.latencyMs / stats.validLatencySamples : 0.0,
			p50, p95, (long long)clockErrorMs, (long long)hostOffsetMs,
			(unsigned long long)(calibrationUtcMs ? UtcMs() - calibrationUtcMs : 0),
			screenWidth, screenHeight, (unsigned long)screenMode.dmDisplayFrequency, snapshotCaptureTimestamps?"capture_snapshot_to_present_return":"source_desktop_update_to_present_return");
		if (length <= 0 || length >= int(sizeof(json)) || statsPath.empty())
			return;
		length -= 2; // Replace the closing brace/newline with path diagnostics.
		length += snprintf(json + length, sizeof(json) - length,
			",\"gpu_direct_present\":%s,\"renderer_vendor\":%u,\"physical_mode_width\":%lu,\"physical_mode_height\":%lu,\"pid\":%lu,\"sampled_utc_ms\":%llu,\"display_feedback_available\":%s,\"display_feedback_samples\":%llu,\"display_observed_fps_1s\":%.3f,\"source_to_display_p50_ms\":%.3f,\"source_to_display_p95_ms\":%.3f,\"chroma\":\"%s\"}\n",
			gpuPresent?"true":"false", renderer.adapterVendor,
			(unsigned long)screenMode.dmPelsWidth,(unsigned long)screenMode.dmPelsHeight,GetCurrentProcessId(),
			(unsigned long long)UtcMs(),haveDisplayFeedback?"true":"false",(unsigned long long)(etwFeedback.valid?etwFeedback.count:stats.displaySamples),displayedFps,displayP50,displayP95,chroma444?"444":"420");
		if(length>1&&length<int(sizeof(json))){
			length-=2;length+=snprintf(json+length,sizeof(json)-length,",\"fec_recovered_fragments\":%llu,\"waitable_present\":%s,\"presentation_color_space\":\"sRGB_full\",\"ycbcr_matrix\":\"BT709\",\"ycbcr_range\":\"full\",\"color_adjustments\":\"none\",\"clock_calibration_method\":\"%s\",\"process_priority_class\":%lu}\n",(unsigned long long)networkStats.fecRecovered.load(std::memory_order_relaxed),waitablePresent?"true":"false",calibrationMethod.c_str(),(unsigned long)GetPriorityClass(GetCurrentProcess()));
		}
		if(length>1&&length<int(sizeof(json))){
			std::sort(intervalPresentGaps.begin(),intervalPresentGaps.end());
			const double gapP95=intervalPresentGaps.empty()?0.0:intervalPresentGaps[(intervalPresentGaps.size()-1)*95/100];
			const double gapMax=intervalPresentGaps.empty()?0.0:intervalPresentGaps.back();
			length-=2;length+=snprintf(json+length,sizeof(json)-length,",\"present_gap_p95_ms_1s\":%.3f,\"present_gap_max_ms_1s\":%.3f,\"local_cursor_visible\":%s,\"local_cursor_movement_seen\":%s,\"local_cursor_idle_ms\":%llu,\"local_cursor_hide_after_ms\":%llu}\n",gapP95,gapMax,localPointerVisible?"true":"false",localPointerLastMove?"true":"false",(unsigned long long)(localPointerLastMove?GetTickCount64()-localPointerLastMove:0),(unsigned long long)LocalPointerIdleMs);
			intervalPresentGaps.clear();
		}
		if(length<=0||length>=int(sizeof(json)))return;
		length-=2;
		const auto latencyAge=etwFeedback.valid?uint64_t(etwFeedback.delayMs):(lastDisplayLatency==Clock::time_point{}?uint64_t(60000):uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-lastDisplayLatency).count()));
		length+=snprintf(json+length,sizeof(json)-length,",\"last_latency_sample_age_ms\":%llu,\"display_latency_window_samples\":%llu,\"encrypted_video\":%s,\"receive_transport\":\"%s\",\"rio_receive_batches\":%llu,\"rio_receive_waits\":%llu,\"receive_coalescing_requested\":%s,\"receive_coalescing_enabled\":%s,\"receive_messages\":%llu,\"coalesced_messages\":%llu,\"coalesced_datagrams\":%llu,\"max_receive_batch\":%llu}\n",(unsigned long long)latencyAge,(unsigned long long)(etwFeedback.valid?etwFeedback.window:stats.displayLatency.size()),encryptedVideo?"true":"false",receiveRio?"rio":coalescedReader?"coalesced":"recvfrom",(unsigned long long)(rioReader?rioReader->Batches():0),(unsigned long long)(rioReader?rioReader->Waits():0),receiveCoalesced?"true":"false",coalescedReader?"true":"false",(unsigned long long)(coalescedReader?coalescedReader->Messages():0),(unsigned long long)(coalescedReader?coalescedReader->MergedMessages():0),(unsigned long long)(coalescedReader?coalescedReader->MergedDatagrams():0),(unsigned long long)(coalescedReader?coalescedReader->MaximumBatch():0));
		if(length<=0||length>=int(sizeof(json)))return;
		length-=2;length+=snprintf(json+length,sizeof(json)-length,",\"stage_trace_enabled\":%s,\"stage_trace_dropped\":%llu,\"stage_trace_error\":%lu,\"present_sync_interval\":0,\"present_flags\":%u,\"tearing_requested\":%s,\"swap_buffers\":2,\"maximum_frame_latency\":1}\n",stageTrace.Enabled()?"true":"false",stageTrace.Dropped(),stageTrace.Error(),renderer.PresentFlags(),allowTearing?"true":"false");
		if(length<=0||length>=int(sizeof(json)))return;
		length-=2;
		length+=snprintf(json+length,sizeof(json)-length,",\"capture_timestamp_kind\":\"%s\",\"hud_latency_available\":%s,\"hud_latency_ms\":%.3f,\"hud_latency_endpoint\":\"%s\"}\n",snapshotCaptureTimestamps?"snapshot":"desktop_update",haveClockCalibration&&haveDisplayFeedback&&latencyAge<5000?"true":"false",liveLatencyMs,"system_display");
		if(length<=0||length>=int(sizeof(json)))return;
		length-=2;
		length+=snprintf(json+length,sizeof(json)-length,",\"partial_frames_decoded\":%llu,\"partial_frames_skipped\":%llu,\"partial_frames_invalid\":%llu,\"partial_decoders\":%llu,\"canvas_present\":%s}\n",(unsigned long long)partialDecoded,(unsigned long long)partialSkipped,(unsigned long long)partialInvalid,(unsigned long long)partialSlots.size(),canvasBase?"true":"false");
		if(length<=0||length>=int(sizeof(json)))return;
		length-=2;length+=snprintf(json+length,sizeof(json)-length,",\"display_feedback_kind\":\"%s\",\"display_latency_endpoint\":\"source_desktop_update_to_system_display\",\"display_feedback_delay_ms\":%.3f,\"etw_unmatched_presents\":%llu,\"etw_dropped_presents\":%llu,\"etw_repeated_sources_ignored\":%llu}\n",etwFeedback.valid?"etw":haveDisplayFeedback?"dxgi":"unavailable",etwFeedback.delayMs,(unsigned long long)etwFeedback.unmatched,(unsigned long long)etwFeedback.dropped,(unsigned long long)etwFeedback.repeats);
        if(length<=0||length>=int(sizeof(json)))return;
        length-=2;
        length+=snprintf(json+length,sizeof(json)-length,",\"network_started_frames\":%llu,\"network_incomplete_frames\":%llu,\"decoder_selected_frames\":%llu,\"decoder_invalid_frames\":%llu,\"decoder_not_ready_frames\":%llu,\"decoder_failed_frames\":%llu}\n",(unsigned long long)networkStats.startedFrames.load(std::memory_order_relaxed),(unsigned long long)networkStats.incompleteFrames.load(std::memory_order_relaxed),(unsigned long long)stats.selectedFrames,(unsigned long long)stats.invalidFrames,(unsigned long long)stats.notReadyFrames,(unsigned long long)stats.decodeFailedFrames);
        if(length<=0||length>=int(sizeof(json)))return;
        length-=2;
        length+=snprintf(json+length,sizeof(json)-length,",\"receive_socket_errors\":%llu,\"transport_rejected_datagrams\":%llu,\"late_data_for_abandoned_frames\":%llu,\"late_parity_for_abandoned_frames\":%llu}\n",(unsigned long long)networkStats.socketErrors.load(std::memory_order_relaxed),(unsigned long long)networkStats.cryptoRejected.load(std::memory_order_relaxed),(unsigned long long)networkStats.lateAbandonedData.load(std::memory_order_relaxed),(unsigned long long)networkStats.lateAbandonedParity.load(std::memory_order_relaxed));
        if(length<=0||length>=int(sizeof(json)))return;
        length-=2;length+=snprintf(json+length,sizeof(json)-length,",\"received_wire_datagrams_total\":%llu,\"received_wire_bytes_total\":%llu,\"untrusted_peer_datagrams\":%llu,\"last_untrusted_peer_ipv4_u32\":%u,\"trusted_peer_ipv4_u32\":%u,\"receive_cpu_requested\":%d,\"receive_cpu_applied\":%d}\n",(unsigned long long)networkStats.wireDatagrams.load(),(unsigned long long)networkStats.wireBytes.load(),(unsigned long long)networkStats.peerRejected.load(),networkStats.lastRejectedPeer.load(),ntohl(trustedPeer),receiveCpu,appliedReceiveCpu.load());
        if(length<=0||length>=int(sizeof(json)))return;
        {
         std::vector<double> cursorLatency;{std::lock_guard<std::mutex> lock(remoteCursor.latencyMutex);cursorLatency.swap(remoteCursor.latencyMs);}
         std::sort(cursorLatency.begin(),cursorLatency.end());
         auto at=[&](double q){return cursorLatency.empty()?-1.0:cursorLatency[std::min(cursorLatency.size()-1,size_t(q*(cursorLatency.size()-1)+0.5))];};
         length-=2;
         length+=snprintf(json+length,sizeof(json)-length,",\"client_cursor\":%s,\"cursor_positions_received_total\":%llu,\"cursor_shapes_applied_total\":%llu,\"cursor_latency_samples\":%llu,\"cursor_latency_p50_ms\":%.3f,\"cursor_latency_p95_ms\":%.3f,\"cursor_latency_endpoint\":\"host_cursor_sample_to_receiver_setcursorpos\"}\n",clientCursor?"true":"false",(unsigned long long)remoteCursor.positions.load(),(unsigned long long)remoteCursor.shapes.load(),(unsigned long long)cursorLatency.size(),at(0.5),at(0.95));
        }
        if(length<=0||length>=int(sizeof(json)))return;
        liveStatsWriter.Publish(json,size_t(length));
	};

	auto updateLiveUi = [&]() {
		const auto now = Clock::now();
        etwFeedback=displayTiming.Poll();
		DXGI_FRAME_STATISTICS displayStats{};
		// DXGI refresh-counter feedback is unusable with tearing. Avoid querying
		// it on every frame (and every idle UI wake) unless a diagnostic trace
		// explicitly needs the raw feedback.
		if((!renderer.PresentFlags()||stageTrace.Enabled()) && renderer.DisplayStatistics(displayStats) && displayStats.PresentCount!=lastDisplayId){
			lastDisplayId=displayStats.PresentCount;
			for(const auto& frame:submitted)if(frame.id==displayStats.PresentCount){
				LARGE_INTEGER counter{},frequency{};QueryPerformanceCounter(&counter);QueryPerformanceFrequency(&frequency);
				const double displayUtc=double(UtcMs())-double(counter.QuadPart-displayStats.SyncQPCTime.QuadPart)*1000.0/frequency.QuadPart+
					int32_t(displayStats.PresentRefreshCount-displayStats.SyncRefreshCount)*1000.0/std::max<DWORD>(1,screenMode.dmDisplayFrequency);
				const double age=displayUtc-double(frame.sourceUtcMs)-double(hostOffsetMs);
    if(stageTrace.Enabled()){
     const double displayUs=double(StageUtcUs())-double(counter.QuadPart-displayStats.SyncQPCTime.QuadPart)*1e6/frequency.QuadPart+int32_t(displayStats.PresentRefreshCount-displayStats.SyncRefreshCount)*1e6/std::max<DWORD>(1,screenMode.dmDisplayFrequency);
     char row[320];snprintf(row,sizeof(row),"{\"type\":\"display\",\"frame\":%u,\"present_id\":%u,\"display_us\":%.3f,\"clock_offset_us\":%lld,\"clock_bound_us\":%lld,\"calibrated\":%s}\n",frame.frame,frame.id,displayUs,(long long)hostOffsetMs*1000,(long long)clockErrorMs*1000,haveClockCalibration?"true":"false");stageTrace.Publish(row);
    }
				// The refresh-counter extrapolation can predate GPU completion
    // with vsync-off. Keep raw diagnostics, but never report it as a
    // validated display latency for that mode.
				if(!renderer.PresentFlags()&&haveClockCalibration&&age>=0.0&&age<5000.0){
					stats.displayLatency.push_back(age);if(stats.displayLatency.size()>512)stats.displayLatency.pop_front();
					++stats.displaySamples;++liveDisplayed;haveDisplayFeedback=true;lastDisplayLatency=now;
				}
				break;
			}
			while(!submitted.empty() && int32_t(submitted.front().id-displayStats.PresentCount)<=0)submitted.pop_front();
		}
		const double reportSeconds = std::chrono::duration<double>(now - liveReport).count();
		if (reportSeconds >= 1.0)
		{
			const uint64_t completed = networkStats.completeFrames.load(std::memory_order_relaxed);
			static uint64_t previousComplete = 0;
			liveNetworkFps = double(completed - previousComplete) / reportSeconds;
			previousComplete = completed;
			const double measuredFps = double(livePresented) / reportSeconds;
			instantaneousFps = measuredFps;
			displayedFps=etwFeedback.valid?etwFeedback.fps:double(liveDisplayed)/reportSeconds;liveDisplayed=0;
			liveFps = livePresented ? (liveFps == 0.0 ? measuredFps : 0.30 * measuredFps + 0.70 * liveFps) : 0.0;
			livePresented = 0;
			liveReport = now;
		}
		if (now - lastOverlay >= std::chrono::milliseconds(250))
		{
			if(etwFeedback.valid&&haveClockCalibration){haveDisplayFeedback=true;displayedFps=etwFeedback.fps;}
            else haveDisplayFeedback=!renderer.PresentFlags()&&!stats.displayLatency.empty()&&lastDisplayLatency!=Clock::time_point{}&&now-lastDisplayLatency<std::chrono::seconds(1);
            const double overlayFps=haveDisplayFeedback?displayedFps:instantaneousFps;
			const auto& latencySamples=stats.displayLatency;
            if(etwFeedback.valid&&haveClockCalibration){
                liveLatencyMs=etwFeedback.mean;
                overlayText=L"FPS "+std::to_wstring(int(overlayFps+0.5))+L"   LAT "+std::to_wstring(int(liveLatencyMs+0.5))+L" ms";
            }
            else if (haveClockCalibration && haveDisplayFeedback && !latencySamples.empty())
			{
				const size_t sampleCount = std::min<size_t>(12, latencySamples.size());
				if (sampleCount)
				{
					double total = 0.0;
					for (size_t i = latencySamples.size() - sampleCount; i < latencySamples.size(); ++i)
						total += latencySamples[i];
					liveLatencyMs = total / sampleCount;
				}
				overlayText = L"FPS " + std::to_wstring(int(overlayFps + 0.5)) + L"   LAT " +
					std::to_wstring(int(liveLatencyMs + 0.5)) + L" ms";
			}
			else
				overlayText = L"FPS " + std::to_wstring(int(overlayFps + 0.5)) + L"   LAT -- ms";

			renderer.UpdateOverlay(overlayText);
			lastOverlay = now;
		}
		if (now - lastReport >= std::chrono::seconds(1))
		{
			if(calibrationUtcMs && (UtcMs()<calibrationUtcMs || UtcMs()-calibrationUtcMs>=60000))haveClockCalibration=false;
			FILE *clockFile = _wfopen(calibrationPath.c_str(), L"rt");
			if (clockFile)
			{
				long long offset = 0, error = 0; unsigned long long calibratedUtc = 0;
				char method[32]{};
				if (fscanf(clockFile, "%lld %lld %llu %31s", &offset, &error, &calibratedUtc,method) >= 3 &&
					calibratedUtc <= UtcMs() && UtcMs() - calibratedUtc < 60000 && error >= 0 && error <= 20)
				{
					if (calibrationUtcMs != calibratedUtc) {
						// Renewals must not briefly publish an empty latency window as zero.
						const double correction=double(offset)-double(hostOffsetMs);
						if(!haveClockCalibration||std::abs(correction)>5.0){
							stats.recentLatency.clear();stats.displayLatency.clear();lastValidLatency=Clock::time_point{};lastDisplayLatency=Clock::time_point{};
						}else{
							for(auto* samples:{&stats.recentLatency,&stats.displayLatency}){
								for(auto& age:*samples)age-=correction;
								samples->erase(std::remove_if(samples->begin(),samples->end(),[](double age){return age<0.0||age>=5000.0;}),samples->end());
							}
						}
					}
					hostOffsetMs = offset; clockErrorMs = error; calibrationUtcMs = calibratedUtc; haveClockCalibration = true;
					calibrationMethod="tls_ntp";
				}
				fclose(clockFile);
			}
			writeStats();
   if(stageTrace.Enabled())pyrowave_device_report_performance_stats(device,StageProfileMessage,&stageTrace,false);
			lastReport = now;
		}
	};

	std::thread receiverThread(NetworkReceiveLoop, socket, frameReady, std::ref(mailboxMutex),
		std::ref(mailbox), std::ref(mailboxReady), std::ref(networkStats),std::ref(stageTrace));
	const HANDLE waitHandles[] = { frameReady };
	while (running.load(std::memory_order_relaxed) &&
		(!durationSeconds || std::chrono::duration<double>(Clock::now() - start).count() < durationSeconds))
	{
  gpuProbe.Poll();
		MsgWaitForMultipleObjects(1, waitHandles, FALSE, 2, QS_ALLINPUT);
		MSG message = {};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		UpdateLocalPointer(window);
		cursorHostOffsetMs.store(hostOffsetMs,std::memory_order_relaxed);cursorCalibrated.store(haveClockCalibration,std::memory_order_relaxed);
		YieldTopmostIfForegroundChanged(window, seenForeground);
		if (!running.load(std::memory_order_relaxed))
			break;
		// Wait before selecting the latest mailbox frame, so rendering never
		// holds an older frame while the display queue is full.
		if(renderer.FrameLatencyReady() && WaitForSingleObject(frameReady,0)==WAIT_OBJECT_0){
			// Some hybrid-GPU drivers fail to signal this object after a burst.
			// Bound the wait so that a driver issue cannot freeze the screen.
			WaitForSingleObject(renderer.FrameLatencyReady(),8);
		}

		bool haveFrame = false;
  uint64_t selectedUs=0;
		{
			std::lock_guard<std::mutex> lock(mailboxMutex);
			if (mailboxReady)
			{
				for (auto &queued : mailbox)
					pendingFrames.push_back(std::move(queued));
				mailbox.clear();
    if(stageTrace.Enabled())selectedUs=StageUtcUs();
				mailboxReady = false;
				ResetEvent(frameReady);
				haveFrame = true;
			}
		}

		if (haveFrame)
		{
			++stats.selectedFrames;
			const auto decodeStart = Clock::now();
   const uint64_t pushStartUs=stageTrace.Enabled()?StageUtcUs():0;
   uint64_t t5=0,t5Return=0,t7=0,t8=0,t8Return=0,t8Qpc=0;
   // Earlier queued frames (partial updates) only change the canvas; the
   // newest frame is decoded last and presented.
   for(size_t i=0;i+1<pendingFrames.size();++i)decodeFrame(pendingFrames[i],nullptr,nullptr,nullptr,false);
   currentFrame=std::move(pendingFrames.back());pendingFrames.clear();
   const FrameResult result=decodeFrame(currentFrame,&t5,&t5Return,&t7,stageTrace.Enabled());
   if(result==FrameResult::Invalid)++stats.invalidFrames;
   else if(result==FrameResult::NotReady)++stats.notReadyFrames;
   const bool readyFrame=result==FrameResult::Drawn||result==FrameResult::Failed;
			if (result==FrameResult::Drawn)
			{
				if(canvasBase)renderer.DrawCanvas();
				if(!gpuPresent){
				std::memcpy(nv12.data(), y.data(), y.size());
				uint8_t *uv = nv12.data() + size_t(Width) * Height;
				for (int row = 0; row < Height / 2; ++row)
				{
					const uint8_t *uRow = u.data() + size_t(row) * (Width / 2);
					const uint8_t *vRow = v.data() + size_t(row) * (Width / 2);
					uint8_t *uvRow = uv + size_t(row) * Width;
					for (int x = 0; x < Width / 2; x += 16)
					{
						const __m128i u8 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(uRow + x));
						const __m128i v8 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vRow + x));
						_mm_storeu_si128(reinterpret_cast<__m128i *>(uvRow + size_t(x) * 2), _mm_unpacklo_epi8(u8, v8));
						_mm_storeu_si128(reinterpret_cast<__m128i *>(uvRow + size_t(x) * 2 + 16), _mm_unpackhi_epi8(u8, v8));
					}
				}
				}
				const auto decodeEnd = Clock::now();
				if(!gpuPresent)renderer.Draw(nv12.data());
				if(GetFileAttributesW(snapshotRequest.c_str())!=INVALID_FILE_ATTRIBUTES && renderer.Snapshot(snapshotPath.c_str()))DeleteFileW(snapshotRequest.c_str());
				renderer.DrawOverlay();
    if(stageTrace.Enabled()){gpuProbe.End();if(gpuPresent)gpuPresent->WatchRenderCompletion(renderFenceProbe,currentFrame.frame);}
    if(flushBeforePresent)renderer.Context()->Flush();
    if(stageTrace.Enabled()){t8=StageUtcUs();t8Qpc=StageQpc();}
				displayTiming.Present(currentFrame.frame,currentFrame.captureUtcMs,hostOffsetMs,clockErrorMs,haveClockCalibration,snapshotCaptureTimestamps);
				renderer.PresentVideo();
    if(stageTrace.Enabled())t8Return=StageUtcUs();
				const UINT presentId=(!renderer.PresentFlags()||stageTrace.Enabled())?renderer.LastPresentId():0;
				if(presentId){submitted.push_back({presentId,currentFrame.captureUtcMs,currentFrame.frame});if(submitted.size()>1024)submitted.pop_front();}
    if(stageTrace.Enabled()){
     char row[1200];snprintf(row,sizeof(row),"{\"type\":\"receiver\",\"pid\":%lu,\"frame\":%u,\"t4_first_us\":%llu,\"t4_complete_us\":%llu,\"selected_us\":%llu,\"push_start_us\":%llu,\"t5_us\":%llu,\"t5_return_us\":%llu,\"t7_submit_us\":%llu,\"t8_call_us\":%llu,\"t8_return_us\":%llu,\"t8_qpc\":%llu,\"present_id\":%u,\"clock_offset_us\":%lld,\"clock_bound_us\":%lld,\"calibrated\":%s,\"trace_dropped\":%llu}\n",GetCurrentProcessId(),currentFrame.frame,currentFrame.firstReceivedUs,currentFrame.completeUs,selectedUs,pushStartUs,t5,t5Return,t7,t8,t8Return,t8Qpc,presentId,(long long)hostOffsetMs*1000,(long long)clockErrorMs*1000,haveClockCalibration?"true":"false",stageTrace.Dropped());stageTrace.Publish(row);
    }
				const auto presentEnd = Clock::now();
				if(lastSubmittedAt!=Clock::time_point{})
					intervalPresentGaps.push_back(std::chrono::duration<double,std::milli>(presentEnd-lastSubmittedAt).count());
				lastSubmittedAt=presentEnd;
				stats.decodeMs += std::chrono::duration<double, std::milli>(decodeEnd - decodeStart).count();
				stats.drawMs += std::chrono::duration<double, std::milli>(presentEnd - decodeEnd).count();
				++stats.presentedFrames;
				++livePresented;
				if (haveClockCalibration && currentFrame.captureUtcMs)
				{
					const int64_t ageMs = int64_t(UtcMs()) - int64_t(currentFrame.captureUtcMs) - hostOffsetMs;
					if (ageMs >= 0 && ageMs < 5000)
					{
						lastValidLatency = presentEnd;
						const double latency = double(ageMs);
						stats.latencyMs += latency;
						++stats.validLatencySamples;
						stats.recentLatency.push_back(latency);
						if (stats.recentLatency.size() > 512)
							stats.recentLatency.pop_front();
					}
				}
			}
   else if(readyFrame){++stats.decodeFailedFrames;if(stageTrace.Enabled()){gpuProbe.Decoded();gpuProbe.End();}}
			updateLiveUi();
		}
		else
			updateLiveUi();
	}

	running.store(false, std::memory_order_relaxed);
	if (receiverThread.joinable())
		receiverThread.join();
	writeStats();
	const double elapsed = std::max(0.001, std::chrono::duration<double>(Clock::now() - start).count());
	printf("{\"receiver\":\"PyroWave\",\"display\":\"primary-borderless\",\"width\":%d,\"height\":%d,\"elapsed_s\":%.3f,\"datagrams\":%llu,\"bytes\":%llu,\"complete_frames\":%llu,\"presented_frames\":%llu,\"freshness_dropped_frames\":%llu,\"receive_fps\":%.3f,\"present_fps\":%.3f,\"decode_mean_ms\":%.3f,\"decode_plus_present_mean_ms\":%.3f,\"stats_file\":\"%ls\"}\n",
		Width, Height, elapsed,
		(unsigned long long)networkStats.datagrams.load(std::memory_order_relaxed),
		(unsigned long long)networkStats.bytes.load(std::memory_order_relaxed),
		(unsigned long long)networkStats.completeFrames.load(std::memory_order_relaxed),
		(unsigned long long)stats.presentedFrames,
		(unsigned long long)networkStats.freshnessDrops.load(std::memory_order_relaxed),
		networkStats.completeFrames.load(std::memory_order_relaxed) / elapsed,
		stats.presentedFrames / elapsed,
		stats.presentedFrames ? stats.decodeMs / stats.presentedFrames : 0.0,
		stats.presentedFrames ? (stats.decodeMs + stats.drawMs) / stats.presentedFrames : 0.0,
		statsPath.c_str());

	CloseHandle(frameReady);
	for(auto&slot:partialSlots){if(slot.decoder)pyrowave_decoder_destroy(slot.decoder);slot.planes.reset();}
	pyrowave_decoder_destroy(decoder);
	gpuPresent.reset();
	pyrowave_device_destroy(device);
	DestroyWindow(overlay);
	DestroyWindow(window);
	CloseNetworkSocket(socket);
	WSACleanup();
	return 0;
}
