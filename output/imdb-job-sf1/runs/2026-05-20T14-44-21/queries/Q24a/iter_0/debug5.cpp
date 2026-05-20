#define _GNU_SOURCE
#include <cstdio>
#include <cstring>
#include "mmap_utils.h"
using gendb::MmapColumn;
int main() {
    std::string g = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    MmapColumn<int64_t> cd_off(g + "/company_name/country_code.dict.off");
    MmapColumn<char> cd_dat(g + "/company_name/country_code.dict.dat");
    for (size_t i = 0; i + 1 < cd_off.count; i++) {
        size_t l = cd_off[i+1] - cd_off[i];
        if (l == 4 && memcmp(cd_dat.data + cd_off[i], "[us]", 4) == 0) {
            printf("'[us]' at dict_index=%zu\n", i);
        }
    }
    // Count distinct codes in country_code.bin
    MmapColumn<int16_t> cc(g + "/company_name/country_code.bin");
    int n0 = 0, n1 = 0, n2 = 0;
    int max_code = 0, min_code = 32767;
    for (size_t i = 0; i < cc.count; i++) {
        int16_t v = cc[i];
        if (v == 0) n0++;
        else if (v == 1) n1++;
        else if (v == 2) n2++;
        if (v > max_code) max_code = v;
        if (v < min_code) min_code = v;
    }
    printf("cc count[0]=%d count[1]=%d count[2]=%d min=%d max=%d\n", n0, n1, n2, min_code, max_code);
    return 0;
}
