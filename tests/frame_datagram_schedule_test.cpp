#include "frame_datagram_schedule.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <set>
#include <utility>

static void Check(bool value) { if(!value) throw std::runtime_error("schedule or reconstruction mismatch"); }
using Fragment=std::array<uint8_t,31>;
static bool RecoverBurst(bool interleave, size_t start, size_t length) {
 const std::vector<uint16_t> counts(14,23);
 std::vector<std::vector<Fragment>> data(14);
 std::vector<Fragment> parity(14);
 for(size_t i=0;i<data.size();++i) {
  data[i].resize(counts[i]);
  for(size_t f=0;f<counts[i];++f) for(size_t b=0;b<31;++b) {
   data[i][f][b]=uint8_t(i*19+f*13+b*7);parity[i][b]^=data[i][f][b];
  }
 }
 std::vector<FrameDatagramPart> schedule;BuildFrameDatagramSchedule(counts,true,interleave,schedule);
 std::vector<std::set<size_t>> missing(14);std::vector<bool> haveParity(14);
 for(size_t i=0;i<14;++i) for(size_t f=0;f<23;++f) missing[i].insert(f);
 for(size_t n=0;n<schedule.size();++n) {
  if(n>=start&&n<start+length)continue;
  const auto part=schedule[n];if(part.fragment==counts[part.packet])haveParity[part.packet]=true;
  else missing[part.packet].erase(part.fragment);
 }
 for(size_t i=0;i<14;++i) {
  if(missing[i].empty())continue;
  if(missing[i].size()!=1||!haveParity[i])return false;
  const size_t lost=*missing[i].begin();auto restored=parity[i];
  for(size_t f=0;f<23;++f)if(f!=lost)for(size_t b=0;b<31;++b)restored[b]^=data[i][f][b];
  Check(restored==data[i][lost]);
 }
 return true;
}
int main() {
 try {
  for(auto counts:{std::vector<uint16_t>{},std::vector<uint16_t>{1},std::vector<uint16_t>{1,3,2},std::vector<uint16_t>{23,23,7,23}}) {
   for(bool fec:{false,true}) {
    std::vector<FrameDatagramPart> a,b;BuildFrameDatagramSchedule(counts,fec,false,a);BuildFrameDatagramSchedule(counts,fec,true,b);
    std::set<std::pair<uint16_t,uint16_t>> sa,sb;for(auto p:a)sa.insert({p.packet,p.fragment});for(auto p:b)sb.insert({p.packet,p.fragment});
    Check(sa==sb&&sa.size()==a.size()&&sb.size()==b.size());
    if(!fec){for(size_t n=0;n<a.size();++n)Check(a[n].packet==b[n].packet&&a[n].fragment==b[n].fragment);}
    else{bool parity=false;for(auto p:b){if(p.fragment==counts[p.packet])parity=true;else Check(!parity);}}
   }
  }
  // Every burst of up to 14 adjacent data fragments remains recoverable with
  // the same single XOR parity per codec packet. Test the exact payload bytes.
  for(size_t length=1;length<=14;++length)for(size_t start=0;start+length<=14*23;++start)Check(RecoverBurst(true,start,length));
  Check(!RecoverBurst(false,0,2));Check(!RecoverBurst(true,0,15));
  std::puts("Passed: identical datagrams; no-FEC order; parity order; all short data bursts; exact XOR payload recovery; unrecoverable burst boundary.");
  return 0;
 } catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
