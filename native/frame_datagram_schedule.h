#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

struct FrameDatagramPart { uint16_t packet, fragment; };

// Framing and parity bytes are unchanged. Spreading adjacent data fragments
// over codec packets lets their existing single-parity protection repair a
// short burst. No pacing delay, retransmission or extra parity is introduced.
inline void BuildFrameDatagramSchedule(const std::vector<uint16_t>& fragments,
                                      bool fec, bool interleave,
                                      std::vector<FrameDatagramPart>& parts) {
 parts.clear();size_t total=0;uint16_t longest=0;
 for(auto n:fragments){total+=n+(fec?1:0);longest=std::max(longest,n);}
 parts.reserve(total);
 if(fec&&interleave){
  for(uint16_t f=0;f<longest;++f)
   for(size_t i=0;i<fragments.size();++i)
    if(f<fragments[i])parts.push_back({uint16_t(i),f});
  for(size_t i=0;i<fragments.size();++i)parts.push_back({uint16_t(i),fragments[i]});
 }else{
  for(size_t i=0;i<fragments.size();++i)
   for(uint32_t f=0;f<uint32_t(fragments[i])+(fec?1u:0u);++f)
    parts.push_back({uint16_t(i),uint16_t(f)});
 }
}
