#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include "mmap_utils.h"
using namespace gendb;
int main() {
    std::string sdir = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    MmapColumn<int32_t> mc_movie_id(sdir + "/movie_companies/movie_id.bin");
    MmapColumn<int32_t> mc_off_co(sdir + "/_idx/movie_companies__company_id__offsets.bin");
    MmapColumn<int32_t> mc_row_co(sdir + "/_idx/movie_companies__company_id__rowids.bin");
    int32_t cn_ids[] = {2653, 3016, 6473, 7253, 7435, 9863, 39981, 57326, 65153, 194377};
    std::vector<int32_t> cand;
    for (int cid : cn_ids) {
        int32_t lo = mc_off_co.data[cid], hi = mc_off_co.data[cid+1];
        std::printf("cn=%d -> %d mc rows\n", cid, hi-lo);
        for (int k = lo; k < hi; k++) {
            int32_t r = mc_row_co.data[k];
            int32_t mv = mc_movie_id.data[r];
            cand.push_back(mv);
        }
    }
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    std::printf("total unique candidates: %zu\n", cand.size());
    // does 1638426 appear?
    auto it = std::find(cand.begin(), cand.end(), 1638426);
    std::printf("1638426 in cand: %s\n", it != cand.end() ? "YES" : "NO");

    // Check mc rows for movie_id == 1638426
    MmapColumn<int32_t> mc_company_id(sdir + "/movie_companies/company_id.bin");
    // mc sorted by movie_id; just scan
    int count = 0;
    for (size_t i = 0; i < mc_movie_id.count; i++) {
        if (mc_movie_id.data[i] == 1638426) {
            std::printf("mc row %zu: movie_id=%d company_id=%d\n", i, mc_movie_id.data[i], mc_company_id.data[i]);
            count++;
        }
    }
    std::printf("mc rows for 1638426: %d\n", count);
    return 0;
}
