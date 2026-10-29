#define PYROSCREEN_DATAGRAM_UNIT_TEST
#define NOMINMAX
#include <winsock2.h>
#include <atomic>
std::atomic<int> injectedFailures{0};
int injectedCode=WSAENOBUFS;
int WSAAPI PressureWSASend(SOCKET s,LPWSABUF b,DWORD n,LPDWORD sent,DWORD flags,LPWSAOVERLAPPED o,LPWSAOVERLAPPED_COMPLETION_ROUTINE c){
 if(injectedFailures.load()>0){--injectedFailures;WSASetLastError(injectedCode);return SOCKET_ERROR;}
 return ::WSASend(s,b,n,sent,flags,o,c);
}
int WSAAPI PressureWSASendTo(SOCKET s,LPWSABUF b,DWORD n,LPDWORD sent,DWORD flags,const sockaddr* to,int size,LPWSAOVERLAPPED o,LPWSAOVERLAPPED_COMPLETION_ROUTINE c){
 if(injectedFailures.load()>0){--injectedFailures;WSASetLastError(injectedCode);return SOCKET_ERROR;}
 return ::WSASendTo(s,b,n,sent,flags,to,size,o,c);
}
#define WSASend PressureWSASend
#define WSASendTo PressureWSASendTo
#include "../native/pyrowave_2k165_gpu_bench.cpp"
#undef WSASend
#undef WSASendTo
#include <stdexcept>

void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
uint8_t pattern(uint32_t frame,size_t byte){return uint8_t((frame*73+byte*31+(byte>>8))&255);}
void run(bool pipeline,bool skip,bool rio=false,bool segmented=false,bool connected=false,int pressure=0){
 constexpr size_t frameBytes=100000;
 constexpr uint32_t frames=256;
 SOCKET rx=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
 SOCKET tx=WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_OVERLAPPED|(rio?WSA_FLAG_REGISTERED_IO:0));
 require(rx!=INVALID_SOCKET&&tx!=INVALID_SOCKET,"socket creation");
 int buffer=8*1024*1024;setsockopt(rx,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&buffer),sizeof(buffer));
 DWORD timeout=2000;setsockopt(rx,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 require(!bind(rx,reinterpret_cast<sockaddr*>(&address),sizeof(address)),"loopback bind");int size=sizeof(address);
 require(!getsockname(rx,reinterpret_cast<sockaddr*>(&address),&size),"loopback address");
 if(connected)require(!connect(tx,reinterpret_cast<sockaddr*>(&address),sizeof(address)),"UDP default peer");
 if(segmented)require(!WSASetUdpSendMessageSize(tx,DWORD(WireDatagramBytes())),"UDP segmentation supported");
 std::atomic<bool> finished{false};std::atomic<uint32_t> receivedFrames{0};std::string failure;
 std::thread reader([&]{try{
  uint32_t current=0;size_t next=0;bool started=false;uint64_t wireSequence=0;
  while(!finished.load()){
   unsigned char wire[1472];sockaddr_in peer{};int n=recv(rx,reinterpret_cast<char*>(wire),sizeof(wire),0);
   if(n==SOCKET_ERROR){if(finished.load())break;throw std::runtime_error("receive timeout");}
   require(n>int(16+sizeof(DatagramHeader)),"datagram size");
   uint64_t sequence=0;std::memcpy(&sequence,wire+8,8);if(!wireSequence)wireSequence=sequence-1;require(sequence==++wireSequence,"wire submission order");
   DatagramHeader h{};std::memcpy(&h,wire+16,sizeof(h));
   require(h.magic==0x50575258u&&h.count==1&&h.index==0&&h.packetBytes==frameBytes,"header intact");
   if(!started){current=h.frame;next=0;started=true;}
   require(h.frame==current&&h.fragment==next/FragmentPayloadBytes,"frame fragment order");
   size_t bytes=std::min(FragmentPayloadBytes,frameBytes-next);
   require(n==int(16+sizeof(h)+((rio||segmented)?FragmentPayloadBytes:bytes)),"fragment size intact");
   for(size_t i=0;i<bytes;++i)require(wire[16+sizeof(h)+i]==pattern(current,next+i),"owned buffer payload intact");
   for(size_t i=bytes;i<size_t(n)-16-sizeof(h);++i)require(wire[16+sizeof(h)+i]==0,"padding contains no stale data");
   next+=bytes;
   if(next==frameBytes){++receivedFrames;++current;next=0;if(receivedFrames.load()==frames)break;}
  }
 }catch(const std::exception&e){failure=e.what();finished=true;}});
 StageTraceWriter trace(L"");
 {
  DatagramSender sender(tx,address,frameBytes,1,64,segmented,!rio,rio,false,false,0,!rio,connected,pipeline,skip,&trace);
  injectedFailures=pressure;
  std::vector<uint8_t> data(frameBytes);std::vector<pyrowave_packet> packets(1);
  for(uint32_t frame=0;frame<frames&&!finished.load();++frame){
   sender.WaitForCapacity(std::chrono::seconds(2));
   for(size_t i=0;i<frameBytes;++i)data[i]=pattern(frame,i);
   packets[0].offset=0;packets[0].size=frameBytes;
   sender.Publish(data,packets,1,frame,123,Clock::now(),SenderStageTimes{});
  }
  sender.Stop();
  auto deadline=Clock::now()+std::chrono::seconds(3);
  while(receivedFrames.load()<frames&&!finished.load()&&Clock::now()<deadline)Sleep(1);
  finished=true;reader.join();
  require(!sender.error.load(),"send completion errors");
  require(sender.totalSentFrames.load()==frames&&!sender.droppedFrames.load(),"actual completed frame accounting");
  require(sender.bufferPressureRetries.load()==uint64_t(pressure),"buffer pressure retries recorded");
  require(failure.empty(),failure.c_str());require(receivedFrames.load()==frames,"complete receiver frame accounting");
  std::printf("PASS: pipeline=%d skip_requested=%d skip_enabled=%d rio=%d segmented=%d connected=%d frames=%u immediate=%llu pending=%llu bypasses=%llu\n",pipeline,skip,sender.immediateCompletionEnabled.load(),rio,segmented,connected,receivedFrames.load(),sender.immediateSends.load(),sender.pendingSends.load(),sender.slotReuseBypasses.load());
 }
 closesocket(tx);closesocket(rx);
}
void persistentPressure(int code){
 SOCKET tx=WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_OVERLAPPED);
 require(tx!=INVALID_SOCKET,"pressure socket");
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(59999);
 StageTraceWriter trace(L"");
 {
  DatagramSender sender(tx,address,100,1,64,false,true,false,false,false,0,true,false,true,false,&trace);
  injectedCode=code;injectedFailures=100;
  std::vector<uint8_t> bytes(100);std::vector<pyrowave_packet> packets(1);packets[0].size=100;packets[0].offset=0;
  sender.Publish(bytes,packets,1,0,123,Clock::now(),SenderStageTimes{});
  const auto deadline=Clock::now()+std::chrono::seconds(2);
  while(!sender.error.load()&&Clock::now()<deadline)Sleep(1);
  require(sender.error.load()==code,"persistent pressure reports exact error");
  sender.Stop();
  require(sender.bufferPressureRetries.load()<=(code==WSAENOBUFS?8u:0u),"pressure retry bound");
  require(sender.totalSentFrames.load()==0,"failed frame never counted as sent");
  std::printf("PASS: persistent error=%d retries=%llu; bounded shutdown\n",code,sender.bufferPressureRetries.load());
 }
 injectedFailures=0;injectedCode=WSAENOBUFS;closesocket(tx);
}
int main(){
 WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return 1;
 encryptedVideo=false;videoCrypto.SetEncrypted(false);
 try{run(false,false);run(true,false);run(false,true);run(true,true);run(false,false,true);run(false,false,false,true);run(true,false,false,false,true);
  run(true,false,false,false,true,3);run(true,true,false,false,false,3);run(false,false,false,true,false,3);
  injectedCode=WSAEWOULDBLOCK;run(true,false,false,false,true,3);injectedCode=WSAENOBUFS;
  persistentPressure(WSAENOBUFS);persistentPressure(WSAENETUNREACH);
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
 WSACleanup();return 0;
}
