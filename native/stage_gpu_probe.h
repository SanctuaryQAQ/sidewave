#pragma once
#include "stage_trace.h"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;

// Nonblocking GPU queries run on the existing immediate-context thread.
// q0 is before the D3D acquire signal, q1 after the Vulkan release wait,
// q2 after rendering. These measure the bridge/decode and rendering GPU spans;
// they are not calibrated absolute GPU timestamps or isolated shader time.
class StageGpuProbe {
 struct Slot{ComPtr<ID3D11Query>disjoint,q[3];uint32_t frame=0;bool active=false;};
 Slot slots[64];Slot*current=nullptr;size_t cursor=0;StageTraceWriter&trace;
 ID3D11DeviceContext*context;bool usable=true;
public:
 StageGpuProbe(ID3D11Device*device,ID3D11DeviceContext*c,StageTraceWriter&t):trace(t),context(c){
  if(!trace.Enabled()){usable=false;return;}
  for(auto&s:slots){D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};if(FAILED(device->CreateQuery(&d,&s.disjoint))){usable=false;break;}d.Query=D3D11_QUERY_TIMESTAMP;for(auto&q:s.q)if(FAILED(device->CreateQuery(&d,&q)))usable=false;}
 }
 void Poll(){if(!usable)return;for(auto&s:slots)if(s.active){
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 v[3]{};
  if(context->GetData(s.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
  bool complete=true;for(int i=0;i<3;++i)if(context->GetData(s.q[i].Get(),&v[i],sizeof(v[i]),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)complete=false;
  if(!complete)continue;s.active=false;
  char row[384];snprintf(row,sizeof(row),"{\"type\":\"gpu\",\"frame\":%u,\"valid\":%s,\"bridge_decode_us\":%.3f,\"render_us\":%.3f}\n",s.frame,!d.Disjoint&&d.Frequency&&v[2]>=v[1]&&v[1]>=v[0]?"true":"false",d.Frequency?double(v[1]-v[0])*1e6/d.Frequency:0,d.Frequency?double(v[2]-v[1])*1e6/d.Frequency:0);trace.Publish(row);
 }}
 void Begin(uint32_t frame){current=nullptr;if(!usable||frame%4)return;auto&s=slots[cursor++%64];if(s.active)return;s.frame=frame;current=&s;context->Begin(s.disjoint.Get());context->End(s.q[0].Get());}
 void Decoded(){if(current)context->End(current->q[1].Get());}
 void End(){if(current){context->End(current->q[2].Get());context->End(current->disjoint.Get());current->active=true;current=nullptr;}}
};

// Observe Vulkan release-fence completion on a separate thread. T6 is an
// upper-bound CPU observation, including event wake/scheduling delay. Never
// wait on the render thread merely to measure it.
class StageFenceProbe {
 struct Item{ComPtr<ID3D11Fence>fence;uint64_t value;uint32_t frame;};
 std::deque<Item>pending;std::mutex gate;std::condition_variable ready;
 std::thread worker;std::atomic<bool>stop{false};StageTraceWriter&trace;const char*eventTag;
public:
 explicit StageFenceProbe(StageTraceWriter&t,const char*tag="decode_fence"):trace(t),eventTag(tag){if(!trace.Enabled())return;worker=std::thread([this]{
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return;
  while(!stop){Item item{};{std::unique_lock<std::mutex>lock(gate);ready.wait(lock,[this]{return stop||!pending.empty();});if(stop)break;item=std::move(pending.front());pending.pop_front();}
   ResetEvent(event);HRESULT hr=item.fence->SetEventOnCompletion(item.value,event);
   bool done=false;if(SUCCEEDED(hr))while(!stop){DWORD w=WaitForSingleObject(event,20);if(w==WAIT_OBJECT_0){done=true;break;}if(w!=WAIT_TIMEOUT)break;}
   if(done){char row[192];snprintf(row,sizeof(row),"{\"type\":\"%s\",\"frame\":%u,\"t6_observed_us\":%llu}\n",eventTag,item.frame,(unsigned long long)StageUtcUs());trace.Publish(row);}
  }
  CloseHandle(event);
 });}
 ~StageFenceProbe(){stop=true;ready.notify_one();if(worker.joinable())worker.join();}
 void Watch(ID3D11Fence*fence,uint64_t value,uint32_t frame){if(!worker.joinable())return;std::lock_guard<std::mutex>lock(gate);if(pending.size()>=128)return;Item item{};item.fence=fence;item.value=value;item.frame=frame;pending.push_back(std::move(item));ready.notify_one();}
};
