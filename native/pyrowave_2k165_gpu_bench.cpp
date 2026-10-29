#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vulkan/vulkan.h>
#include "pyrowave.h"

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "pyrowave_rio_sender.h"
#include "pyrowave_live_stats_writer.h"
#include "video_crypto.h"
#include "desktop_pointer.h"
#include "remote_cursor.h"
#include "stage_trace.h"
#include "frame_datagram_schedule.h"
#include "datagram_slot_pool.h"

namespace {
VideoCrypto videoCrypto;
bool encryptedVideo=true;
size_t WireDatagramBytes(){return encryptedVideo?1472:1456;}
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
uint64_t UtcMs()
{
	FILETIME ft = {};
	GetSystemTimePreciseAsFileTime(&ft);
	ULARGE_INTEGER value = {};
	value.LowPart = ft.dwLowDateTime;
	value.HighPart = ft.dwHighDateTime;
	return (value.QuadPart - 116444736000000000ULL) / 10000ULL;
}
int Width = 2560;
int Height = 1440;
constexpr int DefaultFps = 165;
constexpr int WarmupSeconds = 3;
constexpr int MeasureSeconds = 20;
constexpr size_t DefaultMaxFrameBytes = 180 * 1024;
constexpr size_t PacketBytes = 1380;
constexpr size_t MaxDatagramBytes = 1472; // IPv4 UDP payload within a 1500-byte LAN MTU.

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
constexpr uint32_t FullDatagramMagic = 0x50575258u;
// Same header; packet 0 is an 8-byte region descriptor {x,y,w,h} (uint16).
constexpr uint32_t PartialDatagramMagic = 0x50575259u;

// Bound the queue to one new frame. Capture/encode never wait for UDP syscalls.
class DatagramSender {
 struct Frame {
  std::vector<uint8_t> bytes;
  std::vector<pyrowave_packet> packets;
  size_t count = 0;
  uint32_t id = 0;
  uint32_t magic = FullDatagramMagic;
  uint64_t utc = 0;
  Clock::time_point started;
  Clock::time_point published;
  SenderStageTimes stages;
 } pending;
 SOCKET socket;
 size_t windowSize;
 bool segmented;
 bool useIocp;
 bool useRio;
 bool useFec;
 bool interleaveFec;
 double pacingMbps;
 bool contiguous;
 bool connected;
 bool pipelined;
 bool skipImmediate;
 StageTraceWriter* trace;
 sockaddr_in destination;
 std::mutex mutex, metricsMutex;
 std::condition_variable ready,capacityReady;
 bool available = false, stopping = false;
 std::thread worker;
 std::atomic<HANDLE> wakePort{nullptr};
 std::vector<double> sendSamples, totalSamples,submitSamples,callSamples,slotSamples,prepareSamples;
 std::vector<double> publishIntervals,overwriteAges;
 Clock::time_point previousPublish{};
 void Run() {
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
  Frame frame;
  frame.bytes.resize(pending.bytes.size());frame.packets.resize(pending.packets.size());
  struct Flight {
   Clock::time_point begin,source;SenderStageTimes stages;
   uint32_t id=0;uint64_t dequeue=0,first=0,last=0,bytes=0;size_t outstanding=0;
   bool closed=false,recorded=false;
  };
  struct Slot { WSAOVERLAPPED overlap{};DatagramHeader header{};bool active=false;std::shared_ptr<Flight> flight; };
  std::vector<Slot> slots(windowSize);
  std::vector<uint8_t> wire;
  if(contiguous&&!segmented&&!useRio)wire.resize(windowSize*MaxDatagramBytes);
  if(segmented)wire.reserve((pending.bytes.size()/FragmentPayloadBytes+pending.packets.size()+1)*MaxDatagramBytes);
  std::vector<uint8_t> parity;
  std::vector<uint16_t> fragmentCounts;
  std::vector<FrameDatagramPart> parts;
  std::unique_ptr<RioDatagramWriter> rio;
  if(useRio){
   const size_t capacity=(pending.bytes.size()/FragmentPayloadBytes+pending.packets.size()*2+1)*MaxDatagramBytes;
   wire.resize(capacity);rio.reset(new RioDatagramWriter());
   rio->Init(socket,destination,reinterpret_cast<char*>(wire.data()),ULONG(wire.capacity()));
  }
  HANDLE completionPort=useIocp?CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket),nullptr,0,1):nullptr;
  if(useIocp&&!completionPort){error.store(GetLastError());return;}
  const bool pipeline=pipelined&&useIocp&&contiguous&&!segmented&&!useRio;
  bool skipSuccess=false;
  if(skipImmediate&&useIocp){
   WSAPROTOCOL_INFOW info{};int size=sizeof(info);
   // Non-IFS layered providers cannot use this notification mode. Fall back
   // to ordinary IOCP semantics when provider capability or setup fails.
   if(!getsockopt(socket,SOL_SOCKET,SO_PROTOCOL_INFOW,reinterpret_cast<char*>(&info),&size)&&
      (info.dwServiceFlags1&XP1_IFS_HANDLES))
    skipSuccess=SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(socket),FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)!=FALSE;
  }
  immediateCompletionEnabled.store(skipSuccess);
  if(pipeline)wakePort.store(completionPort);
  if(!useIocp)for(auto &slot:slots)slot.overlap.hEvent=WSACreateEvent();
  auto completeFlight=[&](const std::shared_ptr<Flight>& f){
   if(!f||!f->closed||f->outstanding||f->recorded||error.load())return;
   f->recorded=true;
   if(trace->Enabled()){
    const auto&t=f->stages;char row[1400];
    snprintf(row,sizeof(row),"{\"type\":\"sender\",\"pid\":%lu,\"frame\":%u,\"source_us\":%llu,\"t0_us\":%llu,\"t1_us\":%llu,\"t1_return_us\":%llu,\"t2_ready_us\":%llu,\"packetized_us\":%llu,\"published_us\":%llu,\"dequeued_us\":%llu,\"t3_first_submit_us\":%llu,\"t3_last_submit_us\":%llu,\"t3_completion_us\":%llu,\"acquire_wait_us\":%llu,\"pacing_wait_us\":%llu,\"accumulated\":%u,\"wire_bytes\":%llu,\"trace_dropped\":%llu}\n",GetCurrentProcessId(),f->id,t.source,t.t0,t.t1,t.t1Return,t.t2,t.packetized,t.published,f->dequeue,f->first,f->last,StageUtcUs(),t.acquireWait,t.pacingWait,t.accumulated,f->bytes,trace->Dropped());trace->Publish(row);
   }
   sentFrames.fetch_add(1);totalSentFrames.fetch_add(1);sentBytes.fetch_add(f->bytes);totalBytes.fetch_add(f->bytes);
   std::lock_guard<std::mutex> lock(metricsMutex);sendSamples.push_back(std::chrono::duration<double,std::milli>(Clock::now()-f->begin).count());totalSamples.push_back(std::chrono::duration<double,std::milli>(Clock::now()-f->source).count());
  };
  auto collect=[&](DWORD timeout){
   OVERLAPPED_ENTRY entries[64]{};ULONG count=0;
   if(!GetQueuedCompletionStatusEx(completionPort,entries,64,&count,timeout,FALSE)){
    DWORD code=GetLastError();if(code!=WAIT_TIMEOUT)error.store(code);return false;
   }
   for(ULONG i=0;i<count;++i){
    if(!entries[i].lpOverlapped&&entries[i].lpCompletionKey==1)continue;
    auto done=reinterpret_cast<Slot*>(entries[i].lpOverlapped);
    if(entries[i].Internal){DWORD transferred=0,flags=0;if(!WSAGetOverlappedResult(socket,&done->overlap,&transferred,FALSE,&flags))error.store(WSAGetLastError());}
    done->active=false;auto owner=std::move(done->flight);
    if(owner){--owner->outstanding;completeFlight(owner);}
   }
   return true;
  };
  auto finish=[&](Slot &slot) {
   if(!slot.active)return;
   if(useIocp){
    while(slot.active){
     if(!collect(INFINITE))break;
    }
    return;
   }
   DWORD transferred=0,flags=0;
   if(!WSAGetOverlappedResult(socket,&slot.overlap,&transferred,TRUE,&flags))error.store(WSAGetLastError());
   slot.active=false;
   auto owner=std::move(slot.flight);if(owner){--owner->outstanding;completeFlight(owner);}
  };
  auto submit=[&](auto&& send){
   // A failed initiation owns no OVERLAPPED operation. Keep the same sealed
   // datagram and slot while draining completions; never retry accepted I/O.
   // Buffer pressure is temporary, but recovery must remain bounded.
   const auto deadline=Clock::now()+std::chrono::milliseconds(50);
   unsigned retries=0;
   while(true){
    const int result=send();
    if(result!=SOCKET_ERROR)return result;
    const int code=WSAGetLastError();
    if(code==WSA_IO_PENDING)return result;
    if((code!=WSAENOBUFS&&code!=WSAEWOULDBLOCK)||retries>=8||Clock::now()>=deadline){
     std::fprintf(stderr,"UDP submission failed: %d; buffer-pressure retries: %u\n",code,retries);
     error.store(code);return result;
    }
    ++retries;bufferPressureRetries.fetch_add(1);
    if(useIocp)collect(0);
    if(error.load())return SOCKET_ERROR;
    Sleep(1);
   }
  };
  size_t sequence=0;
  while(true) {
   { std::unique_lock<std::mutex> lock(mutex);
    if(pipeline){if(!available&&!stopping){lock.unlock();collect(INFINITE);continue;}}
    else ready.wait(lock,[&]{return stopping||available;});
    if(!available&&stopping)break;
    frame.bytes.swap(pending.bytes);frame.packets.swap(pending.packets);
    frame.count=pending.count;frame.id=pending.id;frame.magic=pending.magic;frame.utc=pending.utc;frame.started=pending.started;frame.stages=pending.stages;available=false;
   }
   capacityReady.notify_one();
   const auto begin=Clock::now();auto nextDatagram=begin;uint64_t bytes=0;
   double callMax=0,slotMax=0,prepareMax=0;auto preparedAt=begin;
   const uint64_t dequeue=trace->Enabled()?StageUtcUs():0;uint64_t firstSubmit=0,lastSubmit=0;
   auto flight=std::make_shared<Flight>();flight->begin=begin;flight->source=frame.started;flight->stages=frame.stages;flight->id=frame.id;flight->dequeue=dequeue;
   if(segmented||useRio||!contiguous)wire.clear();
   if(useFec)parity.assign(frame.count*FragmentPayloadBytes,0);
   fragmentCounts.resize(frame.count);
   for(size_t i=0;i<frame.count;++i){const size_t n=(frame.packets[i].size+FragmentPayloadBytes-1)/FragmentPayloadBytes;if(n>UINT16_MAX){error.store(WSAEMSGSIZE);break;}fragmentCounts[i]=uint16_t(n);}
   BuildFrameDatagramSchedule(fragmentCounts,useFec,interleaveFec,parts);
   for(const auto&part:parts) {
     if(error.load())break;
     const size_t i=part.packet,f=part.fragment,fragments=fragmentCounts[i];const auto &packet=frame.packets[i];
     const bool parityFragment=f==fragments;
     size_t offset=f*FragmentPayloadBytes;size_t length=parityFragment?FragmentPayloadBytes:std::min(FragmentPayloadBytes,packet.size-offset);
     uint8_t* parityBytes=useFec?parity.data()+i*FragmentPayloadBytes:nullptr;
     const uint8_t* payload=parityFragment?parityBytes:frame.bytes.data()+packet.offset+offset;
     if(useFec&&!parityFragment)for(size_t b=0;b<length;++b)parityBytes[b]^=payload[b];
     DatagramHeader header{frame.magic,frame.id,frame.utc,uint16_t(i),uint16_t(frame.count),uint16_t(f),uint16_t(fragments),uint32_t(packet.size)};
     if(segmented||useRio){
      size_t start=wire.size();
      if(useRio&&start+WireDatagramBytes()>wire.capacity()){error.store(WSAEMSGSIZE);break;}
      wire.resize(start+WireDatagramBytes(),0);
      // Each segment is a complete datagram in the negotiated video mode.
      // In encrypted mode padding is inside GCM. packetBytes retains codec length.
      unsigned char padded[FragmentPayloadBytes]{};
      std::memcpy(padded,payload,length);
      size_t sealed=videoCrypto.Seal(&header,sizeof(header),padded,sizeof(padded),wire.data()+start);
      if(sealed!=WireDatagramBytes()){error.store(WSAEMSGSIZE);break;}
      continue;
     }
     size_t slotIndex=sequence++%slots.size();
     auto slotBegin=Clock::now();prepareMax=std::max(prepareMax,std::chrono::duration<double,std::milli>(slotBegin-preparedAt).count());
     if(pipeline&&slots[slotIndex].active){
      collect(0);
      // Completion order need not match send order. A still-owned ring slot
      // must not stall submission when another sealed buffer is already free.
      // Socket submission order and the fixed 64-slot capacity are unchanged.
      while(slots[slotIndex].active&&!error.load()){
       const size_t free=FindFreeDatagramSlot(slots,slotIndex);
       if(free<slots.size()){slotIndex=free;slotReuseBypasses.fetch_add(1);break;}
       collect(INFINITE);
      }
     }
     if(error.load())break;
     auto &slot=slots[slotIndex];
     finish(slot);slotMax=std::max(slotMax,std::chrono::duration<double,std::milli>(Clock::now()-slotBegin).count());
     WSAEVENT event=slot.overlap.hEvent;slot.overlap={};slot.overlap.hEvent=event;if(event)WSAResetEvent(event);
     slot.header=header;
     WSABUF buffers[2]={{ULONG(sizeof(slot.header)),reinterpret_cast<char*>(&slot.header)},{ULONG(length),reinterpret_cast<char*>(const_cast<uint8_t*>(payload))}};
     WSABUF single{};
     if(contiguous){uint8_t* bytes=wire.data()+slotIndex*MaxDatagramBytes;size_t sealed=videoCrypto.Seal(&header,sizeof(header),payload,length,bytes);single={ULONG(sealed),reinterpret_cast<char*>(bytes)};}
     if(pacingMbps>0){
      // Do not submit a whole catch-up burst after a scheduler stall. Space
      // individual datagrams on the wire budget, without a receiver buffer.
      while(Clock::now()<nextDatagram)YieldProcessor();
      const size_t wireBytes=(contiguous?single.len:sizeof(slot.header)+length)+28+38;
      nextDatagram=Clock::now()+std::chrono::nanoseconds(int64_t(double(wireBytes)*8000.0/pacingMbps));
     }
     if(trace->Enabled()){lastSubmit=StageUtcUs();if(!firstSubmit)firstSubmit=lastSubmit;}
     const auto callBegin=Clock::now();
     int result=submit([&]{return connected?WSASend(socket,contiguous?&single:buffers,contiguous?1:2,nullptr,0,&slot.overlap,nullptr):WSASendTo(socket,contiguous?&single:buffers,contiguous?1:2,nullptr,0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination),&slot.overlap,nullptr);});
     preparedAt=Clock::now();callMax=std::max(callMax,std::chrono::duration<double,std::milli>(preparedAt-callBegin).count());
     if(error.load())break;
     if(result==SOCKET_ERROR)slot.active=true;
     else {immediateSends.fetch_add(1);if(useIocp&&!skipSuccess)slot.active=true;}
     if(result==SOCKET_ERROR&&slot.active)pendingSends.fetch_add(1);
     if(slot.active){slot.flight=flight;++flight->outstanding;}
     bytes+=contiguous?single.len:sizeof(slot.header)+length;
   }
   if(useRio){if(trace->Enabled())firstSubmit=lastSubmit=StageUtcUs();rio->Send(ULONG(wire.size()),ULONG(WireDatagramBytes()));callMax=rio->LastCallMaxMs;slotMax=rio->LastWaitMaxMs;bytes+=wire.size();}
   else if(segmented){
    // Windows segments each batch into MTU-sized UDP datagrams. Padding keeps
    // every segment aligned; packetBytes defines the meaningful final bytes.
    const size_t batchBytes=32*WireDatagramBytes();
    for(size_t offset=0;offset<wire.size()&&!error.load();offset+=batchBytes){
     auto &slot=slots[sequence++%slots.size()];finish(slot);
     WSAEVENT event=slot.overlap.hEvent;slot.overlap={};slot.overlap.hEvent=event;if(event)WSAResetEvent(event);
     size_t length=std::min(batchBytes,wire.size()-offset);
     WSABUF buffer{ULONG(length),reinterpret_cast<char*>(wire.data()+offset)};
     if(trace->Enabled()){lastSubmit=StageUtcUs();if(!firstSubmit)firstSubmit=lastSubmit;}
     int result=submit([&]{return WSASendTo(socket,&buffer,1,nullptr,0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination),&slot.overlap,nullptr);});
     if(error.load())break;
     if(result==SOCKET_ERROR)slot.active=true;
     else {immediateSends.fetch_add(1);if(useIocp&&!skipSuccess)slot.active=true;}
     if(result==SOCKET_ERROR&&slot.active)pendingSends.fetch_add(1);
     if(slot.active){slot.flight=flight;++flight->outstanding;}
     bytes+=length;
    }
   }
   flight->first=firstSubmit;flight->last=lastSubmit;flight->bytes=bytes;flight->closed=true;
   {std::lock_guard<std::mutex>lock(metricsMutex);submitSamples.push_back(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());callSamples.push_back(callMax);slotSamples.push_back(slotMax);prepareSamples.push_back(prepareMax);}
   if(!pipeline)for(auto &slot:slots)finish(slot);
   else collect(0);
   if(error.load())break;
   completeFlight(flight);
  }
  for(auto &slot:slots){finish(slot);if(slot.overlap.hEvent)WSACloseEvent(slot.overlap.hEvent);}
  wakePort.store(nullptr);if(completionPort)CloseHandle(completionPort);
 }
public:
 bool WaitForCapacity(std::chrono::nanoseconds timeout){
  std::unique_lock<std::mutex> lock(mutex);
  if(!available)return false;
  capacityReady.wait_for(lock,timeout,[&]{return !available||stopping||error.load()!=0;});
  return true;
 }
 std::atomic<uint64_t> sentFrames{0},totalSentFrames{0},sentBytes{0},totalBytes{0},droppedFrames{0},slotReuseBypasses{0};
 std::atomic<uint64_t> immediateSends{0},pendingSends{0};std::atomic<bool> immediateCompletionEnabled{false};
 std::atomic<uint64_t> bufferPressureRetries{0};
 std::atomic<int> error{0};
 DatagramSender(SOCKET s,const sockaddr_in &to,size_t bytes,size_t packets,size_t sendWindow,bool uso,bool iocp,bool registered,bool fec,bool interleave,double pacing,bool singleBuffer,bool connectedSocket,bool pipeline,bool skip,StageTraceWriter*t):socket(s),windowSize(sendWindow),segmented(uso),useIocp(iocp),useRio(registered),useFec(fec),interleaveFec(interleave),pacingMbps(pacing),contiguous(singleBuffer),connected(connectedSocket),pipelined(pipeline),skipImmediate(skip),trace(t),destination(to) {
  pending.bytes.resize(bytes);pending.packets.resize(packets);worker=std::thread(&DatagramSender::Run,this);
 }
 void Publish(std::vector<uint8_t>&bytes,std::vector<pyrowave_packet>&packets,size_t count,uint32_t id,uint64_t utc,Clock::time_point started,const SenderStageTimes&stages,uint32_t magic=FullDatagramMagic){
  std::lock_guard<std::mutex> lock(mutex);const auto now=Clock::now();
  {std::lock_guard<std::mutex> metrics(metricsMutex);
   if(previousPublish!=Clock::time_point{})publishIntervals.push_back(std::chrono::duration<double,std::milli>(now-previousPublish).count());
   if(available)overwriteAges.push_back(std::chrono::duration<double,std::milli>(now-pending.published).count());
  }
  previousPublish=now;if(available)droppedFrames.fetch_add(1);pending.published=now;
  bytes.swap(pending.bytes);packets.swap(pending.packets);pending.count=count;pending.id=id;pending.magic=magic;pending.utc=utc;pending.started=started;pending.stages=stages;pending.stages.published=trace->Enabled()?StageUtcUs():0;available=true;ready.notify_one();
  if(HANDLE port=wakePort.load())PostQueuedCompletionStatus(port,0,1,nullptr);
 }
 void Samples(std::vector<double>&send,std::vector<double>&total,std::vector<double>&submit,std::vector<double>&calls,std::vector<double>&slots,std::vector<double>&prepare){std::lock_guard<std::mutex> lock(metricsMutex);send.swap(sendSamples);total.swap(totalSamples);submit.swap(submitSamples);calls.swap(callSamples);slots.swap(slotSamples);prepare.swap(prepareSamples);}
 void QueueSamples(std::vector<double>&intervals,std::vector<double>&ages){std::lock_guard<std::mutex> lock(metricsMutex);intervals.swap(publishIntervals);ages.swap(overwriteAges);}
 void Stop(){ {std::lock_guard<std::mutex> lock(mutex);stopping=true;}ready.notify_one();capacityReady.notify_all();if(HANDLE port=wakePort.load())PostQueuedCompletionStatus(port,0,1,nullptr);if(worker.joinable())worker.join(); }
 ~DatagramSender(){Stop();}
};

#ifdef PYROSCREEN_DATAGRAM_UNIT_TEST
} // The loopback regression exercises the exact production sender class.
#else
bool check(pyrowave_result result, const char *operation)
{
	if (result == PYROWAVE_SUCCESS)
		return true;
	std::fprintf(stderr, "%s failed: PyroWave result %d\n", operation, int(result));
	return false;
}

double percentile(std::vector<double> values, double p)
{
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	const auto index = size_t(std::ceil(p * double(values.size()))) - 1;
	return values[std::min(index, values.size() - 1)];
}

void update_patch(ID3D11DeviceContext *context, ID3D11Texture2D *texture,
				  std::vector<uint8_t> &patch, int x, uint64_t frame)
{
	constexpr int patchWidth = 192;
	constexpr int patchHeight = 240;
	for (int y = 0; y < patchHeight; ++y)
	{
		for (int px = 0; px < patchWidth; ++px)
		{
			const size_t offset = (size_t(y) * patchWidth + px) * 4;
			const bool border = px < 8 || y < 8 || px >= patchWidth - 8 || y >= patchHeight - 8;
			const bool checker = ((px / 24) ^ (y / 24) ^ int(frame / 16)) & 1;
			patch[offset + 0] = border ? 0 : (checker ? 32 : 240);
			patch[offset + 1] = border ? 255 : (checker ? 240 : 32);
			patch[offset + 2] = border ? 255 : 48;
			patch[offset + 3] = 255;
		}
	}
	D3D11_BOX box = {};
	box.left = UINT(x);
	box.top = 600;
	box.front = 0;
	box.right = UINT(x + patchWidth);
	box.bottom = 600 + patchHeight;
	box.back = 1;
	context->UpdateSubresource(texture, 0, &box, patch.data(), patchWidth * 4, 0);
}
}

int wmain(int argc, wchar_t **argv)
{
	SetProcessDPIAware();
	std::wstring captureDisplay;
	std::wstring sendHost;
	unsigned short sendPort = 55165;
	int measureSeconds = MeasureSeconds;
	int Fps = DefaultFps;
	int CaptureFps = DefaultFps;
 int captureOverride = 0;double linkBudgetMbps=0;bool legacyCapturePacing=false,constantFrameRate=false;
	double captureRefreshHz = 0;
	size_t maxFrameBytes = DefaultMaxFrameBytes;
	size_t packetBoundary = PacketBytes;
	bool chroma444 = false;
	size_t sendWindow = 64;
	bool udpSegmented = false;
	int sendBufferBytes = -1;
	bool sendIocp = false;
	bool sendRio = false;
	bool fec = false;
 bool sendInterleaved=false;
 double sendPacingMbps=0;
 bool captureBackpressure=false;
 bool sendPipelined=false;
 bool sendSkipImmediate=false;
	bool sendContiguous = false;
	bool sendConnected = false;
 std::wstring sendBindHost;
 bool sendDeadlineControl=true;
 bool partialFrames=false;
 bool captureOnUpdate=false;
 bool clientCursor=false;
	bool captureRequested = false;
	bool sendRequested = false;
 bool traceStages=false;
#ifdef PYROSCREEN_TEST_STAGE_TRACE_DEFAULT
 traceStages=true;
#endif
	std::wstring secretFile;
	for (int i = 1; i < argc; ++i)
	{
		if (std::wstring(argv[i]) == L"--capture-display" && i + 1 < argc) { captureRequested = true; captureDisplay = argv[++i]; }
		else if (std::wstring(argv[i]) == L"--send-host" && i + 1 < argc) { sendRequested = true; sendHost = argv[++i]; }
		else if (std::wstring(argv[i]) == L"--width" && i+1<argc) Width=_wtoi(argv[++i]);
		else if (std::wstring(argv[i]) == L"--height" && i+1<argc) Height=_wtoi(argv[++i]);
		else if (std::wstring(argv[i]) == L"--unencrypted-video") encryptedVideo=false;
  else if (std::wstring(argv[i]) == L"--trace-stages") traceStages=true;
		else if (std::wstring(argv[i]) == L"--secret-file" && i+1<argc) secretFile=argv[++i];
		else if (std::wstring(argv[i]) == L"--port" && i + 1 < argc) sendPort = (unsigned short)_wtoi(argv[++i]);
		else if (std::wstring(argv[i]) == L"--target-fps" && i + 1 < argc)
		{
			Fps = _wtoi(argv[++i]);
			if (Fps != 60 && Fps != 120 && Fps != 165 && Fps != 240) return 1;
		}
		else if (std::wstring(argv[i]) == L"--capture-fps" && i+1<argc) {
   captureOverride=_wtoi(argv[++i]);if(captureOverride<1||captureOverride>240)return 1;
  }
  else if (std::wstring(argv[i]) == L"--link-budget-mbps" && i+1<argc) {
   linkBudgetMbps=_wtof(argv[++i]);if(!std::isfinite(linkBudgetMbps)||linkBudgetMbps<1||linkBudgetMbps>100000)return 1;
  }
  else if (std::wstring(argv[i]) == L"--capture-fixed-phase")legacyCapturePacing=true;
  else if (std::wstring(argv[i]) == L"--capture-catch-up")legacyCapturePacing=false;
  else if (std::wstring(argv[i]) == L"--constant-frame-rate")constantFrameRate=true;
  else if (std::wstring(argv[i]) == L"--capture-backpressure")captureBackpressure=true;
  else if (std::wstring(argv[i]) == L"--send-pipelined")sendPipelined=true;
  else if (std::wstring(argv[i]) == L"--send-skip-immediate")sendSkipImmediate=true;
  else if (std::wstring(argv[i]) == L"--measure-seconds" && i + 1 < argc)
		{
			const int requested = _wtoi(argv[++i]);
			measureSeconds = requested == 0 ? 0 : std::max(1, std::min(3600, requested));
		}
		else if (std::wstring(argv[i]) == L"--max-frame-bytes" && i + 1 < argc) maxFrameBytes = size_t(std::max(65536, std::min(4*1048576, _wtoi(argv[++i]))));
		else if (std::wstring(argv[i]) == L"--packet-bytes" && i + 1 < argc) packetBoundary = size_t(std::max(1380, std::min(32768, _wtoi(argv[++i]))));
		else if (std::wstring(argv[i]) == L"--chroma444") chroma444 = true;
		else if (std::wstring(argv[i]) == L"--send-window" && i+1<argc) sendWindow=size_t(std::max(16,std::min(1024,_wtoi(argv[++i]))));
		else if (std::wstring(argv[i]) == L"--udp-segmented") udpSegmented=true;
		else if (std::wstring(argv[i]) == L"--send-buffer-bytes" && i+1<argc) sendBufferBytes=std::max(0,std::min(4*1024*1024,_wtoi(argv[++i])));
		else if (std::wstring(argv[i]) == L"--send-iocp") sendIocp=true;
		else if (std::wstring(argv[i]) == L"--send-rio") sendRio=true;
		else if (std::wstring(argv[i]) == L"--fec") fec=true;
  else if (std::wstring(argv[i]) == L"--send-interleaved") sendInterleaved=true;
  else if (std::wstring(argv[i]) == L"--send-pacing-mbps" && i+1<argc) {sendPacingMbps=_wtof(argv[++i]);if(!std::isfinite(sendPacingMbps)||sendPacingMbps<0||sendPacingMbps>100000)return 1;}
		else if (std::wstring(argv[i]) == L"--send-contiguous") sendContiguous=true;
		else if (std::wstring(argv[i]) == L"--send-connected") sendConnected=true;
  else if (std::wstring(argv[i]) == L"--send-bind-host" && i+1<argc)sendBindHost=argv[++i];
  else if (std::wstring(argv[i]) == L"--no-send-deadline-control") sendDeadlineControl=false;
  else if (std::wstring(argv[i]) == L"--partial-frames") partialFrames=true;
  else if (std::wstring(argv[i]) == L"--capture-on-update") captureOnUpdate=true;
  else if (std::wstring(argv[i]) == L"--client-cursor") clientCursor=true;
		else { std::fprintf(stderr, "invalid arguments\n"); return 1; }
	}
#ifdef PYROSCREEN_TEST_RIO_DEFAULT
 // Development comparison build; never enabled in portable release packaging.
 sendRio=true;sendIocp=false;udpSegmented=false;sendConnected=false;
#endif
	if(Width<640||Width>3840||Height<480||Height>2160||Width%2||Height%2||secretFile.empty()||!captureRequested||!sendRequested)return 1;
	try{videoCrypto.SetEncrypted(encryptedVideo);videoCrypto.Load(secretFile.c_str());}catch(const std::exception&e){fprintf(stderr,"%s\n",e.what());return 1;}
	sendContiguous=!sendRio;
	if ((captureRequested || sendRequested) && (captureDisplay.empty() || sendHost.empty()))
	{
		std::fprintf(stderr, "desktop capture and UDP send must both be configured; refusing to run the synthetic benchmark\n");
		return 1;
	}
	CaptureFps = captureOverride>0?std::min(Fps,captureOverride):Fps;
	SOCKET udp = INVALID_SOCKET;
	sockaddr_in remote = {};
	if (!sendHost.empty())
	{
		WSADATA wsa = {}; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
		if(sendRio&&(sendIocp||udpSegmented)){std::fprintf(stderr,"RIO must be tested independently of IOCP/segmentation\n");return 1;}
		if(sendContiguous&&sendRio){std::fprintf(stderr,"contiguous slots must be tested independently of RIO/segmentation\n");return 1;}
		if(sendConnected&&sendRio){std::fprintf(stderr,"connected sends must be tested independently of RIO/segmentation\n");return 1;}
		udp = sendRio?WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_REGISTERED_IO):socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		remote.sin_family = AF_INET; remote.sin_port = htons(sendPort);
		if (udp == INVALID_SOCKET || InetPtonW(AF_INET, sendHost.c_str(), &remote.sin_addr) != 1) { std::fprintf(stderr, "bad UDP destination\n"); return 1; }
  if(!sendBindHost.empty()){
   sockaddr_in local{};local.sin_family=AF_INET;
   if(InetPtonW(AF_INET,sendBindHost.c_str(),&local.sin_addr)!=1||bind(udp,reinterpret_cast<sockaddr*>(&local),sizeof(local))){std::fprintf(stderr,"UDP source bind failed: %d\n",WSAGetLastError());return 1;}
  }
		if(sendConnected&&connect(udp,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))!=0){std::fprintf(stderr,"UDP connect failed: %d\n",WSAGetLastError());return 1;}
		if(udpSegmented && WSASetUdpSendMessageSize(udp,DWORD(WireDatagramBytes()))!=0){std::fprintf(stderr,"UDP segmentation unavailable: %d\n",WSAGetLastError());return 1;}
		if(sendBufferBytes>=0 && setsockopt(udp,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&sendBufferBytes),sizeof(sendBufferBytes))!=0){std::fprintf(stderr,"UDP send buffer unavailable: %d\n",WSAGetLastError());return 1;}
		int optionBytes=sizeof(sendBufferBytes);getsockopt(udp,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&sendBufferBytes),&optionBytes);
		std::fprintf(stderr,"UDP send buffer: %d bytes; segmentation: %s; window: %llu\n",sendBufferBytes,udpSegmented?"enabled":"disabled",(unsigned long long)sendWindow);
	}

	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return 2;
	ComPtr<IDXGIAdapter1> adapter;
	DXGI_ADAPTER_DESC1 adapterDesc = {};
	for(UINT index=0;;++index)
	{
		ComPtr<IDXGIAdapter1> candidate;
		if(factory->EnumAdapters1(index,&candidate)==DXGI_ERROR_NOT_FOUND)break;
		DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);
		for(UINT o=0;;++o)
		{
			ComPtr<IDXGIOutput> output;
			if(candidate->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;
			DXGI_OUTPUT_DESC od{};
			if(SUCCEEDED(output->GetDesc(&od))&&captureDisplay==od.DeviceName){adapter=candidate;adapterDesc=desc;break;}
		}
		if(adapter)break;
	}
	if (!adapter)
	{
		std::fprintf(stderr, "The selected desktop output has no DXGI capture adapter\n");
		return 3;
	}

	ComPtr<IDXGIOutput1> captureOutput;
	std::wstring captureDisplayText = L"synthetic_texture";
	if (!captureDisplay.empty())
	{
		for (UINT outputIndex = 0; ; ++outputIndex)
		{
			ComPtr<IDXGIOutput> candidate;
			if (adapter->EnumOutputs(outputIndex, &candidate) == DXGI_ERROR_NOT_FOUND)
				break;
			DXGI_OUTPUT_DESC outputDesc = {};
			if (SUCCEEDED(candidate->GetDesc(&outputDesc)) && captureDisplay == outputDesc.DeviceName)
			{
				if (FAILED(candidate.As(&captureOutput)))
					return 3;
				captureDisplayText = outputDesc.DeviceName;
				break;
			}
		}
		if (!captureOutput)
		{
			std::fwprintf(stderr, L"DXGI output %ls is not attached to the selected capture adapter\n", captureDisplay.c_str());
			return 3;
		}
	}

	ComPtr<ID3D11Device> d3dDevice;
	ComPtr<ID3D11DeviceContext> d3dContext;
	ComPtr<ID3D11Device5> d3dDevice5;
	ComPtr<ID3D11DeviceContext4> d3dContext4;
	const HRESULT d3dResult = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
											D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
											&d3dDevice, nullptr, &d3dContext);
	if (FAILED(d3dResult) || FAILED(d3dDevice.As(&d3dDevice5)) || FAILED(d3dContext.As(&d3dContext4)))
	{
		std::fprintf(stderr, "Failed to create D3D11.4 device/context: 0x%08lx\n", (unsigned long)d3dResult);
		return 4;
	}
	ComPtr<IDXGIOutputDuplication> duplication;
	if (captureOutput)
	{
		if (FAILED(captureOutput->DuplicateOutput(d3dDevice.Get(), &duplication)))
		{
			std::fwprintf(stderr, L"Desktop Duplication could not open %ls\n", captureDisplay.c_str());
			return 4;
		}
		DXGI_OUTDUPL_DESC dupDesc = {};
		duplication->GetDesc(&dupDesc);
		if (dupDesc.ModeDesc.Width != Width || dupDesc.ModeDesc.Height != Height ||
		    dupDesc.ModeDesc.RefreshRate.Denominator == 0 ||
		    dupDesc.ModeDesc.RefreshRate.Numerator == 0)
		{
			std::fprintf(stderr, "Capture output does not match the selected mode\n");
			return 4;
		}
		captureRefreshHz = double(dupDesc.ModeDesc.RefreshRate.Numerator) / dupDesc.ModeDesc.RefreshRate.Denominator;
		if(!constantFrameRate)CaptureFps = std::max(1,std::min(CaptureFps,int(std::round(captureRefreshHz))));
		std::fprintf(stderr, "Desktop capture mode: %dx%d at %.3f Hz; requested ceiling: %d FPS\n", Width, Height, captureRefreshHz, Fps);
	}

	pyrowave_device device = nullptr;
	if (!check(pyrowave_create_device_by_compat(0, 0, nullptr, nullptr,
						reinterpret_cast<const pyrowave_luid *>(&adapterDesc.AdapterLuid), VK_QUEUE_GLOBAL_PRIORITY_MEDIUM, &device), "create matching Vulkan device"))
		return 5;

	D3D11_TEXTURE2D_DESC textureDesc = {};
	textureDesc.Width = Width;
	textureDesc.Height = Height;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	ComPtr<ID3D11Texture2D> texture;
	DesktopPointer desktopPointer;
 // Client cursor: the pointer is not composed into video; a separate socket
 // sends sealed position/shape datagrams that bypass the frame pipeline.
 using CursorSeal=size_t(*)(const void*,size_t,const void*,size_t,unsigned char*);
 std::unique_ptr<RemoteCursorSender<CursorSeal>> remoteCursor;
 if(clientCursor&&duplication&&udp!=INVALID_SOCKET){
  DXGI_OUTPUT_DESC od{};captureOutput->GetDesc(&od);
  SOCKET cursorSocket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  sockaddr_in local{};local.sin_family=AF_INET;
  if(cursorSocket==INVALID_SOCKET||(!sendBindHost.empty()&&(InetPtonW(AF_INET,sendBindHost.c_str(),&local.sin_addr)!=1||bind(cursorSocket,reinterpret_cast<sockaddr*>(&local),sizeof(local))))){std::fprintf(stderr,"Cursor socket failed: %d\n",WSAGetLastError());return 1;}
  remoteCursor.reset(new RemoteCursorSender<CursorSeal>(cursorSocket,remote,od.DesktopCoordinates,
   [](const void*h,size_t hn,const void*p,size_t pn,unsigned char*out){return videoCrypto.Seal(h,hn,p,pn,out);}));
 }
	if (FAILED(d3dDevice->CreateTexture2D(&textureDesc, nullptr, &texture)))
	{
		std::fprintf(stderr, "Failed to create shareable BGRA texture\n");
		return 6;
	}
	ComPtr<IDXGIResource1> textureResource;
	if (FAILED(texture.As(&textureResource)))
		return 7;
	HANDLE textureHandle = nullptr;
	if (FAILED(textureResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &textureHandle)))
	{
		std::fprintf(stderr, "D3D11 BGRA shared texture handle is unavailable\n");
		return 8;
	}

	ComPtr<ID3D11Fence> fence;
	HANDLE fenceHandle = nullptr;
	if (FAILED(d3dDevice5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
	    FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle)))
	{
		std::fprintf(stderr, "D3D11 shared fence is unavailable\n");
		return 9;
	}

	pyrowave_image image = nullptr;
	VkImageCreateInfo vkImageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	vkImageInfo.imageType = VK_IMAGE_TYPE_2D;
	vkImageInfo.extent = { uint32_t(Width), uint32_t(Height), 1 };
	vkImageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	vkImageInfo.mipLevels = 1;
	vkImageInfo.arrayLayers = 1;
	vkImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	vkImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	vkImageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	vkImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	pyrowave_image_create_info imageInfo = {};
	imageInfo.device = device;
	imageInfo.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	imageInfo.external_handle = reinterpret_cast<pyrowave_os_handle>(textureHandle);
	imageInfo.image_create_info = &vkImageInfo;
	if (!check(pyrowave_image_create(&imageInfo, &image), "import D3D11 texture into Vulkan"))
		return 10;

	pyrowave_sync_object sync = nullptr;
	pyrowave_sync_object_create_info syncInfo = {};
	syncInfo.device = device;
	syncInfo.external_handle = reinterpret_cast<pyrowave_os_handle>(fenceHandle);
	syncInfo.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
	syncInfo.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
	if (!check(pyrowave_sync_object_create(&syncInfo, &sync), "import D3D11 timeline fence"))
		return 11;

	pyrowave_encoder_create_info encoderInfo = {};
	encoderInfo.device = device;
	encoderInfo.width = Width;
	encoderInfo.height = Height;
	encoderInfo.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
	pyrowave_encoder encoder = nullptr;
	if (!check(pyrowave_encoder_create(&encoderInfo, &encoder), "create 2K encoder"))
		return 12;
	pyrowave_image_view inputView = {};
	if (!check(pyrowave_image_get_image_view(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &inputView), "get texture view"))
		return 13;
	pyrowave_scaled_encode_info scaleInfo = {};
	scaleInfo.view = inputView;
	scaleInfo.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	scaleInfo.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	scaleInfo.intermediate_plane_format = VK_FORMAT_R8_UNORM;
	scaleInfo.ycbcr_chroma_midpoint = 128.0f / 255.0f;
	pyrowave_gpu_external_reference externalImage = { image, VK_QUEUE_FAMILY_EXTERNAL };
	pyrowave_gpu_sync_operation acquire = {};
	acquire.images = &externalImage;
	acquire.num_images = 1;
	acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(sync);
	pyrowave_gpu_sync_operation release = {};
	release.sync.semaphore = pyrowave_sync_object_get_semaphore(sync);

 // Partial frames: changed regions are encoded 1:1 from the captured texture
 // with crop_rect by a cached encoder per size class. Anything that fits no
 // class, lacks damage metadata or follows a sender-queue drop is a full frame.
 if(partialFrames&&!duplication){std::fprintf(stderr,"--partial-frames requires desktop capture\n");return 1;}
 struct PartialEncoder{int w=0,h=0;pyrowave_encoder encoder=nullptr;};
 std::vector<PartialEncoder> partialEncoders;
 auto partialEncoderFor=[&](int w,int h)->pyrowave_encoder{
  for(auto&e:partialEncoders)if(e.w==w&&e.h==h)return e.encoder;
  pyrowave_encoder_create_info info=encoderInfo;info.width=w;info.height=h;pyrowave_encoder created=nullptr;
  const auto createStart=Clock::now();
  if(!check(pyrowave_encoder_create(&info,&created),"create partial encoder"))return nullptr;
  std::fprintf(stderr,"Partial encoder %dx%d created in %.1f ms\n",w,h,std::chrono::duration<double,std::milli>(Clock::now()-createStart).count());
  partialEncoders.push_back({w,h,created});return created;
 };
 // The engine copies every block named in the GPU-written block table without bounds checks
 // (its asserts are compiled out of release builds), so one corrupt entry becomes a wild memcpy
 // and kills the sender. A block is at most 4095 payload words plus an 8-byte header.
 uint64_t rejectedFrames=0;
 auto blockTableProblem=[](const void*table,size_t tableBytes,size_t bitstreamBytes,size_t outputBytes,char*text,size_t textBytes)->bool{
  struct Entry{uint32_t offset,words;};
  const Entry*e=static_cast<const Entry*>(table);const size_t blocks=table?tableBytes/sizeof(Entry):0;
  const uint64_t bitstreamWords=bitstreamBytes/4;uint64_t total=64; // sequence header
  if(!blocks){std::snprintf(text,textBytes,"empty block table");return true;}
  // The sequence number is read through block 0 even when that block carries no data.
  if(uint64_t(e[0].offset)+2>bitstreamWords){std::snprintf(text,textBytes,"block 0 header offset %u beyond %llu words",e[0].offset,(unsigned long long)bitstreamWords);return true;}
  for(size_t i=0;i<blocks;++i){
   if(!e[i].words)continue;
   if(e[i].words<2||e[i].words>8192||uint64_t(e[i].offset)+e[i].words>bitstreamWords){
    std::snprintf(text,textBytes,"block %zu of %zu offset %u words %u in %llu words",i,blocks,e[i].offset,e[i].words,(unsigned long long)bitstreamWords);return true;}
   total+=uint64_t(e[i].words)*4;
  }
  if(total>outputBytes){std::snprintf(text,textBytes,"%llu bytes exceed the %zu byte output buffer",(unsigned long long)total,outputBytes);return true;}
  return false;
 };
 // Full-width strips (scrolling rows, wide animations) get their own class.
 const int PartialWidths[]={256,512,768,1024,1536,2048,Width},PartialHeights[]={128,256,384,512,640,768,896,1024};
 auto unite=[](RECT&into,bool&valid,const RECT&r){
  if(r.right<=r.left||r.bottom<=r.top)return;
  if(!valid){into=r;valid=true;return;}
  into.left=std::min(into.left,r.left);into.top=std::min(into.top,r.top);into.right=std::max(into.right,r.right);into.bottom=std::max(into.bottom,r.bottom);
 };
 std::vector<uint8_t> damageMetadata;
 RECT frameDamage{};bool frameDamageValid=false,damageUnknown=true;
 RECT damageHistory[2]{};bool damageHistoryValid[2]{};
 RECT lastPointer{};bool lastPointerValid=false;
 uint64_t lastFullFrame=0,lastSenderDrops=0;
 uint64_t totalPartialFrames=0,totalFullFrames=0;std::vector<double> livePartialArea;
 // Why a full frame was sent: no damage metadata, periodic refresh, sender drop, or damage too large.
 uint64_t fullUnknown=0,fullPeriodic=0,fullDropped=0,fullLarge=0,damageWithoutMetadata=0;std::vector<double> liveDamageArea;

	std::vector<uint8_t> initial(size_t(Width) * Height * 4);
	const uint8_t bars[8][3] = {{24,24,24},{24,24,235},{24,235,24},{24,235,235},{235,24,24},{235,24,235},{235,235,24},{235,235,235}};
	for (int y = 0; y < Height; ++y)
	{
		for (int x = 0; x < Width; ++x)
		{
			const int bar = std::min(7, x * 8 / Width);
			const int shade = ((y / 240) & 1) ? 0 : -16;
			const size_t offset = (size_t(y) * Width + x) * 4;
			initial[offset + 0] = uint8_t(std::max(0, std::min(255, int(bars[bar][0]) + shade)));
			initial[offset + 1] = uint8_t(std::max(0, std::min(255, int(bars[bar][1]) + shade)));
			initial[offset + 2] = uint8_t(std::max(0, std::min(255, int(bars[bar][2]) + shade)));
			initial[offset + 3] = 255;
		}
	}
	d3dContext->UpdateSubresource(texture.Get(), 0, nullptr, initial.data(), Width * 4, 0);
	std::vector<uint8_t> patch(192 * 240 * 4);
	std::vector<uint8_t> bitstream(maxFrameBytes + 65536);
	std::vector<pyrowave_packet> packets(maxFrameBytes / PacketBytes + 2048);
 wchar_t traceRuntime[32768]{};DWORD traceLength=GetEnvironmentVariableW(L"PYROSCREEN_RUNTIME",traceRuntime,32768);
 StageTraceWriter stageTrace(traceStages&&traceLength&&traceLength<32768?std::wstring(traceRuntime)+L"\\sender-stages.jsonl":L"");
	std::unique_ptr<DatagramSender> networkSender;
	if(sendPacingMbps>0&&(udpSegmented||sendRio)){std::fprintf(stderr,"Datagram pacing requires the normal IOCP transport\n");return 1;}
	if (udp != INVALID_SOCKET) networkSender.reset(new DatagramSender(udp, remote, bitstream.size(), packets.size(),sendWindow,udpSegmented,sendIocp,sendRio,fec,sendInterleaved,sendPacingMbps,sendContiguous,sendConnected,sendPipelined,sendSkipImmediate,&stageTrace));
	pyrowave_rate_control rate = { maxFrameBytes };
 size_t sendDeadlineBudget=maxFrameBytes;double sendMedianMs=0;
 const double wireBytesPerFrame=linkBudgetMbps>0?linkBudgetMbps*1000000.0/8.0/CaptureFps:0;
 double lastEstimatedWireMbps=0;
	uint64_t timeline = 0;
	std::vector<double> captureMs, encodeMs, packetizeMs, totalMs;
	const auto start = Clock::now();
	const auto measureStart = start + std::chrono::seconds(measureSeconds > 0 ? WarmupSeconds : 0);
	const auto end = measureSeconds > 0 ? start + std::chrono::seconds(WarmupSeconds + measureSeconds) : Clock::time_point::max();
	uint64_t packetBytes = 0;
	uint64_t udpBytes = 0;
	uint64_t measuredFrames = 0;
	uint64_t captureTimeouts = 0;
	uint64_t liveCaptureFrames = 0, liveCaptureTimeouts = 0, liveUdpFrames = 0, liveUdpBytes = 0;
 uint64_t totalEncodedFrames=0,totalEncodedBytes=0;
 std::vector<double> liveEncodedBytes;
 uint64_t liveRepeatedFrames=0;bool haveDesktopFrame=false;uint64_t lastDesktopUtcMs=0;Clock::time_point lastDesktopSourceStart{};
	std::vector<double> liveCaptureMs, liveEncodeMs, liveFrameToUdpMs;
	std::vector<double> livePacketizeMs, liveSendMs, liveSourceAgeMs,liveGpuReadbackMs,liveCpuPacketizeMs,livePacingWaitMs;
 std::vector<double> liveBackpressureMs,liveSubmitMs,liveCallMs,liveSlotMs,livePrepareMs;uint64_t totalBackpressureFrames=0;
 std::vector<double> livePublishIntervals,liveOverwriteAges;
 uint64_t liveAccumulatedSourceFrames=0;
	auto liveReportStart = start;
 wchar_t runtimeBuffer[32768]{};DWORD runtimeLength=GetEnvironmentVariableW(L"PYROSCREEN_RUNTIME",runtimeBuffer,32768);
 std::wstring liveStatsPath;
 if(runtimeLength&&runtimeLength<32768)liveStatsPath=std::wstring(runtimeBuffer)+L"\\sender-live.json";
 else{wchar_t legacy[MAX_PATH]{};DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",legacy,MAX_PATH);liveStatsPath=length&&length<MAX_PATH?std::wstring(legacy)+L"\\PyroScreen\\runtime\\sender-live.json":L"sender-live.json";}
	PyrowaveLiveStatsWriter liveStatsWriter(liveStatsPath);
	auto writeLiveStats = [&]() {
		const auto now = Clock::now();
		const double interval = std::chrono::duration<double>(now - liveReportStart).count();
		if (interval < 0.95)
			return;
		if(networkSender){liveUdpFrames=networkSender->sentFrames.exchange(0);liveUdpBytes=networkSender->sentBytes.exchange(0);networkSender->Samples(liveSendMs,liveFrameToUdpMs,liveSubmitMs,liveCallMs,liveSlotMs,livePrepareMs);}
  if(networkSender)networkSender->QueueSamples(livePublishIntervals,liveOverwriteAges);
  sendMedianMs=percentile(liveSendMs,0.5);
  if(sendDeadlineControl&&networkSender&&!liveSendMs.empty()){
   const double periodMs=1000.0/CaptureFps;
   // Fixed cadence needs headroom for slower frames, not just the median.
   // The 90th percentile still ignores isolated stalls. Adapt frame quality,
   // never the selected FPS, to keep the bounded mailbox from overwriting.
   const auto& deadlineSamples=(sendPipelined&&sendIocp&&sendContiguous&&!udpSegmented&&!sendRio)?liveSubmitMs:liveSendMs;
   const double controlSendMs=constantFrameRate?percentile(deadlineSamples,0.90):percentile(deadlineSamples,0.50);
   if(controlSendMs>periodMs*0.95){
    size_t activeBudget=std::min(sendDeadlineBudget,size_t(rate.maximum_bitstream_size));
    sendDeadlineBudget=std::max<size_t>(65536,size_t(activeBudget*std::max(0.90,periodMs*0.90/controlSendMs)))&~size_t(3);
   }else if(controlSendMs<periodMs*0.80&&sendDeadlineBudget<maxFrameBytes){
    sendDeadlineBudget=std::min(maxFrameBytes,sendDeadlineBudget+std::max<size_t>(256,sendDeadlineBudget/40))&~size_t(3);
   }
   rate.maximum_bitstream_size=std::min(size_t(rate.maximum_bitstream_size),sendDeadlineBudget);
  }
		char json[8192];
		int length = std::snprintf(json, sizeof(json),
			"{\"pid\":%lu,\"elapsed_s\":%.3f,\"interval_s\":%.3f,\"capture_fps\":%.3f,\"capture_timeouts_per_s\":%.3f,\"udp_send_fps\":%.3f,\"udp_mbps\":%.3f,\"capture_p95_ms\":%.3f,\"encode_p50_ms\":%.3f,\"encode_p95_ms\":%.3f,\"frame_to_udp_p95_ms\":%.3f,\"max_frame_bytes\":%llu,\"packet_boundary\":%llu,\"packetize_p95_ms\":%.3f,\"udp_send_p95_ms\":%.3f,\"source_age_p95_ms\":%.3f,\"send_window\":%llu,\"network_queue_dropped_frames\":%llu,\"send_buffer_bytes\":%d,\"udp_segmented\":%s}\n",
			GetCurrentProcessId(), std::chrono::duration<double>(now - start).count(), interval,
			double(liveCaptureFrames) / interval, double(liveCaptureTimeouts) / interval,
			double(liveUdpFrames) / interval, double(liveUdpBytes) * 8.0 / interval / 1000000.0,
			percentile(liveCaptureMs, 0.95), percentile(liveEncodeMs, 0.50), percentile(liveEncodeMs, 0.95),
			percentile(liveFrameToUdpMs, 0.95), (unsigned long long)maxFrameBytes,
			(unsigned long long)packetBoundary, percentile(livePacketizeMs, 0.95), percentile(liveSendMs, 0.95), percentile(liveSourceAgeMs, 0.95),
			(unsigned long long)sendWindow, (unsigned long long)(networkSender ? networkSender->droppedFrames.load() : 0),sendBufferBytes,udpSegmented?"true":"false");
		if(length>1 && length<int(sizeof(json))){
			length-=2;
			length+=std::snprintf(json+length,sizeof(json)-length,",\"send_transport\":\"%s\",\"encrypted_video\":%s,\"fec\":%s,\"send_contiguous\":%s,\"send_connected\":%s,\"target_fps\":%d,\"capture_refresh_hz\":%.3f,\"capture_limit_fps\":%d,\"capture_rate_limited\":%s}\n",sendRio?"rio":sendIocp?"iocp":"events",encryptedVideo?"true":"false",fec?"true":"false",sendContiguous?"true":"false",sendConnected?"true":"false",Fps,captureRefreshHz,CaptureFps,duplication?"true":"false");
		}
  if(length>1&&length<int(sizeof(json))){
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"gpu_readback_p95_ms\":%.3f,\"cpu_packetize_p95_ms\":%.3f,\"capture_pacing_wait_p95_ms\":%.3f,\"source_accumulated_frames\":%llu,\"requested_frame_bytes\":%llu,\"effective_frame_bytes\":%llu,\"link_budget_mbps\":%.3f,\"estimated_wire_mbps\":%.3f,\"capture_pacing_mode\":\"%s\"}\n",percentile(liveGpuReadbackMs,0.95),percentile(liveCpuPacketizeMs,0.95),percentile(livePacingWaitMs,0.95),(unsigned long long)liveAccumulatedSourceFrames,(unsigned long long)maxFrameBytes,(unsigned long long)rate.maximum_bitstream_size,linkBudgetMbps,lastEstimatedWireMbps,legacyCapturePacing?"fixed_phase":"bounded_catch_up");
  }
  if(length>1&&length<int(sizeof(json))){
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"send_deadline_control\":%s,\"send_deadline_budget_bytes\":%llu,\"udp_send_p50_ms\":%.3f}\n",sendDeadlineControl?"true":"false",(unsigned long long)sendDeadlineBudget,sendMedianMs);
  }
  if(length>1&&length<int(sizeof(json))){
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"frame_rate_policy\":\"%s\",\"capture_on_update\":%s,\"repeated_capture_fps\":%.3f,\"capture_timestamp_kind\":\"%s\"}\n",constantFrameRate?"constant":"desktop_updates",captureOnUpdate?"true":"false",double(liveRepeatedFrames)/interval,"desktop_update");
  }
		if(length>1&&length<int(sizeof(json))){
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"encoded_frames_total\":%llu,\"encoded_bytes_total\":%llu,\"udp_sent_frames_total\":%llu,\"udp_sent_bytes_total\":%llu,\"encoded_frame_bytes_p50\":%.0f,\"encoded_frame_bytes_p95\":%.0f,\"encoded_frame_bytes_max\":%.0f,\"sampled_utc_ms\":%llu,\"fec_interleaved\":%s,\"send_pacing_mbps\":%.3f,\"capture_backpressure\":%s,\"capture_backpressure_frames_total\":%llu,\"capture_backpressure_wait_p95_ms\":%.3f,\"process_priority_class\":%lu,\"send_iocp_pipelined\":%s,\"udp_submit_p95_ms\":%.3f,\"udp_submit_max_ms\":%.3f,\"udp_completion_max_ms\":%.3f,\"udp_call_max_ms\":%.3f,\"udp_slot_wait_max_ms\":%.3f,\"udp_prepare_gap_max_ms\":%.3f,\"udp_slot_reuse_bypasses_total\":%llu,\"udp_skip_immediate_enabled\":%s,\"udp_immediate_sends_total\":%llu,\"udp_pending_sends_total\":%llu}\n",(unsigned long long)totalEncodedFrames,(unsigned long long)totalEncodedBytes,(unsigned long long)(networkSender?networkSender->totalSentFrames.load():0),(unsigned long long)(networkSender?networkSender->totalBytes.load():0),percentile(liveEncodedBytes,0.50),percentile(liveEncodedBytes,0.95),percentile(liveEncodedBytes,1.0),(unsigned long long)UtcMs(),fec&&sendInterleaved?"true":"false",sendPacingMbps,captureBackpressure?"true":"false",(unsigned long long)totalBackpressureFrames,percentile(liveBackpressureMs,0.95),GetPriorityClass(GetCurrentProcess()),sendPipelined&&sendIocp&&sendContiguous&&!udpSegmented&&!sendRio?"true":"false",percentile(liveSubmitMs,0.95),percentile(liveSubmitMs,1.0),percentile(liveSendMs,1.0),percentile(liveCallMs,1.0),percentile(liveSlotMs,1.0),percentile(livePrepareMs,1.0),(unsigned long long)(networkSender?networkSender->slotReuseBypasses.load():0),networkSender&&networkSender->immediateCompletionEnabled.load()?"true":"false",(unsigned long long)(networkSender?networkSender->immediateSends.load():0),(unsigned long long)(networkSender?networkSender->pendingSends.load():0));
  }
  if(length>1&&length<int(sizeof(json))){
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"publish_interval_min_ms\":%.3f,\"publish_interval_p95_ms\":%.3f,\"publish_interval_max_ms\":%.3f,\"overwrite_pending_age_p50_ms\":%.3f,\"overwrite_pending_age_max_ms\":%.3f}\n",livePublishIntervals.empty()?0:*std::min_element(livePublishIntervals.begin(),livePublishIntervals.end()),percentile(livePublishIntervals,0.95),percentile(livePublishIntervals,1.0),percentile(liveOverwriteAges,0.50),percentile(liveOverwriteAges,1.0));
  }
  if(length>1&&length<int(sizeof(json))){
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"udp_buffer_pressure_retries_total\":%llu}\n",(unsigned long long)(networkSender?networkSender->bufferPressureRetries.load():0));
  }
  if(length>1&&length<int(sizeof(json))){
   double areaMean=0;for(double a:livePartialArea)areaMean+=a;if(!livePartialArea.empty())areaMean/=double(livePartialArea.size());
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"partial_frames_enabled\":%s,\"partial_frames_total\":%llu,\"full_frames_total\":%llu,\"partial_frames_live\":%llu,\"partial_area_ratio_mean\":%.4f,\"partial_encoders\":%llu,\"rejected_encoder_frames_total\":%llu}\n",partialFrames?"true":"false",(unsigned long long)totalPartialFrames,(unsigned long long)totalFullFrames,(unsigned long long)livePartialArea.size(),areaMean,(unsigned long long)partialEncoders.size(),(unsigned long long)rejectedFrames);
  }
  if(length>1&&length<int(sizeof(json))){
   double damageMean=0,damageMax=0;for(double a:liveDamageArea){damageMean+=a;damageMax=std::max(damageMax,a);}if(!liveDamageArea.empty())damageMean/=double(liveDamageArea.size());
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"full_reason_unknown_total\":%llu,\"full_reason_periodic_total\":%llu,\"full_reason_dropped_total\":%llu,\"full_reason_large_total\":%llu,\"damage_without_metadata_total\":%llu,\"damage_area_ratio_mean\":%.4f,\"damage_area_ratio_max\":%.4f,\"damage_samples_live\":%llu}\n",(unsigned long long)fullUnknown,(unsigned long long)fullPeriodic,(unsigned long long)fullDropped,(unsigned long long)fullLarge,(unsigned long long)damageWithoutMetadata,damageMean,damageMax,(unsigned long long)liveDamageArea.size());
  }
  if(length>1&&length<int(sizeof(json))){
   length-=2;
   length+=std::snprintf(json+length,sizeof(json)-length,",\"client_cursor\":%s,\"cursor_positions_sent_total\":%llu,\"cursor_shapes_sent_total\":%llu,\"cursor_send_errors_total\":%llu,\"cursor_shape_queries\":%u,\"cursor_last_shape_hr\":%u,\"cursor_last_shape_size\":%u}\n",remoteCursor?"true":"false",(unsigned long long)(remoteCursor?remoteCursor->Positions():0),(unsigned long long)(remoteCursor?remoteCursor->Shapes():0),(unsigned long long)(remoteCursor?remoteCursor->Errors():0),remoteCursor?remoteCursor->shapeQueries.load():0u,remoteCursor?remoteCursor->lastShapeResult.load():0u,remoteCursor?remoteCursor->lastShapeSize.load():0u);
  }
  livePartialArea.clear();liveDamageArea.clear();
		if(length>0&&length<int(sizeof(json)))liveStatsWriter.Publish(json,size_t(length));
  liveEncodedBytes.clear();
		liveCaptureFrames = liveCaptureTimeouts = liveUdpFrames = liveUdpBytes = 0;liveRepeatedFrames=0;
		liveCaptureMs.clear(); liveEncodeMs.clear(); liveFrameToUdpMs.clear();
		livePacketizeMs.clear();liveGpuReadbackMs.clear();liveCpuPacketizeMs.clear();livePacingWaitMs.clear();liveAccumulatedSourceFrames=0; liveSendMs.clear(); liveSourceAgeMs.clear();
   liveBackpressureMs.clear();liveSubmitMs.clear();liveCallMs.clear();liveSlotMs.clear();livePrepareMs.clear();
   livePublishIntervals.clear();liveOverwriteAges.clear();
		liveReportStart = now;
  if(stageTrace.Enabled())pyrowave_device_report_performance_stats(device,StageProfileMessage,&stageTrace,false);
	};
	const uint64_t totalFrames = measureSeconds > 0 ? uint64_t(Fps) * uint64_t(WarmupSeconds + measureSeconds) : UINT64_MAX;
	// DWM may compose this virtual output at another monitor's refresh rate.
	// Lower profiles must limit real capture/encode work, not just set a mode.
	// A fixed phase and a high-resolution wait keep timer error from accumulating;
	// Constant cadence refreshes the latest snapshot even without desktop damage.
	// Freshness mailboxes still prevent overload from building a latency queue.
	HANDLE captureTimer = nullptr;
	const bool limitCapture = bool(duplication);
	if (limitCapture)
	{
		captureTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!captureTimer) captureTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
		if (!captureTimer) { std::fprintf(stderr, "capture pacing timer unavailable: %lu\n", GetLastError()); return 23; }
	}
	const auto capturePeriod = std::chrono::nanoseconds(1000000000ll / CaptureFps);
	auto nextCapture = Clock::now();
	auto lastCaptureStart = Clock::time_point{};
	for (uint64_t frame = 0; frame < totalFrames && (!duplication || Clock::now() < end); ++frame)
	{
		writeLiveStats();
		if(networkSender&&networkSender->error.load()){std::fprintf(stderr,"UDP send failed: %d\n",networkSender->error.load());return 22;}
		const auto pacingBegin=Clock::now();
  // Update-driven cadence: wait inside AcquireNextFrame until the deadline, so
  // a desktop update is captured immediately instead of at the next fixed
  // phase; the deadline still sends a repeated snapshot to keep the FPS.
  const bool updateDriven=captureOnUpdate&&constantFrameRate&&haveDesktopFrame&&duplication;
  // Pointer-only updates can arrive at the mouse polling rate; keep at least
  // 0.8 of the frame period between captures so the rate stays bounded.
  const auto earliestCapture=lastCaptureStart+capturePeriod*4/5;
  if (limitCapture && updateDriven && earliestCapture > Clock::now())
  {
   LARGE_INTEGER due{};
   due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(earliestCapture - Clock::now()).count() / 100);
   if (!SetWaitableTimer(captureTimer, &due, 0, nullptr, nullptr, FALSE) || WaitForSingleObject(captureTimer, INFINITE) != WAIT_OBJECT_0)
   { std::fprintf(stderr, "capture spacing wait failed: %lu\n", GetLastError()); return 23; }
  }
  if (limitCapture && !updateDriven && nextCapture > Clock::now())
		{
			LARGE_INTEGER due{};
			due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(nextCapture - Clock::now()).count() / 100);
			if (!SetWaitableTimer(captureTimer, &due, 0, nullptr, nullptr, FALSE) || WaitForSingleObject(captureTimer, INFINITE) != WAIT_OBJECT_0)
			{ std::fprintf(stderr, "capture pacing wait failed: %lu\n", GetLastError()); return 23; }
		}
		livePacingWaitMs.push_back(std::chrono::duration<double,std::milli>(Clock::now()-pacingBegin).count());
  const auto capacityBegin=Clock::now();
  if(captureBackpressure&&networkSender&&networkSender->WaitForCapacity(capturePeriod))++totalBackpressureFrames;
  liveBackpressureMs.push_back(std::chrono::duration<double,std::milli>(Clock::now()-capacityBegin).count());
  const auto frameStart = Clock::now();
  SenderStageTimes stages{};
  if(stageTrace.Enabled())stages.pacingWait=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(frameStart-pacingBegin).count());
		const auto captureStart = Clock::now();
		auto copyStart = captureStart;
		auto sourceStart = captureStart;
		uint64_t captureUtcMs = UtcMs();
		if (duplication)
		{
			DXGI_OUTDUPL_FRAME_INFO captureInfo = {};
			ComPtr<IDXGIResource> desktopResource;
			// Wake on the actual desktop update. Zero-timeout polling at a second
			// 165 Hz phase can miss alternating frames after coarse timer wakeups.
			UINT acquireTimeout=constantFrameRate?1:20;
   // Round up: a repeat must never pre-empt an update due at the deadline.
   if(updateDriven)acquireTimeout=UINT(std::max<int64_t>(0,(std::chrono::duration_cast<std::chrono::microseconds>(nextCapture-Clock::now()).count()+999)/1000));
			const HRESULT acquireResult = duplication->AcquireNextFrame(acquireTimeout, &captureInfo, &desktopResource);
			if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT)
			{
				++captureTimeouts;
				++liveCaptureTimeouts;
    if(!constantFrameRate||!haveDesktopFrame){if(constantFrameRate)nextCapture=Clock::now()+capturePeriod;continue;}
			}
			if (FAILED(acquireResult) && acquireResult!=DXGI_ERROR_WAIT_TIMEOUT)
			{
				std::fprintf(stderr, "Desktop Duplication AcquireNextFrame failed: 0x%08lx\n", (unsigned long)acquireResult);
				return 20;
			}
			copyStart = Clock::now();
   lastCaptureStart=copyStart;
   if(stageTrace.Enabled()){stages.t0=StageUtcUs();stages.acquireWait=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(copyStart-captureStart).count());stages.accumulated=captureInfo.AccumulatedFrames;}
			if (limitCapture)
			{
				if(legacyCapturePacing){do { nextCapture += capturePeriod; } while (nextCapture <= copyStart);}
    // Phase follows the update. The extra margin lets the next update of a
    // continuously refreshing desktop arrive first instead of being preceded
    // by a redundant repeat; static desktops return to the exact cadence.
    else if(updateDriven&&SUCCEEDED(acquireResult))nextCapture=copyStart+capturePeriod+std::chrono::milliseconds(2);
    else{
     // Retain at most one deadline of catch-up credit. Do not skip an
     // additional whole interval solely because capture/encode woke late.
     nextCapture+=capturePeriod;
     if(copyStart-nextCapture>capturePeriod)nextCapture=copyStart;
    }
    if(SUCCEEDED(acquireResult))liveAccumulatedSourceFrames+=std::max<UINT>(1,captureInfo.AccumulatedFrames);
			}
			// Timestamp the actual source desktop update, before capture/encode.
			// LastPresentTime uses the local QPC clock, not the laptop UTC clock.
   // Preserve the actual source update time in repeated frames. System
   // display feedback counts latency once per distinct displayed source.
			LARGE_INTEGER sourceUpdated{};
			// A client-drawn pointer is not in the video, so pointer moves are not source updates.
			sourceUpdated.QuadPart=remoteCursor?captureInfo.LastPresentTime.QuadPart:std::max(captureInfo.LastPresentTime.QuadPart,captureInfo.LastMouseUpdateTime.QuadPart);
			if (sourceUpdated.QuadPart > 0)
			{
				LARGE_INTEGER qpcNow{}, qpcFrequency{};
				QueryPerformanceCounter(&qpcNow); QueryPerformanceFrequency(&qpcFrequency);
				const double ageMs = std::max(0.0, double(qpcNow.QuadPart - sourceUpdated.QuadPart) * 1000.0 / qpcFrequency.QuadPart);
    captureUtcMs = UtcMs() - uint64_t(ageMs);sourceStart = Clock::now() - std::chrono::nanoseconds(int64_t(ageMs * 1000000.0));
				liveSourceAgeMs.push_back(ageMs);
    if(stageTrace.Enabled())stages.source=StageUtcUs()-uint64_t(ageMs*1000.0);
			}
   if(remoteCursor&&SUCCEEDED(acquireResult)&&!captureInfo.LastPresentTime.QuadPart&&haveDesktopFrame){captureUtcMs=lastDesktopUtcMs;sourceStart=lastDesktopSourceStart;}
   if(SUCCEEDED(acquireResult)){
			ComPtr<ID3D11Texture2D> desktopTexture;
			if (FAILED(desktopResource.As(&desktopTexture)))
				return 20;
			d3dContext->CopyResource(texture.Get(), desktopTexture.Get());
			if(remoteCursor)remoteCursor->UpdateShape(duplication.Get(),captureInfo);
			else desktopPointer.Draw(d3dDevice.Get(),d3dContext.Get(),duplication.Get(),texture.Get(),captureInfo,Width,Height);
   if(partialFrames){
    // Move and dirty rectangles must be read before ReleaseFrame.
    if(captureInfo.TotalMetadataBufferSize){
     damageMetadata.resize(captureInfo.TotalMetadataBufferSize);UINT used=0;
     if(SUCCEEDED(duplication->GetFrameMoveRects(UINT(damageMetadata.size()),reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(damageMetadata.data()),&used))){
      const auto*moves=reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(damageMetadata.data());
      for(UINT i=0;i<used/sizeof(DXGI_OUTDUPL_MOVE_RECT);++i){
       const RECT&d=moves[i].DestinationRect;unite(frameDamage,frameDamageValid,d);
       const RECT s{moves[i].SourcePoint.x,moves[i].SourcePoint.y,moves[i].SourcePoint.x+(d.right-d.left),moves[i].SourcePoint.y+(d.bottom-d.top)};unite(frameDamage,frameDamageValid,s);
      }
     }else damageUnknown=true;
     used=0;
     if(SUCCEEDED(duplication->GetFrameDirtyRects(UINT(damageMetadata.size()),reinterpret_cast<RECT*>(damageMetadata.data()),&used))){
      const auto*dirty=reinterpret_cast<const RECT*>(damageMetadata.data());
      for(UINT i=0;i<used/sizeof(RECT);++i)unite(frameDamage,frameDamageValid,dirty[i]);
     }else damageUnknown=true;
    }else if(captureInfo.LastPresentTime.QuadPart){damageUnknown=true;++damageWithoutMetadata;}
    // The pointer is composed by us, so its old and new boxes are damage too.
    RECT pointer{};const bool pointerValid=!remoteCursor&&desktopPointer.Bounds(Width,Height,pointer);
    if(pointerValid!=lastPointerValid||captureInfo.PointerShapeBufferSize||(pointerValid&&!EqualRect(&pointer,&lastPointer))){
     if(lastPointerValid)unite(frameDamage,frameDamageValid,lastPointer);
     if(pointerValid)unite(frameDamage,frameDamageValid,pointer);
    }
    lastPointer=pointer;lastPointerValid=pointerValid;
   }
			duplication->ReleaseFrame();haveDesktopFrame=true;lastDesktopUtcMs=captureUtcMs;lastDesktopSourceStart=sourceStart;
   }else{++liveRepeatedFrames;captureUtcMs=lastDesktopUtcMs;sourceStart=lastDesktopSourceStart;}
		}
		else
			update_patch(d3dContext.Get(), texture.Get(), patch, int(uint64_t(frame) * 19 % (Width - 192)), uint64_t(frame));
		acquire.sync.value = ++timeline;
		d3dContext4->Signal(fence.Get(), timeline);
		const auto captureEnd = Clock::now();
		++liveCaptureFrames;
		liveCaptureMs.push_back(std::chrono::duration<double, std::milli>(captureEnd - copyStart).count());

		const auto encodeStart = Clock::now();
  if(stageTrace.Enabled())stages.t1=StageUtcUs();
		release.sync.value = ++timeline;
  pyrowave_encoder frameEncoder=encoder;pyrowave_rate_control frameRate=rate;
  VkRect2D crop{};bool partialFrame=false;
  if(partialFrames){
   const uint64_t drops=networkSender?networkSender->droppedFrames.load():0;
   // A frame replaced in the send mailbox never reached the receiver.
   bool full=damageUnknown||frame==0||frame-lastFullFrame>=uint64_t(CaptureFps)||drops!=lastSenderDrops;
   if(damageUnknown)++fullUnknown;else if(drops!=lastSenderDrops)++fullDropped;else if(full)++fullPeriodic;
   if(frameDamageValid)liveDamageArea.push_back(double(frameDamage.right-frameDamage.left)*(frameDamage.bottom-frameDamage.top)/(double(Width)*Height));
   lastSenderDrops=drops;
   // Resend the previous two frames' damage so one lost partial frame heals.
   RECT region=frameDamage;bool regionValid=frameDamageValid;
   for(int i=0;i<2;++i)if(damageHistoryValid[i])unite(region,regionValid,damageHistory[i]);
   if(!full){
    if(!regionValid){region={0,0,1,1};regionValid=true;} // cadence keep-alive
    const int bw=region.right-region.left,bh=region.bottom-region.top;int cw=0,ch=0;
    for(int w:PartialWidths)if(w>=bw&&w<=Width){cw=w;break;}
    for(int h:PartialHeights)if(h>=bh&&h<=Height){ch=h;break;}
    // Beyond three quarters of the output a partial frame saves little.
    if(cw&&ch&&double(cw)*ch<=0.75*double(Width)*Height){
     const int x=std::max(0,std::min(Width-cw,int(region.left)-(cw-bw)/2))&~1,y=std::max(0,std::min(Height-ch,int(region.top)-(ch-bh)/2))&~1;
     if(pyrowave_encoder partial=partialEncoderFor(cw,ch)){
      crop={{x,y},{uint32_t(cw),uint32_t(ch)}};partialFrame=true;frameEncoder=partial;
      // Same bits per pixel as the current full-frame budget.
      frameRate.maximum_bitstream_size=std::max<size_t>(16384,size_t(double(rate.maximum_bitstream_size)*cw*ch/(double(Width)*Height)))&~size_t(3);
      livePartialArea.push_back(double(cw)*ch/(double(Width)*Height));
     }
    }
    if(!partialFrame)++fullLarge;
   }
   if(partialFrame)++totalPartialFrames;
   else{++totalFullFrames;lastFullFrame=frame;damageUnknown=false;damageHistoryValid[0]=damageHistoryValid[1]=false;}
   if(partialFrame){damageHistory[1]=damageHistory[0];damageHistoryValid[1]=damageHistoryValid[0];damageHistory[0]=frameDamage;damageHistoryValid[0]=frameDamageValid;}
   frameDamageValid=false;
  }
  scaleInfo.crop_rect=partialFrame?&crop:nullptr;
		if (!check(pyrowave_encoder_encode_gpu_scaled(frameEncoder, &acquire, &release, &scaleInfo, &frameRate), "GPU encode"))
			return 14;
		const auto encodeEnd = Clock::now();
  if(stageTrace.Enabled())stages.t1Return=StageUtcUs();
		liveEncodeMs.push_back(std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count());

		const auto packetizeStart = Clock::now();
  const void* mappedBitstream=nullptr;const void* mappedMetadata=nullptr;size_t mappedSize=0,metadataSize=0;
  if(!check(pyrowave_encoder_get_mapped_raw_bitstream(frameEncoder,&mappedBitstream,&mappedSize,&mappedMetadata,&metadataSize),"wait for encoded GPU frame"))return 15;
  const auto gpuReadbackEnd=Clock::now();
  if(stageTrace.Enabled())stages.t2=StageUtcUs();
  liveGpuReadbackMs.push_back(std::chrono::duration<double,std::milli>(gpuReadbackEnd-packetizeStart).count());
		size_t packetCount = 0;
  // Partial frames reserve packet 0 and 16 bytes for the region descriptor.
  const size_t descriptorBytes=partialFrame?16:0,firstPacket=partialFrame?1:0;
  char tableProblem[192]={};
  bool frameUsable=!blockTableProblem(mappedMetadata,metadataSize,mappedSize,bitstream.size()-descriptorBytes,tableProblem,sizeof(tableProblem));
  if(frameUsable){
   if (!check(pyrowave_encoder_compute_num_packets(frameEncoder, packetBoundary, &packetCount), "compute packet count"))
    return 15;
   // An impossible packet count is the same corrupt block table seen from another side.
   if (packetCount + 1 > packets.size()){
    std::snprintf(tableProblem,sizeof(tableProblem),"packet count %zu exceeds provisioned capacity %zu",packetCount,packets.size());
    frameUsable=false;
   }
  }
  if(!frameUsable){
   // Drop the frame and resynchronise with a full one instead of copying wild memory.
   ++rejectedFrames;
   if(rejectedFrames<=10||rejectedFrames%100==0)
    std::fprintf(stderr,"Rejected corrupt encoder output #%llu: %s (partial=%d crop=%ux%u)\n",(unsigned long long)rejectedFrames,tableProblem,partialFrame?1:0,partialFrame?crop.extent.width:0u,partialFrame?crop.extent.height:0u);
   damageUnknown=true;packetCount=0;
  }
  else{
   if (!check(pyrowave_encoder_packetize(frameEncoder, packets.data()+firstPacket, packetBoundary, &packetCount,
                                         bitstream.data()+descriptorBytes, bitstream.size()-descriptorBytes), "packetize GPU frame"))
    return 17;
   if(partialFrame){
    const uint16_t descriptor[4]={uint16_t(crop.offset.x),uint16_t(crop.offset.y),uint16_t(crop.extent.width),uint16_t(crop.extent.height)};
    std::memcpy(bitstream.data(),descriptor,sizeof(descriptor));
    for(size_t i=1;i<=packetCount;++i)packets[i].offset+=descriptorBytes;
    packets[0]={0,sizeof(descriptor)};++packetCount;
   }
  }
		uint64_t frameBytes = 0;
		for (size_t i = 0; i < packetCount; ++i)
			frameBytes += packets[i].size;
		const auto packetizeEnd = Clock::now();
  ++totalEncodedFrames;totalEncodedBytes+=frameBytes;liveEncodedBytes.push_back(double(frameBytes));
  if(stageTrace.Enabled())stages.packetized=StageUtcUs();
		livePacketizeMs.push_back(std::chrono::duration<double, std::milli>(packetizeEnd - packetizeStart).count());
  liveCpuPacketizeMs.push_back(std::chrono::duration<double,std::milli>(packetizeEnd-gpuReadbackEnd).count());
  // Include encryption, IPv4/UDP and Ethernet framing/IFG in the link budget.
  // Quality changes to fit the link; the capture FPS target never changes here.
  uint64_t estimatedWireBytes=0;
  for(size_t i=0;i<packetCount;++i){
   const uint64_t fragments=(packets[i].size+FragmentPayloadBytes-1)/FragmentPayloadBytes;
   estimatedWireBytes+=((udpSegmented||sendRio)?fragments*FragmentPayloadBytes:packets[i].size)+fragments*(sizeof(DatagramHeader)+(encryptedVideo?32:16)+28+38);
   if(fec)estimatedWireBytes+=FragmentPayloadBytes+sizeof(DatagramHeader)+(encryptedVideo?32:16)+28+38;
  }
  if(!partialFrame)lastEstimatedWireMbps=double(estimatedWireBytes)*8.0*CaptureFps/1000000.0;
  if(wireBytesPerFrame>0&&!partialFrame&&frameUsable){
   if(double(estimatedWireBytes)>wireBytesPerFrame*1.01)
    rate.maximum_bitstream_size=std::max<size_t>(65536,size_t(double(rate.maximum_bitstream_size)*wireBytesPerFrame/double(estimatedWireBytes)*0.99))&~size_t(3);
   else if(double(estimatedWireBytes)<wireBytesPerFrame*0.98&&rate.maximum_bitstream_size<std::min(maxFrameBytes,sendDeadlineBudget))
    rate.maximum_bitstream_size=std::min(std::min(maxFrameBytes,sendDeadlineBudget),rate.maximum_bitstream_size+std::max<size_t>(256,rate.maximum_bitstream_size/200))&~size_t(3);
  }
		if (FAILED(d3dContext4->Wait(fence.Get(), timeline)))
		{
			std::fprintf(stderr, "D3D11 wait for Vulkan encode failed\n");
			return 18;
		}
		const auto sendStart = Clock::now();
		if (frameUsable && udp != INVALID_SOCKET && frameStart >= measureStart && frameStart < end)
		{
			if (packetCount > UINT16_MAX) { std::fprintf(stderr, "packet count overflow\n"); return 21; }
			networkSender->Publish(bitstream, packets, packetCount, uint32_t(frame), captureUtcMs, sourceStart,stages,partialFrame?PartialDatagramMagic:FullDatagramMagic);
		}
		if(!networkSender)liveFrameToUdpMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());

		if (measureSeconds > 0 && frameStart >= measureStart && frameStart < end)
		{
			captureMs.push_back(std::chrono::duration<double, std::milli>(captureEnd - copyStart).count());
			encodeMs.push_back(std::chrono::duration<double, std::milli>(encodeEnd - encodeStart).count());
			packetizeMs.push_back(std::chrono::duration<double, std::milli>(packetizeEnd - packetizeStart).count());
			totalMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - sourceStart).count());
			packetBytes += frameBytes;
			++measuredFrames;
		}
		const auto target = start + std::chrono::nanoseconds((int64_t(frame) + 1) * 1000000000ll / Fps);
		if (!duplication && target > Clock::now())
			std::this_thread::sleep_until(target);
	}
	writeLiveStats();
	if (captureTimer) CloseHandle(captureTimer);
	if(networkSender){networkSender->Stop();udpBytes=networkSender->totalBytes.load();}
	const double elapsed = std::chrono::duration<double>(Clock::now() - measureStart).count();
	std::wstring jsonCaptureDisplayText;
	for (wchar_t ch : captureDisplayText)
	{
		if (ch == L'\\' || ch == L'"')
			jsonCaptureDisplayText.push_back(L'\\');
		jsonCaptureDisplayText.push_back(ch);
	}
	std::printf("{\"codec\":\"PyroWave\",\"gpu_shared_texture\":true,\"source\":\"%ls\",\"adapter_vendor\":%u,\"width\":%d,\"height\":%d,\"target_fps\":%d,\"warmup_s\":%d,\"measure_s\":%d,\"max_frame_bytes\":%llu,\"measured_frames\":%llu,\"capture_timeouts\":%llu,\"elapsed_s\":%.3f,\"fps\":%.3f,\"packet_bytes\":%llu,\"udp_bytes\":%llu,\"mean_bitrate_mbps\":%.3f,\"capture_p95_ms\":%.3f,\"encode_p50_ms\":%.3f,\"encode_p95_ms\":%.3f,\"packetize_p95_ms\":%.3f,\"encode_to_packets_p50_ms\":%.3f,\"encode_to_packets_p95_ms\":%.3f,\"frame_to_udp_p95_ms\":%.3f}\n",
	            jsonCaptureDisplayText.c_str(), adapterDesc.VendorId, Width, Height, Fps, WarmupSeconds, measureSeconds, (unsigned long long)maxFrameBytes,
	            (unsigned long long)measuredFrames, (unsigned long long)captureTimeouts, elapsed, measuredFrames / elapsed,
	            (unsigned long long)packetBytes, (unsigned long long)udpBytes,
	            double(packetBytes) * 8.0 / elapsed / 1000000.0,
	            percentile(captureMs, 0.95), percentile(encodeMs, 0.50), percentile(encodeMs, 0.95),
	            percentile(packetizeMs, 0.95), percentile(totalMs, 0.50), percentile(totalMs, 0.95), percentile(totalMs, 0.95));

	for(auto&e:partialEncoders)pyrowave_encoder_destroy(e.encoder);
	pyrowave_encoder_destroy(encoder);
	pyrowave_sync_object_destroy(sync);
	pyrowave_image_destroy(image);
	pyrowave_device_destroy(device);
	CloseHandle(fenceHandle);
	CloseHandle(textureHandle);
	if (udp != INVALID_SOCKET) { closesocket(udp); WSACleanup(); }
	return measuredFrames >= uint64_t(Fps * measureSeconds * 0.90) ? 0 : 19;
}
#endif
