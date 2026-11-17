#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#include "util/crc32.h"

uint32_t reference(const void* data, size_t size, uint32_t seed) {
  auto p=static_cast<const unsigned char*>(data);
  uint32_t crc=~seed;
  for(size_t i=0;i<size;++i){crc^=p[i];for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xEDB88320u:0u);}
  return ~crc;
}
int main(){
  if(mvdb::crc32(nullptr,0)!=0 || mvdb::crc32("123456789",9)!=0xCBF43926u) return 1;
  std::vector<unsigned char> bytes(65544);uint32_t state=12345;
  for(auto& byte:bytes){state^=state<<13;state^=state>>17;state^=state<<5;byte=static_cast<unsigned char>(state);}
  std::vector<size_t> lengths;for(size_t n=0;n<34;++n)lengths.push_back(n);
  for(size_t n:{255u,256u,257u,8191u,8192u,8193u,65536u})lengths.push_back(n);
  for(size_t offset=0;offset<8;++offset)for(size_t n:lengths)for(uint32_t seed:{0u,0x12345678u,0xFFFFFFFFu}){
    const auto* p=bytes.data()+offset;
    const uint32_t want=reference(p,n,seed);
    const uint32_t got=mvdb::crc32(p,n,seed);
    const size_t split=n/2;
    const uint32_t chained=mvdb::crc32(p+split,n-split,mvdb::crc32(p,split,seed));
    if(got!=want || chained!=want){std::fprintf(stderr,"CRC mismatch offset=%zu size=%zu seed=%u\n",offset,n,seed);return 1;}
  }
  std::puts("crc32: known vectors, unaligned buffers, boundaries and incremental seeds passed");
}
