#pragma once
// Client-side cursor: the host pointer travels in its own small sealed UDP
// datagrams and the receiver shows it with the system hardware cursor, so the
// pointer no longer waits for capture, encode, transmit and decode.
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

constexpr uint32_t CursorDatagramMagic = 0x5057525Au;
constexpr uint16_t CursorKindPosition = 1, CursorKindShape = 2;
constexpr uint32_t CursorMaxShapeBytes = 256 * 256 * 4;
constexpr size_t CursorShapeChunkBytes = 1380;

#pragma pack(push, 1)
struct CursorDatagramHeader {
 uint32_t magic;
 uint16_t kind;
 uint16_t visible;
 uint32_t shapeSerial;
 int32_t x, y;          // hotspot, output pixels
 uint64_t sampleUtcUs;  // host UTC when this position was observed
};
// Followed by shape bytes [offset, offset+chunk). Monochrome: AND rows then
// XOR rows, WORD-aligned pitch. Color / masked color: BGRA, pitch width*4.
struct CursorShapeHeader {
 uint16_t type, width, height, hotX, hotY, pitch;
 uint32_t totalBytes, offset;
};
#pragma pack(pop)
static_assert(sizeof(CursorDatagramHeader) == 28, "unexpected cursor header size");
static_assert(sizeof(CursorShapeHeader) == 20, "unexpected cursor shape header size");

inline uint64_t CursorUtcUs()
{
 FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);
 ULARGE_INTEGER v{};v.LowPart=ft.dwLowDateTime;v.HighPart=ft.dwHighDateTime;
 return (v.QuadPart-116444736000000000ULL)/10ULL;
}

#ifdef __dxgi1_2_h__
#include <thread>
// Host side. The capture thread supplies DXGI pointer shapes; a 1 kHz thread
// samples the position and sends position (on change, else every 100 ms) and
// the current shape (on change, else every second, healing lost fragments).
template<class Seal>
class RemoteCursorSender {
 SOCKET socket;sockaddr_in destination;RECT output;Seal seal;
 std::mutex mutex;std::vector<uint8_t> shape;CursorShapeHeader shapeInfo{};uint32_t serial=0;
 std::vector<uint8_t> raw;
 std::atomic<bool> stopping{false};std::thread worker;
 std::atomic<uint64_t> positions{0},shapes{0},errors{0};
 void Send(const CursorDatagramHeader&h,const void*extra,size_t extraBytes,const void*payload,size_t payloadBytes){
  uint8_t plain[1440];std::memcpy(plain,&h,sizeof(h));
  if(extraBytes)std::memcpy(plain+sizeof(h),extra,extraBytes);
  uint8_t wire[1472];const size_t n=seal(plain,sizeof(h)+extraBytes,payload,payloadBytes,wire);
  if(sendto(socket,reinterpret_cast<const char*>(wire),int(n),0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))==SOCKET_ERROR)errors.fetch_add(1);
 }
 void Run(){
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
  // Physical pixels on every monitor, independent of the process awareness.
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
  CursorDatagramHeader last{};uint32_t sentSerial=0;bool havePosition=false;
  ULONGLONG lastPositionMs=0,lastShapeMs=0;
  std::vector<uint8_t> local;CursorShapeHeader localInfo{};
  while(!stopping.load(std::memory_order_relaxed)){
   LARGE_INTEGER due{};due.QuadPart=-10000; // 1 ms
   if(timer&&SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,5);else Sleep(1);
   CURSORINFO ci{};ci.cbSize=sizeof(ci);if(!GetCursorInfo(&ci))continue;
   const uint64_t sampled=CursorUtcUs();
   CursorDatagramHeader h{};h.magic=CursorDatagramMagic;h.kind=CursorKindPosition;
   h.x=ci.ptScreenPos.x-output.left;h.y=ci.ptScreenPos.y-output.top;
   const bool inside=ci.ptScreenPos.x>=output.left&&ci.ptScreenPos.x<output.right&&ci.ptScreenPos.y>=output.top&&ci.ptScreenPos.y<output.bottom;
   h.visible=uint16_t((ci.flags&CURSOR_SHOWING)&&inside?1:0);
   uint32_t currentSerial;{std::lock_guard<std::mutex> lock(mutex);currentSerial=serial;
    if(currentSerial!=sentSerial){local=shape;localInfo=shapeInfo;}}
   h.shapeSerial=currentSerial;h.sampleUtcUs=sampled;
   const ULONGLONG now=GetTickCount64();
   if(!havePosition||h.x!=last.x||h.y!=last.y||h.visible!=last.visible||h.shapeSerial!=last.shapeSerial||now-lastPositionMs>=100){
    Send(h,nullptr,0,nullptr,0);last=h;havePosition=true;lastPositionMs=now;positions.fetch_add(1);
   }
   if(currentSerial&&(currentSerial!=sentSerial||now-lastShapeMs>=1000)){
    if(currentSerial==sentSerial){std::lock_guard<std::mutex> lock(mutex);if(serial==currentSerial){local=shape;localInfo=shapeInfo;}}
    h.kind=CursorKindShape;
    for(uint32_t at=0;at<localInfo.totalBytes;at+=uint32_t(CursorShapeChunkBytes)){
     CursorShapeHeader s=localInfo;s.offset=at;
     Send(h,&s,sizeof(s),local.data()+at,std::min<size_t>(CursorShapeChunkBytes,localInfo.totalBytes-at));
    }
    sentSerial=currentSerial;lastShapeMs=now;shapes.fetch_add(1);
   }
  }
  if(timer)CloseHandle(timer);
 }
public:
 RemoteCursorSender(SOCKET s,const sockaddr_in&to,const RECT&desktop,Seal sealer):socket(s),destination(to),output(desktop),seal(sealer){
  worker=std::thread([this]{Run();});
 }
 ~RemoteCursorSender(){stopping=true;if(worker.joinable())worker.join();}
 uint64_t Positions()const{return positions.load();}
 uint64_t Shapes()const{return shapes.load();}
 uint64_t Errors()const{return errors.load();}
 // Diagnostics: last shape query result and dimensions (w<<16|h).
 std::atomic<uint32_t> shapeQueries{0},lastShapeResult{0},lastShapeSize{0};
 // Capture thread, before ReleaseFrame.
 void UpdateShape(IDXGIOutputDuplication*dup,const DXGI_OUTDUPL_FRAME_INFO&frame){
  // DXGI flags only shape changes; until one is held, query the current shape.
  if(!frame.PointerShapeBufferSize&&serial)return;
  raw.resize(std::max<size_t>(frame.PointerShapeBufferSize,256*256*4));
  DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};UINT required=0;
  const HRESULT hr=dup->GetFramePointerShape(UINT(raw.size()),raw.data(),&required,&info);
  shapeQueries.fetch_add(1);lastShapeResult=uint32_t(hr);lastShapeSize=(info.Width<<16)|(info.Height&0xffff);
  if(FAILED(hr))return;
  const bool mono=info.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
  const UINT w=info.Width,h=mono?info.Height/2:info.Height;
  if(!w||!h||w>256||h>256)return;
  CursorShapeHeader s{};s.type=uint16_t(info.Type);s.width=uint16_t(w);s.height=uint16_t(h);
  s.hotX=uint16_t(info.HotSpot.x);s.hotY=uint16_t(info.HotSpot.y);
  std::vector<uint8_t> packed;
  if(mono){
   s.pitch=uint16_t((w+15)/16*2);packed.assign(size_t(s.pitch)*h*2,0);
   for(UINT y=0;y<h*2;++y)std::memcpy(packed.data()+size_t(y)*s.pitch,raw.data()+size_t(y)*info.Pitch,(w+7)/8);
  }else{
   s.pitch=uint16_t(w*4);packed.resize(size_t(s.pitch)*h);
   for(UINT y=0;y<h;++y)std::memcpy(packed.data()+size_t(y)*s.pitch,raw.data()+size_t(y)*info.Pitch,size_t(w)*4);
  }
  s.totalBytes=uint32_t(packed.size());
  std::lock_guard<std::mutex> lock(mutex);
  if(s.totalBytes==shapeInfo.totalBytes&&!std::memcmp(&s,&shapeInfo,sizeof(s))&&packed==shape)return;
  shape.swap(packed);shapeInfo=s;++serial;if(!serial)serial=1;
 }
};
#endif

// Receiver side: reassembles shapes and moves the real system cursor.
class RemoteCursorReceiver {
 struct Assembly{uint32_t serial=0;CursorShapeHeader info{};std::vector<uint8_t> bytes;std::vector<bool> chunks;size_t have=0;} assembly;
 uint32_t appliedSerial=0;
 HCURSOR previous=nullptr;
 POINT lastSet{LONG_MIN,LONG_MIN};
 static HCURSOR Build(const CursorShapeHeader&s,const std::vector<uint8_t>&b){
  ICONINFO ii{};ii.fIcon=FALSE;ii.xHotspot=s.hotX;ii.yHotspot=s.hotY;
  const int w=s.width,h=s.height;
  if(s.type==1){ // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
   ii.hbmMask=CreateBitmap(w,h*2,1,1,b.data());
  }else{
   BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(bi.bmiHeader);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;
   bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
   void*bits=nullptr;HDC dc=GetDC(nullptr);ii.hbmColor=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);ReleaseDC(nullptr,dc);
   if(!ii.hbmColor||!bits)return nullptr;
   const int maskPitch=(w+15)/16*2;std::vector<uint8_t> mask(size_t(maskPitch)*h,0);
   uint8_t*out=static_cast<uint8_t*>(bits);
   for(int y=0;y<h;++y)for(int x=0;x<w;++x){
    const uint8_t*p=b.data()+size_t(y)*s.pitch+size_t(x)*4;uint8_t*q=out+(size_t(y)*w+x)*4;
    q[0]=p[0];q[1]=p[1];q[2]=p[2];q[3]=p[3];
    // Masked color: alpha 0xFF XORs the RGB with the screen, alpha 0 replaces.
    // A zero alpha channel makes Windows apply AND mask + XOR color.
    if(s.type==4){q[3]=0;if(p[3])mask[size_t(y)*maskPitch+x/8]|=uint8_t(0x80>>(x%8));}
   }
   ii.hbmMask=CreateBitmap(w,h,1,1,mask.data());
  }
  HCURSOR c=ii.hbmMask?HCURSOR(CreateIconIndirect(&ii)):nullptr;
  if(ii.hbmMask)DeleteObject(ii.hbmMask);if(ii.hbmColor)DeleteObject(ii.hbmColor);
  return c;
 }
public:
 std::atomic<HCURSOR> cursor{nullptr};
 std::atomic<bool> visible{false};
 std::atomic<uint64_t> positions{0},shapes{0};
 std::vector<double> latencyMs; std::mutex latencyMutex;
 ~RemoteCursorReceiver(){if(HCURSOR c=cursor.exchange(nullptr))DestroyCursor(c);if(previous)DestroyCursor(previous);}
 // Network thread. Returns true when the datagram was a cursor datagram.
 bool Handle(const uint8_t*data,size_t n,HWND window,float videoX,float videoY,float scale,int64_t hostOffsetMs,bool calibrated){
  if(n<sizeof(CursorDatagramHeader))return false;
  CursorDatagramHeader h{};std::memcpy(&h,data,sizeof(h));
  if(h.magic!=CursorDatagramMagic)return false;
  if(h.kind==CursorKindShape&&n>=sizeof(h)+sizeof(CursorShapeHeader)){
   CursorShapeHeader s{};std::memcpy(&s,data+sizeof(h),sizeof(s));
   const size_t chunk=n-sizeof(h)-sizeof(s);
   if(!s.width||!s.height||s.width>256||s.height>256||s.totalBytes>CursorMaxShapeBytes||s.offset%CursorShapeChunkBytes||s.offset>=s.totalBytes||
      chunk!=std::min<size_t>(CursorShapeChunkBytes,s.totalBytes-s.offset)||h.shapeSerial==appliedSerial)return true;
   const size_t need=s.type==1?size_t(s.pitch)*s.height*2:size_t(s.pitch)*s.height;
   if(s.totalBytes!=need||(s.type==1?s.pitch<(s.width+7)/8:s.pitch!=s.width*4))return true;
   if(assembly.serial!=h.shapeSerial||std::memcmp(&assembly.info,&s,offsetof(CursorShapeHeader,offset))){
    assembly=Assembly{};assembly.serial=h.shapeSerial;assembly.info=s;assembly.bytes.resize(s.totalBytes);
    assembly.chunks.assign((s.totalBytes+CursorShapeChunkBytes-1)/CursorShapeChunkBytes,false);
   }
   const size_t index=s.offset/CursorShapeChunkBytes;
   if(!assembly.chunks[index]){assembly.chunks[index]=true;++assembly.have;std::memcpy(assembly.bytes.data()+s.offset,data+sizeof(h)+sizeof(s),chunk);}
   if(assembly.have==assembly.chunks.size()){
    if(HCURSOR c=Build(assembly.info,assembly.bytes)){
     HCURSOR old=cursor.exchange(c);if(previous)DestroyCursor(previous);previous=old;
     appliedSerial=assembly.serial;shapes.fetch_add(1);
     // WM_SETCURSOR applies the new shape on the window thread.
     POINT p{};if(GetCursorPos(&p)&&WindowFromPoint(p)==window)PostMessageW(window,WM_SETCURSOR,WPARAM(window),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
    }
    assembly=Assembly{};
   }
   return true;
  }
  if(h.kind!=CursorKindPosition)return true;
  positions.fetch_add(1);
  const bool show=h.visible!=0;
  if(visible.exchange(show)!=show)PostMessageW(window,WM_SETCURSOR,WPARAM(window),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
  // Never move the laptop's pointer while another application is in front.
  if(!show||GetForegroundWindow()!=window)return true;
  POINT p{LONG(videoX+(h.x+0.5f)*scale),LONG(videoY+(h.y+0.5f)*scale)};
  if(p.x!=lastSet.x||p.y!=lastSet.y){SetCursorPos(p.x,p.y);lastSet=p;}
  if(calibrated){
   const double ms=double(int64_t(CursorUtcUs()-h.sampleUtcUs))/1000.0-double(hostOffsetMs);
   if(ms>-5&&ms<5000){std::lock_guard<std::mutex> lock(latencyMutex);latencyMs.push_back(ms);}
  }
  return true;
 }
};
