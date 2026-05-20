#include <cstdio>
#include <cstdint>
#include "mmap_utils.h"
using namespace gendb;
int main() {
    MmapColumn<int8_t> ng("/home/pei/Project/GenDB/output/imdb-job-sf1/storage/name/gender.bin");
    int hist[256] = {0};
    for (size_t i = 0; i < ng.count; i++) hist[(uint8_t)ng.data[i]]++;
    for (int i = 0; i < 256; i++) if (hist[i]) std::printf("code=%d count=%d\n", i, hist[i]);
    return 0;
}
