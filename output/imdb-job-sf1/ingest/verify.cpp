// Spot-check the generated storage.
#include "common.h"

int main() {
    std::string s = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";

    // 1. title.production_year sanity: count non-null, min/max
    {
        auto py = read_vec<int32_t>(s + "/title/production_year.bin");
        int64_t nn = 0, mn = INT32_MAX, mx = INT32_MIN, sum = 0;
        for (auto v : py) if (v != NULL_INT) { ++nn; mn = std::min<int64_t>(mn, v); mx = std::max<int64_t>(mx, v); sum += v; }
        printf("title.production_year: rows=%zu non-null=%lld min=%lld max=%lld avg=%.0f\n",
               py.size(), (long long)nn, (long long)mn, (long long)mx, nn ? sum/(double)nn : 0);
    }
    // 2. title id placement: id[i] == i+1
    {
        auto id = read_vec<int32_t>(s + "/title/id.bin");
        bool ok = true;
        for (size_t i = 0; i < id.size(); ++i) if (id[i] != (int32_t)(i+1)) { ok = false; printf("title.id[%zu]=%d\n", i, id[i]); break; }
        printf("title.id contiguous 1..N: %s (n=%zu)\n", ok ? "yes" : "NO", id.size());
    }
    // 3. cast_info sorted by movie_id
    {
        auto mid = read_vec<int32_t>(s + "/cast_info/movie_id.bin");
        bool ok = true;
        for (size_t i = 1; i < mid.size(); ++i) if (mid[i] < mid[i-1]) { ok = false; printf("cast_info.movie_id[%zu]=%d < [%zu]=%d\n", i, mid[i], i-1, mid[i-1]); break; }
        printf("cast_info sorted by movie_id: %s (n=%zu)\n", ok ? "yes" : "NO", mid.size());
    }
    // 4. gender dict + sample values
    {
        auto dict_off = read_vec<int64_t>(s + "/name/gender.dict.off");
        auto dict_dat = read_file(s + "/name/gender.dict.dat");
        printf("name.gender dict entries=%zu: ", dict_off.size()-1);
        for (size_t i = 0; i + 1 < dict_off.size(); ++i)
            printf("[%zu]=\"%.*s\" ", i+1, (int)(dict_off[i+1]-dict_off[i]), (char*)dict_dat.data()+dict_off[i]);
        printf("\n");
        auto codes = read_vec<int8_t>(s + "/name/gender.bin");
        int64_t c[4] = {0,0,0,0};
        for (auto v : codes) c[(int)v & 3]++;
        printf("name.gender code histogram: NULL=%lld c1=%lld c2=%lld c3=%lld (total=%zu)\n",
               (long long)c[0], (long long)c[1], (long long)c[2], (long long)c[3], codes.size());
    }
    // 5. company_name country_code dict + a few entries
    {
        auto dict_off = read_vec<int64_t>(s + "/company_name/country_code.dict.off");
        auto dict_dat = read_file(s + "/company_name/country_code.dict.dat");
        printf("company_name.country_code dict entries=%zu, first few: ", dict_off.size()-1);
        for (size_t i = 0; i < 5 && i + 1 < dict_off.size(); ++i)
            printf("[%zu]=\"%.*s\" ", i+1, (int)(dict_off[i+1]-dict_off[i]), (char*)dict_dat.data()+dict_off[i]);
        printf("\n");
    }
    // 6. info_type entries (small table, dump all)
    {
        auto off = read_vec<int64_t>(s + "/info_type/info.off");
        auto dat = read_file(s + "/info_type/info.dat");
        printf("info_type rows=%zu first 5: ", off.size()-1);
        for (size_t i = 0; i < 5 && i + 1 < off.size(); ++i)
            printf("[%zu]=\"%.*s\" ", i+1, (int)(off[i+1]-off[i]), (char*)dat.data()+off[i]);
        printf("\n");
    }
    // 7. title.title – fetch row for id=80889 (we saw it = "(#1.66)")
    {
        auto off = read_vec<int64_t>(s + "/title/title.off");
        auto dat = read_file(s + "/title/title.dat");
        int32_t id = 80889;
        printf("title.title[id=%d] = \"%.*s\"\n", id, (int)(off[id] - off[id-1]), (char*)dat.data() + off[id-1]);
    }
    // 8. movie_info__movie_id offsets: lookups for movie_id=2 (we saw 2 mi rows for it)
    {
        auto off = read_vec<int32_t>(s + "/_idx/movie_info__movie_id__offsets.bin");
        printf("movie_info row range for movie_id=2: [%d, %d) (%d rows)\n", off[2], off[3], off[3]-off[2]);
        printf("movie_info row range for movie_id=1: [%d, %d) (%d rows)\n", off[1], off[2], off[2]-off[1]);
    }
    // 9. keyword__keyword_id CSR: lookup for keyword_id=1 (we saw mk row had keyword_id=1)
    {
        auto off = read_vec<int32_t>(s + "/_idx/movie_keyword__keyword_id__offsets.bin");
        auto rid = read_vec<int32_t>(s + "/_idx/movie_keyword__keyword_id__rowids.bin");
        printf("movie_keyword rows for keyword_id=1: count=%d first rowid=%d\n",
               off[2]-off[1], off[2]>off[1] ? rid[off[1]] : -1);
    }
    return 0;
}
