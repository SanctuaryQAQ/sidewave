#include "pyrowave_bitstream.hpp"
#include <cstdio>
#include <stdexcept>
#include <cstring>
using namespace PyroWave;
static void Check(bool v){if(!v)throw std::runtime_error("sequence regression failed");}
static std::vector<uint8_t> Frame(const BlockLayout&layout,unsigned sequence,bool complete=true){
 const size_t blocks=layout.block_count_32x32;
 std::vector<uint8_t> bytes(sizeof(BitstreamSequenceHeader)+(blocks-(complete?0:1))*sizeof(BitstreamHeader));
 BitstreamSequenceHeader start{};start.width_minus_1=layout.width-1;start.height_minus_1=layout.height-1;start.sequence=sequence&7;start.extended=1;start.total_blocks=uint32_t(blocks);start.chroma_resolution=CHROMA_RESOLUTION_444;
 std::memcpy(bytes.data(),&start,sizeof(start));
 for(size_t i=0;i<blocks-(complete?0:1);++i){BitstreamHeader h{};h.sequence=sequence&7;h.block_index=uint32_t(i);h.payload_words=2;std::memcpy(bytes.data()+sizeof(start)+i*sizeof(h),&h,sizeof(h));}
 return bytes;
}
int main(){try{
 BlockLayout layout;Check(layout.init(128,128,ChromaSubsampling::Chroma444));BitstreamParser parser;parser.init(&layout);
 auto first=Frame(layout,0);Check(parser.push_packet(first.data(),first.size()));Check(parser.decode_is_ready(false));parser.mark_frame_decoded();
 auto afterLoss=Frame(layout,4);Check(parser.push_packet(afterLoss.data(),afterLoss.size()));Check(!parser.decode_is_ready(false));
 for(unsigned outer=4;outer<1028;++outer){parser.clear();auto frame=Frame(layout,outer);Check(parser.push_packet(frame.data(),frame.size()));Check(parser.decode_is_ready(false));parser.mark_frame_decoded();}
 parser.clear();auto partial=Frame(layout,1028,false);Check(parser.push_packet(partial.data(),partial.size()));Check(!parser.decode_is_ready(false));
 std::puts("Passed: upstream 3-bit gap rejection reproduced; explicit per-frame clear accepts 1024 independent frames across wraps; incomplete frame still rejected.");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
