#pragma once
#include <mswsock.h>
#include <chrono>

// One registered frame buffer, one batch, and no additional frame backlog.
class RioDatagramWriter {
    static constexpr ULONG BatchPackets=256;
    ULONG registeredCapacity=0;
    RIO_EXTENSION_FUNCTION_TABLE api{};
    RIO_CQ completion=RIO_INVALID_CQ;
    RIO_RQ requests=RIO_INVALID_RQ;
    RIO_BUFFERID dataId=RIO_INVALID_BUFFERID,addressId=RIO_INVALID_BUFFERID;
    SOCKADDR_INET address{};
    HANDLE event=nullptr;
    static void Fatal(const char* operation,int code){
        std::fprintf(stderr,"%s failed: %d; stopping sender safely\n",operation,code);
        // Pending registered buffers must remain alive until canceled by the OS.
        // Exiting the process lets Windows cancel them without reusing memory.
        ExitProcess(23);
    }
public:
    static constexpr ULONG BatchDatagrams=BatchPackets;
    double LastWaitMaxMs=0,LastCallMaxMs=0;
    ~RioDatagramWriter(){
        if(dataId!=RIO_INVALID_BUFFERID)api.RIODeregisterBuffer(dataId);
        if(addressId!=RIO_INVALID_BUFFERID)api.RIODeregisterBuffer(addressId);
        if(completion!=RIO_INVALID_CQ)api.RIOCloseCompletionQueue(completion);
        if(event)CloseHandle(event);
    }
    void Init(SOCKET socket,const sockaddr_in&destination,char*data,ULONG capacity){
        GUID id=WSAID_MULTIPLE_RIO;DWORD returned=0;api.cbSize=sizeof(api);
        if(WSAIoctl(socket,SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,&id,sizeof(id),&api,sizeof(api),&returned,nullptr,nullptr))Fatal("RIO extension",WSAGetLastError());
        event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event)Fatal("RIO event",GetLastError());
        RIO_NOTIFICATION_COMPLETION notify{};notify.Type=RIO_EVENT_COMPLETION;notify.Event.EventHandle=event;notify.Event.NotifyReset=TRUE;
        completion=api.RIOCreateCompletionQueue(BatchPackets*2,&notify);if(completion==RIO_INVALID_CQ)Fatal("RIO completion queue",WSAGetLastError());
        sockaddr_in local{};int localBytes=sizeof(local);
        if(getsockname(socket,reinterpret_cast<sockaddr*>(&local),&localBytes)||!local.sin_port){
            local={};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_ANY);
            if(bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local)))Fatal("RIO bind",WSAGetLastError());
        }
        requests=api.RIOCreateRequestQueue(socket,0,1,BatchPackets,1,completion,completion,nullptr);
        if(requests==RIO_INVALID_RQ)Fatal("RIO request queue",WSAGetLastError());
        address.Ipv4=destination;
        registeredCapacity=capacity;
        dataId=api.RIORegisterBuffer(data,capacity);addressId=api.RIORegisterBuffer(reinterpret_cast<char*>(&address),sizeof(address));
        if(dataId==RIO_INVALID_BUFFERID||addressId==RIO_INVALID_BUFFERID)Fatal("RIO registered buffer",WSAGetLastError());
    }
    void Send(ULONG bytes,ULONG segment){
        using Timer=std::chrono::steady_clock;LastWaitMaxMs=0;LastCallMaxMs=0;
        if(!segment||bytes%segment||bytes>registeredCapacity)Fatal("RIO batch size",WSAEMSGSIZE);
        const ULONG count=bytes/segment;
        RIO_BUF remote{addressId,0,sizeof(address)};
        for(ULONG begin=0;begin<count;begin+=BatchPackets){
            const ULONG batch=std::min(BatchPackets,count-begin);
            for(ULONG i=0;i<batch;++i){
                RIO_BUF data{dataId,(begin+i)*segment,segment};
                const auto before=Timer::now();
                if(!api.RIOSendEx(requests,&data,1,nullptr,&remote,nullptr,nullptr,RIO_MSG_DEFER,nullptr))Fatal("RIO send",WSAGetLastError());
                LastCallMaxMs=std::max(LastCallMaxMs,std::chrono::duration<double,std::milli>(Timer::now()-before).count());
            }
            const auto commit=Timer::now();
            if(!api.RIOSendEx(requests,nullptr,0,nullptr,nullptr,nullptr,nullptr,RIO_MSG_COMMIT_ONLY,nullptr))Fatal("RIO commit",WSAGetLastError());
            LastCallMaxMs=std::max(LastCallMaxMs,std::chrono::duration<double,std::milli>(Timer::now()-commit).count());
            ULONG completed=0;
            while(completed<batch){
                RIORESULT results[128]{};ULONG got=api.RIODequeueCompletion(completion,results,128);
                if(got==RIO_CORRUPT_CQ)Fatal("RIO completion corruption",WSAEFAULT);
                if(!got){
                    int status=api.RIONotify(completion);if(status)Fatal("RIO notify",status);
                    const auto wait=Timer::now();
                    if(WaitForSingleObject(event,2000)!=WAIT_OBJECT_0)Fatal("RIO completion timeout",WSAETIMEDOUT);
                    LastWaitMaxMs=std::max(LastWaitMaxMs,std::chrono::duration<double,std::milli>(Timer::now()-wait).count());
                    continue;
                }
                for(ULONG i=0;i<got;++i)if(results[i].Status)Fatal("RIO send completion",results[i].Status);
                completed+=got;
            }
        }
    }
};
