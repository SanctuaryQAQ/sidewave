#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <cstring>
// Current-user mapped ring: no per-frame disk write or GPU observer work.
class DisplayTimingMap {
 static constexpr size_t Slots=1024,Footer=64+Slots*64,Bytes=Footer+128;
 HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr;uint8_t*data=nullptr;
 std::wstring path;uint64_t sequence=0;int64_t frequency=0;
 template<class T>T Read(size_t offset)const{T v{};std::memcpy(&v,data+offset,sizeof(v));return v;}
 template<class T>void Put(size_t offset,T v){std::memcpy(data+offset,&v,sizeof(v));}
public:
 struct Feedback{uint64_t count=0,window=0;double mean=0,p50=0,p95=0,fps=0,bound=0,delayMs=0,publishAgeMs=0;uint64_t unmatched=0,dropped=0,repeats=0;bool valid=false;};
 DisplayTimingMap(const std::wstring&runtime){
  LARGE_INTEGER f{};QueryPerformanceFrequency(&f);frequency=f.QuadPart;
  path=runtime+L"\\display-timing-"+std::to_wstring(GetCurrentProcessId())+L".bin";
  file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
  if(file==INVALID_HANDLE_VALUE)return;
  mapping=CreateFileMappingW(file,nullptr,PAGE_READWRITE,0,DWORD(Bytes),nullptr);if(!mapping)return;
  data=static_cast<uint8_t*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,Bytes));if(!data)return;
  std::memset(data,0,Bytes);Put<uint32_t>(0,0x50594c54);Put<uint32_t>(4,1);Put<uint32_t>(8,GetCurrentProcessId());Put<uint32_t>(12,Slots);Put<int64_t>(16,frequency);
 }
 ~DisplayTimingMap(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(!path.empty())DeleteFileW(path.c_str());}
 void Present(uint32_t frame,uint64_t sourceMs,int64_t offsetMs,int64_t boundMs,bool calibrated,bool snapshot){
  if(!data)return;const uint64_t seq=++sequence;const size_t at=64+(seq%Slots)*64;
  InterlockedExchange(reinterpret_cast<volatile LONG*>(data+at),LONG(seq*2+1));
  LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);ULARGE_INTEGER utc{};utc.LowPart=ft.dwLowDateTime;utc.HighPart=ft.dwHighDateTime;
  Put<uint32_t>(at+4,frame);Put<int64_t>(at+8,qpc.QuadPart);Put<int64_t>(at+16,int64_t((utc.QuadPart-116444736000000000ull)/10));Put<int64_t>(at+24,int64_t(sourceMs)*1000);
  Put<int64_t>(at+32,offsetMs*1000);Put<int64_t>(at+40,boundMs*1000);Put<uint32_t>(at+48,calibrated?1:0);Put<uint32_t>(at+52,snapshot?1:0);
  MemoryBarrier();InterlockedExchange(reinterpret_cast<volatile LONG*>(data+at),LONG(seq*2+2));InterlockedExchange64(reinterpret_cast<volatile LONG64*>(data+24),LONG64(seq));
 }
 Feedback Poll()const{
  Feedback f{};if(!data)return f;const LONG before=InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(data+Footer),0,0);if(!before||(before&1))return f;
  const uint32_t flags=Read<uint32_t>(Footer+4);f.count=Read<uint64_t>(Footer+8);f.window=Read<uint64_t>(Footer+16);
  const int64_t published=Read<int64_t>(Footer+32),displayed=Read<int64_t>(Footer+40);
  f.mean=Read<double>(Footer+48);f.p50=Read<double>(Footer+56);f.p95=Read<double>(Footer+64);f.fps=Read<double>(Footer+72);f.unmatched=Read<uint64_t>(Footer+80);f.dropped=Read<uint64_t>(Footer+88);f.repeats=Read<uint64_t>(Footer+96);f.bound=Read<double>(Footer+104);
  MemoryBarrier();if(before!=InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(data+Footer),0,0))return Feedback{};
  LARGE_INTEGER now{};QueryPerformanceCounter(&now);const double age=double(now.QuadPart-published)*1000/frequency;f.delayMs=double(now.QuadPart-displayed)*1000/frequency;
  // ETW/CSV delivery is batched. Retain an accurately timestamped display
  // sample between batches, bounded by the same five-second event expiry.
  // This changes reporting freshness, never the measured latency endpoint.
  f.publishAgeMs=age;
  f.valid=flags==1&&f.window&&published&&age>=0&&age<5000&&f.delayMs>=0&&f.delayMs<5000&&f.mean>=0&&f.mean<5000;return f;
 }
};
