#pragma once
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>

inline uint64_t StageUtcUs(){FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);ULARGE_INTEGER v{};v.LowPart=ft.dwLowDateTime;v.HighPart=ft.dwHighDateTime;return (v.QuadPart-116444736000000000ULL)/10;}
inline uint64_t StageQpc(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return uint64_t(q.QuadPart);}

// Diagnostic append-only trace. Disk writes are never performed by the video
// threads. Bounded memory; loss is counted rather than blocking for storage.
class StageTraceWriter {
 std::mutex gate;std::condition_variable ready;std::deque<std::string> pending;
 std::thread worker;bool stopped=false;std::atomic<bool> enabled{false};
 std::atomic<uint64_t> dropped{0};std::atomic<DWORD> error{0};
public:
 explicit StageTraceWriter(const std::wstring&path){
  if(path.empty())return;
  HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE){error=GetLastError();return;}
  enabled=true;worker=std::thread([this,file]{
   SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
   std::deque<std::string> batch;
   for(;;){
    {std::unique_lock<std::mutex>lock(gate);ready.wait_for(lock,std::chrono::milliseconds(200),[this]{return stopped;});pending.swap(batch);if(batch.empty()&&stopped)break;}
    std::string bytes;for(const auto&line:batch)bytes+=line;batch.clear();
    if(!bytes.empty()){DWORD written=0;if(!WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()){error=GetLastError();enabled=false;}}
   }
   CloseHandle(file);
  });
 }
 ~StageTraceWriter(){{std::lock_guard<std::mutex>lock(gate);stopped=true;}ready.notify_one();if(worker.joinable())worker.join();}
 bool Enabled()const{return enabled.load();}
 uint64_t Dropped()const{return dropped.load();}
 DWORD Error()const{return error.load();}
 void Publish(const char*text){if(!Enabled())return;std::lock_guard<std::mutex>lock(gate);if(pending.size()>=8192){++dropped;return;}pending.emplace_back(text);}
 StageTraceWriter(const StageTraceWriter&)=delete;
 StageTraceWriter&operator=(const StageTraceWriter&)=delete;
};

struct SenderStageTimes {
 uint64_t source=0,t0=0,t1=0,t1Return=0,t2=0,packetized=0,published=0;
 uint64_t acquireWait=0,pacingWait=0;unsigned accumulated=0;
};

inline void StageProfileMessage(void*userdata,const char*message){
 if(!std::strstr(message,"ms per frame"))return;
 char clean[256]{};size_t i=0;for(;message[i]&&i<sizeof(clean)-1;++i)clean[i]=message[i]=='"'||message[i]=='\\'||static_cast<unsigned char>(message[i])<32?' ':message[i];
 char row[440];snprintf(row,sizeof(row),"{\"type\":\"gpu_profile\",\"utc_us\":%llu,\"text\":\"%s\"}\n",StageUtcUs(),clean);static_cast<StageTraceWriter*>(userdata)->Publish(row);
}
