#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <vector>
#include "mmap_utils.h"
using gendb::MmapColumn;
int main() {
    std::string g = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    // Find title "Baiohazâdo 6"
    MmapColumn<int64_t> title_off(g + "/title/title.off");
    MmapColumn<char> title_dat(g + "/title/title.dat");
    MmapColumn<int32_t> title_year(g + "/title/production_year.bin");
    for (size_t i = 0; i < title_year.count; i++) {
        int64_t lo = title_off[i], hi = title_off[i+1];
        size_t l = hi - lo;
        std::string s(title_dat.data + lo, l);
        if (s.find("Baiohaz") != std::string::npos) {
            printf("title id=%zu '%s' year=%d\n", i+1, s.c_str(), title_year[i]);
        }
    }
    return 0;
}
