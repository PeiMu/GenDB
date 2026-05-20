#define _GNU_SOURCE
#include <cstdio>
#include "mmap_utils.h"
using gendb::MmapColumn;
int main() {
    std::string g = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    MmapColumn<int8_t> gender(g + "/name/gender.bin");
    int cnt[256] = {0};
    for (size_t i = 0; i < gender.count; i++) cnt[(uint8_t)gender[i]]++;
    for (int i = 0; i < 256; i++) if (cnt[i]) printf("  code=%d (signed=%d) count=%d\n", i, (int8_t)i, cnt[i]);
    return 0;
}
