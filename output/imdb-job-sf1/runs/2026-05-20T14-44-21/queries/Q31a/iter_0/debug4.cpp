#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main() {
    std::string sdir = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    MmapColumn<int8_t> ng(sdir+"/name/gender.bin");
    MmapColumn<char> nd(sdir+"/name/name.dat");
    MmapColumn<char> nor(sdir+"/name/name.off");
    const int64_t* noff = (int64_t*)nor.data;
    int32_t pids[] = {827460, 1510607};
    for (int pid : pids) {
        int8_t g = ng.data[pid-1];
        size_t l = noff[pid] - noff[pid-1];
        std::printf("pid=%d gender_code=%d name=%.*s\n", pid, (int)g, (int)l, nd.data+noff[pid-1]);
    }
    return 0;
}
