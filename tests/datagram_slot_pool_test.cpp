#include "datagram_slot_pool.h"
#include <array>
#include <cstdio>
#include <cstdlib>
struct Slot{bool active=false;unsigned owner=0;};
void require(bool value,const char* reason){if(!value){std::fprintf(stderr,"FAIL: %s\n",reason);std::exit(1);}}
int main(){
 std::array<Slot,64> slots{};
 require(FindFreeDatagramSlot(slots,17)==17,"preferred free slot retained");
 for(auto& slot:slots)slot.active=true;
 require(FindFreeDatagramSlot(slots,0)==slots.size(),"full capacity requires completion");
 slots[51].active=false;
 require(FindFreeDatagramSlot(slots,0)==51,"later completion bypasses owned earlier slot");
 slots[51].active=true;
 // Keep the first send outstanding across repeated out-of-order completions.
 // The real submission helper must keep finding only completed buffers while
 // every submitted datagram remains in caller order.
 for(unsigned packet=1;packet<=8192;++packet){
  size_t completed=1+packet%63;
  slots[completed].active=false;
  size_t chosen=FindFreeDatagramSlot(slots,0);
  require(chosen==completed,"owned slots must never be reused");
  slots[chosen].active=true;slots[chosen].owner=packet;
  require(slots[0].active&&slots[0].owner==0,"old outstanding buffer remains untouched");
  require(FindFreeDatagramSlot(slots,0)==64,"inflight capacity remains bounded");
 }
 slots[0].active=false;
 require(FindFreeDatagramSlot(slots,0)==0,"eventual oldest completion reusable");
 std::puts("PASS: preferred/full/out-of-order/8192 bounded reuses");
}
