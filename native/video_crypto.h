#pragma once
#include <bcrypt.h>
#include <wincrypt.h>
#include <array>
#include <mutex>
#include <stdexcept>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"crypt32.lib")

// The GUI supplies a fresh per-stream secret through a CurrentUser-DPAPI file.
// Never put a raw key in argv, telemetry, source packages or discovery.
class VideoCrypto {
 BCRYPT_ALG_HANDLE alg=nullptr;
 BCRYPT_KEY_HANDLE key=nullptr;
 std::vector<unsigned char> object;
 std::array<unsigned char,12> prefix{};
 uint64_t sequence=0,highest=0;
 std::array<uint64_t,128> replay{}; // 8192-packet reorder window.
 bool haveSequence=false;
 bool encrypted=true;
 std::mutex sealMutex; // video sender and cursor thread share the sequence space
 static void Check(NTSTATUS n){if(n<0)throw std::runtime_error("AES-GCM initialization failed");}
public:
 ~VideoCrypto(){if(key)BCryptDestroyKey(key);if(alg)BCryptCloseAlgorithmProvider(alg,0);}
 void SetEncrypted(bool value){encrypted=value;}
 void Load(const wchar_t*path){
  HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  if(f==INVALID_HANDLE_VALUE)throw std::runtime_error("Stream secret is unavailable");
  DWORD n=GetFileSize(f,nullptr),got=0;
  if(n>16384||n==0){CloseHandle(f);throw std::runtime_error("Invalid protected secret");}
  std::vector<unsigned char> protectedData(n);BOOL ok=ReadFile(f,protectedData.data(),n,&got,nullptr);CloseHandle(f);
  DATA_BLOB in{n,protectedData.data()},out{};
  if(!ok||got!=n||!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))
   throw std::runtime_error("Cannot decrypt stream secret for this Windows user");
  if(out.cbData!=44){SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);throw std::runtime_error("Invalid secret length");}
  if(encrypted){
  Check(BCryptOpenAlgorithmProvider(&alg,BCRYPT_AES_ALGORITHM,nullptr,0));
  Check(BCryptSetProperty(alg,BCRYPT_CHAINING_MODE,(PUCHAR)BCRYPT_CHAIN_MODE_GCM,sizeof(BCRYPT_CHAIN_MODE_GCM),0));
  DWORD bytes=0,len=0;Check(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&bytes,sizeof(bytes),&len,0));object.resize(bytes);
  Check(BCryptGenerateSymmetricKey(alg,&key,object.data(),bytes,out.pbData,32,0));
  }
  std::memcpy(prefix.data(),out.pbData+32,12);
  SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);
 }
 size_t Seal(const void*header,size_t hn,const void*payload,size_t pn,unsigned char*out){
  if(hn>1440||pn>1440-hn)throw std::runtime_error("Oversized video datagram");
  std::lock_guard<std::mutex> lock(sealMutex);
  if(!encrypted){uint64_t seq=++sequence;std::memcpy(out,prefix.data()+4,8);std::memcpy(out+8,&seq,8);std::memcpy(out+16,header,hn);std::memcpy(out+16+hn,payload,pn);return 16+hn+pn;}
  unsigned char plain[1472];std::memcpy(plain,header,hn);std::memcpy(plain+hn,payload,pn);
  uint64_t seq=++sequence;std::memcpy(out,prefix.data()+4,8);std::memcpy(out+8,&seq,8);
  unsigned char nonce[12];std::memcpy(nonce,prefix.data(),4);std::memcpy(nonce+4,&seq,8);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce=nonce;info.cbNonce=12;info.pbAuthData=out;info.cbAuthData=16;
  info.pbTag=out+16+hn+pn;info.cbTag=16;ULONG written=0;
  Check(BCryptEncrypt(key,plain,ULONG(hn+pn),&info,nullptr,0,out+16,ULONG(hn+pn),&written,0));
  return 16+written+16;
 }
 int Open(const unsigned char*input,size_t n,unsigned char*out){
  if(n<(encrypted?33u:17u)||n>1472||std::memcmp(input,prefix.data()+4,8))return -1;
  uint64_t seq=0;std::memcpy(&seq,input+8,8);if(!seq)return -1;
  if(haveSequence&&seq<=highest&&highest-seq>=8192)return -1;
  size_t index=size_t(seq%8192);uint64_t bit=uint64_t(1)<<(index%64);
  if(haveSequence&&seq<=highest&&(replay[index/64]&bit))return -1;
  ULONG written=0;
  if(!encrypted){written=ULONG(n-16);std::memcpy(out,input+16,written);}else{
  unsigned char nonce[12];std::memcpy(nonce,prefix.data(),4);std::memcpy(nonce+4,&seq,8);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce=nonce;info.cbNonce=12;info.pbAuthData=const_cast<PUCHAR>(input);info.cbAuthData=16;
  info.pbTag=const_cast<PUCHAR>(input+n-16);info.cbTag=16;
  if(BCryptDecrypt(key,const_cast<PUCHAR>(input+16),ULONG(n-32),&info,nullptr,0,out,ULONG(n-32),&written,0)<0)return -1;
  }
  if(!haveSequence||seq>highest){
   if(!haveSequence||seq-highest>=8192)replay.fill(0);
   else for(uint64_t s=highest+1;s<=seq;++s){size_t i=size_t(s%8192);replay[i/64]&=~(uint64_t(1)<<(i%64));}
   highest=seq;haveSequence=true;
  }
  replay[index/64]|=bit;return int(written);
 }
};
