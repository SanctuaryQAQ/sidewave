#pragma once
#include <mswsock.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstring>

// Drain whatever is ready; never wait for a batch to fill. Every slot remains
// registered until its receive completes (including socket-close cancellation).
class RioDatagramReader {
    static constexpr ULONG SlotCount=128,DatagramBytes=1472;
    struct Slot {unsigned char data[DatagramBytes];SOCKADDR_INET peer;};
    std::array<Slot,SlotCount> slots{};
    std::array<RIORESULT,SlotCount> completed{};
    RIO_EXTENSION_FUNCTION_TABLE api{};
    RIO_BUFFERID buffer=RIO_INVALID_BUFFERID;
    RIO_CQ cq=RIO_INVALID_CQ;
    RIO_RQ rq=RIO_INVALID_RQ;
    SOCKET socket=INVALID_SOCKET;
    HANDLE event=nullptr;
    bool notificationArmed=false;
    ULONG ready=0,index=0,outstanding=0;
    std::atomic<uint64_t> batches{0},waits{0};
    static void Fatal(const char*op,int code){
        std::fprintf(stderr,"%s failed: %d; stopping receiver safely\n",op,code);
        ExitProcess(24); // OS cancels pending I/O before registered storage dies.
    }
    void Post(ULONG slot,DWORD flags){
        RIO_BUF data{buffer,ULONG(slot*sizeof(Slot)),DatagramBytes};
        RIO_BUF peer{buffer,ULONG(slot*sizeof(Slot)+offsetof(Slot,peer)),sizeof(SOCKADDR_INET)};
        if(!api.RIOReceiveEx(rq,&data,1,nullptr,&peer,nullptr,nullptr,flags,reinterpret_cast<void*>(size_t(slot)+1)))Fatal("RIO receive",WSAGetLastError());
        ++outstanding;
    }
    void Commit(){
        if(!api.RIOReceiveEx(rq,nullptr,0,nullptr,nullptr,nullptr,nullptr,RIO_MSG_COMMIT_ONLY,nullptr))Fatal("RIO receive commit",WSAGetLastError());
    }
    ULONG Drain(){
        const ULONG count=api.RIODequeueCompletion(cq,completed.data(),SlotCount);
        if(count==RIO_CORRUPT_CQ||count>outstanding)Fatal("RIO receive completion",WSAEFAULT);
        outstanding-=count;return count;
    }
    void Arm(){
        if(notificationArmed)return;
        const int error=api.RIONotify(cq);
        if(error)Fatal("RIO receive notify",error);
        notificationArmed=true;
    }
public:
    ~RioDatagramReader(){
        if(socket!=INVALID_SOCKET){closesocket(socket);socket=INVALID_SOCKET;}
        if(cq!=RIO_INVALID_CQ){
            const ULONGLONG limit=GetTickCount64()+2000;
            while(outstanding){
                if(Drain())continue;
                if(GetTickCount64()>=limit)Fatal("RIO receive cancellation",WSAETIMEDOUT);
                Arm();
                if(WaitForSingleObject(event,20)==WAIT_OBJECT_0)notificationArmed=false;
            }
        }
        if(buffer!=RIO_INVALID_BUFFERID)api.RIODeregisterBuffer(buffer);
        if(cq!=RIO_INVALID_CQ)api.RIOCloseCompletionQueue(cq);
        if(event)CloseHandle(event);
    }
    void Init(SOCKET boundSocket){
        socket=boundSocket;
        GUID id=WSAID_MULTIPLE_RIO;DWORD bytes=0;api.cbSize=sizeof(api);
        if(WSAIoctl(socket,SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,&id,sizeof(id),&api,sizeof(api),&bytes,nullptr,nullptr))Fatal("RIO receive extension",WSAGetLastError());
        event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event)Fatal("RIO receive event",GetLastError());
        RIO_NOTIFICATION_COMPLETION notify{};notify.Type=RIO_EVENT_COMPLETION;notify.Event.EventHandle=event;notify.Event.NotifyReset=TRUE;
        cq=api.RIOCreateCompletionQueue(SlotCount*2,&notify);if(cq==RIO_INVALID_CQ)Fatal("RIO receive CQ",WSAGetLastError());
        rq=api.RIOCreateRequestQueue(socket,SlotCount,1,0,1,cq,cq,nullptr);if(rq==RIO_INVALID_RQ)Fatal("RIO receive RQ",WSAGetLastError());
        buffer=api.RIORegisterBuffer(reinterpret_cast<char*>(slots.data()),ULONG(sizeof(slots)));if(buffer==RIO_INVALID_BUFFERID)Fatal("RIO receive registration",WSAGetLastError());
        for(ULONG i=0;i<SlotCount;++i)Post(i,RIO_MSG_DEFER);
        Commit();
    }
    int Receive(char*out,size_t capacity,sockaddr_in&peer,DWORD timeoutMs=20){
        if(index==ready){
            ready=Drain();index=0;
            const ULONGLONG limit=GetTickCount64()+timeoutMs;
            while(!ready){
                Arm();
                const ULONGLONG now=GetTickCount64();
                ++waits;const DWORD status=WaitForSingleObject(event,DWORD(now<limit?limit-now:0));
                if(status==WAIT_TIMEOUT){WSASetLastError(WSAETIMEDOUT);return SOCKET_ERROR;}
                if(status!=WAIT_OBJECT_0)Fatal("RIO receive wait",GetLastError());
                notificationArmed=false;
                ready=Drain();
            }
            ++batches;
        }
        const RIORESULT result=completed[index++];
        const size_t context=size_t(result.RequestContext);
        if(!context||context>SlotCount)Fatal("RIO receive slot",WSAEFAULT);
        const ULONG slot=ULONG(context-1);
        const bool valid=!result.Status&&result.BytesTransferred<=capacity&&result.BytesTransferred<=DatagramBytes&&slots[slot].peer.si_family==AF_INET;
        if(valid){std::memcpy(out,slots[slot].data,result.BytesTransferred);peer=slots[slot].peer.Ipv4;}
        Post(slot,RIO_MSG_DEFER);
        if(index==ready)Commit();
        if(!valid){WSASetLastError(result.Status?result.Status:WSAEMSGSIZE);return SOCKET_ERROR;}
        return int(result.BytesTransferred);
    }
    uint64_t Batches()const{return batches.load();}
    uint64_t Waits()const{return waits.load();}
};
