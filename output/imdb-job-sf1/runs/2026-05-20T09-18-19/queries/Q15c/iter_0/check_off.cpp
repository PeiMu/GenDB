#include <cstdio>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main(int argc, char* argv[]){
    std::string store=argv[1];
    MmapColumn<int32_t> mc(store+"/_idx/movie_companies__movie_id__offsets.bin");
    printf("mc_off size=%zu first=%d last=%d second_last=%d\n", mc.size(), mc[0], mc[mc.size()-1], mc[mc.size()-2]);
    // mc table has 2609129 rows. Last offset should be 2609129
    // check id=1 range
    printf("mc_off[0]=%d mc_off[1]=%d mc_off[2]=%d\n", mc[0],mc[1],mc[2]);
    MmapColumn<int32_t> at(store+"/_idx/aka_title__movie_id__offsets.bin");
    printf("at_off size=%zu first=%d last=%d\n", at.size(), at[0], at[at.size()-1]);
    MmapColumn<int32_t> mk(store+"/_idx/movie_keyword__movie_id__offsets.bin");
    printf("mk_off size=%zu first=%d last=%d\n", mk.size(), mk[0], mk[mk.size()-1]);
    MmapColumn<int32_t> mc_mid(store+"/movie_companies/movie_id.bin");
    printf("mc rows=%zu first_movie_id=%d second=%d\n", mc_mid.size(), mc_mid[0], mc_mid[1]);
    return 0;
}
