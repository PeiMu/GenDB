#include <cstdio>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main(int argc, char* argv[]){
    std::string store=argv[1];
    MmapColumn<int32_t> mc_off(store+"/_idx/movie_companies__movie_id__offsets.bin");
    MmapColumn<int32_t> mc_mid(store+"/movie_companies/movie_id.bin");
    MmapColumn<int32_t> mc_cid(store+"/movie_companies/company_id.bin");
    // Find mc rows for movie_id=9133
    for (int mv=9131; mv<=9135; ++mv) {
        printf("mc_off[%d]=%d mc_off[%d]=%d\n", mv, mc_off[mv], mv+1, mc_off[mv+1]);
    }
    // Scan actual mc movie_ids around index 7460-7470
    for (int r=7458; r<7470; ++r) {
        printf("mc[%d]: movie_id=%d company_id=%d\n", r, mc_mid[r], mc_cid[r]);
    }
    // Also scan for movie_id=9133 directly
    int cnt=0;
    for (size_t i=0;i<mc_mid.size();++i) {
        if (mc_mid[i]==9133) { printf("  -> mc[%zu]: company_id=%d\n", i, mc_cid[i]); cnt++; }
    }
    printf("total mc rows for 9133: %d\n", cnt);
    return 0;
}
