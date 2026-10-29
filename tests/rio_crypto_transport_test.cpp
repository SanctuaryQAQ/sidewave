#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <algorithm>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include "../native/video_crypto.h"
#include "../native/pyrowave_rio_sender.h"
#include "../native/pyrowave_rio_receiver.h"
#include "../native/pyrowave_coalesced_receiver.h"
int wmain(int argc,wchar_t**argv){
 if(argc<2)return 1;
 bool encrypted=true,receiveRio=false,sendStandard=false,receiveCoalesced=false;
 for(int i=2;i<argc;++i){if(!wcscmp(argv[i],L"--plain"))encrypted=false;else if(!wcscmp(argv[i],L"--receive-rio"))receiveRio=true;else if(!wcscmp(argv[i],L"--receive-coalesced"))receiveCoalesced=true;else if(!wcscmp(argv[i],L"--send-standard"))sendStandard=true;else return 1;}
 if(receiveRio&&receiveCoalesced)return 1;
 if(!CoalescedDatagramReader::ValidSegments(3*1456+111,1456,true)||CoalescedDatagramReader::ValidSegments(1473,0,false)||CoalescedDatagramReader::ValidSegments(65537,1456,true)||CoalescedDatagramReader::ValidSegments(100,0,true)||CoalescedDatagramReader::ValidSegments(1,1500,true))return 15;
 SOCKET sender=INVALID_SOCKET,receiver=INVALID_SOCKET;
 try{
  WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
  unsigned char secret[44];if(BCryptGenRandom(nullptr,secret,sizeof(secret),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return 3;
  DATA_BLOB in{sizeof(secret),secret},protectedKey{};
  if(!CryptProtectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&protectedKey))return 4;
  SecureZeroMemory(secret,sizeof(secret));std::wstring keyPath=std::wstring(argv[1])+L".dpapi";
  HANDLE keyFile=CreateFileW(keyPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);DWORD written=0;
  BOOL saved=keyFile!=INVALID_HANDLE_VALUE&&WriteFile(keyFile,protectedKey.pbData,protectedKey.cbData,&written,nullptr);
  if(keyFile!=INVALID_HANDLE_VALUE)CloseHandle(keyFile);
  const DWORD expectedKeyBytes=protectedKey.cbData;SecureZeroMemory(protectedKey.pbData,protectedKey.cbData);LocalFree(protectedKey.pbData);
  if(!saved||written!=expectedKeyBytes)return 5;
  VideoCrypto tx,rx;tx.SetEncrypted(encrypted);rx.SetEncrypted(encrypted);tx.Load(keyPath.c_str());rx.Load(keyPath.c_str());DeleteFileW(keyPath.c_str());
  receiver=receiveRio?WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_REGISTERED_IO):socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sender=sendStandard?socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP):WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_REGISTERED_IO);
  if(receiver==INVALID_SOCKET||sender==INVALID_SOCKET)return 6;
  int bufferBytes=16*1024*1024;DWORD timeout=2000;
  if(setsockopt(receiver,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&bufferBytes),sizeof(bufferBytes))||setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout)))return 7;
  sockaddr_in destination{};destination.sin_family=AF_INET;destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(receiver,reinterpret_cast<sockaddr*>(&destination),sizeof(destination)))return 8;
  int addressBytes=sizeof(destination);if(getsockname(receiver,reinterpret_cast<sockaddr*>(&destination),&addressBytes))return 9;
  std::unique_ptr<CoalescedDatagramReader> coalesced;
  if(receiveCoalesced){coalesced.reset(new CoalescedDatagramReader());if(!coalesced->Init(receiver))coalesced.reset();}
  std::unique_ptr<RioDatagramReader> rio;
  if(receiveRio){rio.reset(new RioDatagramReader());rio->Init(receiver);char empty[1472];sockaddr_in peer{};for(int idle=0;idle<3;++idle)if(rio->Receive(empty,sizeof(empty),peer,10)!=SOCKET_ERROR||WSAGetLastError()!=WSAETIMEDOUT)return 14;}
  constexpr ULONG count=4097;const ULONG segment=encrypted?1472:1456;
  std::vector<unsigned char> wire(size_t(count)*segment);
  std::vector<ULONG> wireLengths(count),payloadLengths(count);
  unsigned char header[28]{},payload[1412]{};
  for(ULONG i=0;i<count;++i){std::memcpy(header,&i,sizeof(i));std::memset(payload,int(i%251),sizeof(payload));payloadLengths[i]=sendStandard&&receiveCoalesced&&i%29==0?37:sizeof(payload);wireLengths[i]=ULONG(tx.Seal(header,sizeof(header),payload,payloadLengths[i],wire.data()+size_t(i)*segment));if(wireLengths[i]!=payloadLengths[i]+28+(encrypted?32:16))return 10;}
  std::atomic<ULONG> received{0},invalid{0};std::atomic<int> receiveError{0};
  std::thread reader([&]{
   unsigned char datagram[1472],plain[1472];std::vector<bool> seen(count,false);
   for(ULONG j=0;j<count;++j){
    sockaddr_in peer{};int bytes=rio?rio->Receive(reinterpret_cast<char*>(datagram),sizeof(datagram),peer,2000):coalesced?coalesced->Receive(reinterpret_cast<char*>(datagram),sizeof(datagram),peer):recv(receiver,reinterpret_cast<char*>(datagram),sizeof(datagram),0);
    if(bytes==SOCKET_ERROR){receiveError=WSAGetLastError();break;}
    if((rio||coalesced)&&peer.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)){++invalid;continue;}
    int decoded=rx.Open(datagram,bytes,plain);if(decoded<28){++invalid;continue;}
    ULONG index=0;std::memcpy(&index,plain,sizeof(index));
    if(index>=count||seen[index]||bytes!=int(wireLengths[index])||decoded!=int(payloadLengths[index]+28)){++invalid;continue;}
    seen[index]=true;bool valid=true;
    for(size_t k=28;k<size_t(decoded);++k)if(plain[k]!=index%251){valid=false;break;}
    if(!valid)++invalid;else ++received;
   }
  });
  const auto begin=std::chrono::steady_clock::now();
  if(sendStandard){for(ULONG i=0;i<count;++i){if(sendto(sender,reinterpret_cast<char*>(wire.data()+size_t(i)*segment),wireLengths[i],0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))!=int(wireLengths[i])){receiveError=WSAGetLastError();break;}}}
  else{RioDatagramWriter writer;writer.Init(sender,destination,reinterpret_cast<char*>(wire.data()),ULONG(wire.size()));writer.Send(ULONG(wire.size()),segment);}
  reader.join();const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
  const uint64_t batches=rio?rio->Batches():0;
  const uint64_t coalescedMessages=coalesced?coalesced->MergedMessages():0,maxBatch=coalesced?coalesced->MaximumBatch():0;
  const bool coalescingEnabled=bool(coalesced);
  closesocket(sender);sender=INVALID_SOCKET;if(rio)rio.reset();else closesocket(receiver);receiver=INVALID_SOCKET;WSACleanup();
  const bool passed=received==count&&invalid==0&&receiveError==0;
  FILE* report=_wfopen(argv[1],L"wt");if(!report)return 11;
  fprintf(report,"{\"passed\":%s,\"expected\":%lu,\"received\":%lu,\"invalid\":%lu,\"receiveError\":%d,\"datagramBytes\":%lu,\"batchOver1024\":true,\"encrypted\":%s,\"receiveRio\":%s,\"sendStandard\":%s,\"receiveBatches\":%llu,\"pendingCanceled\":%s,\"receiveCoalesced\":%s,\"coalescingEnabled\":%s,\"coalescedMessages\":%llu,\"maxReceiveBatch\":%llu,\"shortTailTest\":%s,\"elapsedMs\":%.3f}\n",passed?"true":"false",count,received.load(),invalid.load(),receiveError.load(),segment,encrypted?"true":"false",receiveRio?"true":"false",sendStandard?"true":"false",(unsigned long long)batches,receiveRio?"true":"false",receiveCoalesced?"true":"false",coalescingEnabled?"true":"false",(unsigned long long)coalescedMessages,(unsigned long long)maxBatch,receiveCoalesced&&sendStandard?"true":"false",ms);fclose(report);
  return passed?0:12;
 }catch(const std::exception&e){fprintf(stderr,"transport test failed: %s\n",e.what());return 13;}
}
