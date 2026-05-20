#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main() {
    std::string sdir = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    MmapColumn<char> t_off_r(sdir + "/title/title.off");
    MmapColumn<char> t_dat(sdir + "/title/title.dat");
    const int64_t* off = reinterpret_cast<const int64_t*>(t_off_r.data);
    size_t n = t_off_r.file_size/8 - 1;
    for (size_t i = 0; i < n; i++) {
        size_t l = off[i+1]-off[i];
        if (l == 12 && std::memcmp(t_dat.data+off[i], "2001 Maniacs", 12) == 0) {
            std::printf("2001 Maniacs at title row %zu (id=%zu)\n", i, i+1);
        }
    }
    // find Lionsgate companies
    MmapColumn<char> cn_off_r(sdir + "/company_name/name.off");
    MmapColumn<char> cn_dat(sdir + "/company_name/name.dat");
    const int64_t* coff = reinterpret_cast<const int64_t*>(cn_off_r.data);
    size_t nc = cn_off_r.file_size/8 - 1;
    int cnt=0;
    for (size_t i = 0; i < nc; i++) {
        size_t l = coff[i+1]-coff[i];
        if (l >= 9 && std::memcmp(cn_dat.data+coff[i], "Lionsgate", 9) == 0) {
            std::printf("CN row %zu (id=%zu) name=%.*s\n", i, i+1, (int)l, cn_dat.data+coff[i]);
            cnt++;
        }
    }
    std::printf("Total Lionsgate companies: %d\n", cnt);
    return 0;
}
