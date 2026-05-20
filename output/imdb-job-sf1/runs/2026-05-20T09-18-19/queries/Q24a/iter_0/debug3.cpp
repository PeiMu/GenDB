#define _GNU_SOURCE
#include <cstdio>
#include "mmap_utils.h"
using gendb::MmapColumn;
int main() {
    std::string g = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    MmapColumn<int64_t> gd_off(g + "/name/gender.dict.off");
    MmapColumn<char> gd_dat(g + "/name/gender.dict.dat");
    printf("dict.off count=%zu, dict.dat size=%zu\n", gd_off.count, gd_dat.count);
    for (size_t i = 0; i + 1 < gd_off.count; i++) {
        size_t l = gd_off[i+1] - gd_off[i];
        printf("  [%zu] len=%zu '%.*s'\n", i, l, (int)l, gd_dat.data + gd_off[i]);
    }
    // dump gd_off raw
    for (size_t i = 0; i < gd_off.count; i++) printf("  off[%zu]=%ld\n", i, gd_off[i]);
    return 0;
}
