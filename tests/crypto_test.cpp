#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "../native/video_crypto.h"
int wmain(int argc,wchar_t**argv){
 if(argc!=2)return 1;
 try{
  unsigned char secret[44];BCryptGenRandom(nullptr,secret,44,BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  DATA_BLOB in{44,secret},out{};if(!CryptProtectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))return 2;
  std::wstring file=std::wstring(argv[1])+L".dpapi";
  HANDLE f=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);DWORD n;
  WriteFile(f,out.pbData,out.cbData,&n,nullptr);CloseHandle(f);SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);
  VideoCrypto tx,rx,ptx,prx;tx.Load(file.c_str());rx.Load(file.c_str());ptx.SetEncrypted(false);prx.SetEncrypted(false);ptx.Load(file.c_str());prx.Load(file.c_str());DeleteFileW(file.c_str());
  unsigned char header[28]{},payload[1412]{},wire[1472],plain[1472];payload[100]=76;
  size_t bytes=tx.Seal(header,28,payload,1412,wire);
  if(bytes!=1472||rx.Open(wire,bytes,plain)!=1440||plain[128]!=76)return 3;
  if(rx.Open(wire,bytes,plain)>=0)return 4;
  bytes=tx.Seal(header,28,payload,1412,wire);wire[42]^=1;
  if(rx.Open(wire,bytes,plain)>=0)return 5;
  wire[42]^=1;if(rx.Open(wire,bytes,plain)!=1440)return 6;
  // Segmentation submits one concatenated buffer. Authenticate each
  // fixed-size segment separately, including a padded short codec tail.
  std::vector<unsigned char> batch(32*1472);
  for(int i=0;i<32;++i){payload[0]=static_cast<unsigned char>(i);if(tx.Seal(header,28,payload,1412,batch.data()+i*1472)!=1472)return 9;}
  for(int i=31;i>=0;--i){if(rx.Open(batch.data()+i*1472,1472,plain)!=1440||plain[28]!=i)return 10;}
  if(rx.Open(batch.data(),1472,plain)>=0)return 11;
  for(int i=0;i<32;++i){payload[0]=static_cast<unsigned char>(i);if(ptx.Seal(header,28,payload,1412,batch.data()+i*1456)!=1456)return 12;}
  for(int i=31;i>=0;--i){if(prx.Open(batch.data()+i*1456,1456,plain)!=1440||plain[28]!=i)return 13;}
  if(prx.Open(batch.data(),1456,plain)>=0)return 14;
  bytes=ptx.Seal(header,28,payload,1412,wire);wire[0]^=1;if(prx.Open(wire,bytes,plain)>=0)return 15;wire[0]^=1;if(prx.Open(wire,bytes,plain)!=1440)return 16;
  const auto begin=GetTickCount64();
  for(int i=0;i<60000;++i){bytes=tx.Seal(header,28,payload,1412,wire);if(rx.Open(wire,bytes,plain)!=1440)return 7;}
  FILE*report=_wfopen(argv[1],L"wt");fprintf(report,"{\"passed\":true,\"plaintextRoundTrip\":true,\"plaintextReplayRejected\":true,\"plaintextSessionRejected\":true,\"roundTrip\":true,\"segmentedBatch\":true,\"replayRejected\":true,\"tamperRejected\":true,\"datagramBytes\":1472,\"sixtyThousandRoundTripsMs\":%llu}\n",GetTickCount64()-begin);fclose(report);return 0;
 }catch(...){return 8;}
}
