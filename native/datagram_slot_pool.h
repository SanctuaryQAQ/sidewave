#pragma once
#include <cstddef>

// Completion callbacks can arrive in a different order from submission.
// size() means every slot is still owned by an outstanding send.
template<class Slots>
size_t FindFreeDatagramSlot(const Slots& slots,size_t preferred){
 if(preferred<slots.size()&&!slots[preferred].active)return preferred;
 for(size_t i=0;i<slots.size();++i)if(!slots[i].active)return i;
 return slots.size();
}
