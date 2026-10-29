#pragma once
#include <mswsock.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdint>

class CoalescedDatagramReader {
    static constexpr ULONG Capacity=65536,MaxDatagram=1472;
    SOCKET socket=INVALID_SOCKET;
    LPFN_WSARECVMSG readMessage=nullptr;
    std::array<char,Capacity> data{};
    alignas(WSACMSGHDR) std::array<char,128> controls{};
    sockaddr_in source{};
    size_t offset=0,length=0,stride=0;
    std::atomic<uint64_t> messages{0},mergedMessages{0},mergedDatagrams{0},maximumBatch{0};
public:
    bool Init(SOCKET s){
        socket=s;GUID id=WSAID_WSARECVMSG;DWORD bytes=0;
        if(WSAIoctl(socket,SIO_GET_EXTENSION_FUNCTION_POINTER,&id,sizeof(id),&readMessage,sizeof(readMessage),&bytes,nullptr,nullptr))return false;
        if(WSASetUdpRecvMaxCoalescedSize(socket,Capacity))return false;
        ULONG enabled=0;
        if(WSAGetUdpRecvMaxCoalescedSize(socket,&enabled)||enabled!=Capacity){WSASetUdpRecvMaxCoalescedSize(socket,0);return false;}
        return true;
    }
    static bool ValidSegments(size_t bytes,size_t segment,bool hasInfo){
        if(bytes>Capacity)return false;
        if(!bytes)return !hasInfo;
        if(hasInfo)return segment>0&&segment<=MaxDatagram;
        return bytes<=MaxDatagram;
    }
    int Receive(char*out,size_t capacity,sockaddr_in&peer){
        if(offset==length){
            WSABUF payload{Capacity,data.data()};WSAMSG msg{};
            msg.name=reinterpret_cast<sockaddr*>(&source);msg.namelen=sizeof(source);
            msg.lpBuffers=&payload;msg.dwBufferCount=1;msg.Control={ULONG(controls.size()),controls.data()};
            DWORD received=0;if(readMessage(socket,&msg,&received,nullptr,nullptr))return SOCKET_ERROR;
            ++messages;bool hasInfo=false;DWORD segment=0;
            for(WSACMSGHDR*control=WSA_CMSG_FIRSTHDR(&msg);control;control=WSA_CMSG_NXTHDR(&msg,control)){
                if(control->cmsg_level==IPPROTO_UDP&&control->cmsg_type==UDP_COALESCED_INFO){
                    if(hasInfo||control->cmsg_len<WSA_CMSG_LEN(sizeof(DWORD))){WSASetLastError(WSAEMSGSIZE);return SOCKET_ERROR;}
                    hasInfo=true;std::memcpy(&segment,WSA_CMSG_DATA(control),sizeof(segment));
                }
            }
            if((msg.dwFlags&(MSG_CTRUNC|MSG_TRUNC))||source.sin_family!=AF_INET||!ValidSegments(received,segment,hasInfo)){WSASetLastError(WSAEMSGSIZE);return SOCKET_ERROR;}
            offset=0;length=received;stride=hasInfo?segment:received;
            if(!length){peer=source;return 0;}
            const uint64_t count=(length+stride-1)/stride;
            if(count>1){++mergedMessages;mergedDatagrams.fetch_add(count);}
            if(count>maximumBatch.load())maximumBatch.store(count);
        }
        const size_t bytes=std::min(stride,length-offset);
        if(bytes>capacity){offset=length;WSASetLastError(WSAEMSGSIZE);return SOCKET_ERROR;}
        std::memcpy(out,data.data()+offset,bytes);offset+=bytes;peer=source;return int(bytes);
    }
    uint64_t Messages()const{return messages.load();}
    uint64_t MergedMessages()const{return mergedMessages.load();}
    uint64_t MergedDatagrams()const{return mergedDatagrams.load();}
    uint64_t MaximumBatch()const{return maximumBatch.load();}
};
