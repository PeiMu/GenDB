// Q11a - generated
#include "timing_utils.h"
#include "mmap_utils.h"
#include "cli_params.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace gendb;

// lex compare for varlen strings (raw bytes)
static inline bool lt_strview(const uint8_t* a, uint32_t la, const uint8_t* b, uint32_t lb) {
    uint32_t n = la < lb ? la : lb;
    int c = std::memcmp(a, b, n);
    if (c != 0) return c < 0;
    return la < lb;
}

struct MinStr {
    const uint8_t* ptr = nullptr;
    uint32_t len = 0;
    void update(const uint8_t* p, uint32_t l) {
        if (ptr == nullptr || lt_strview(p, l, ptr, len)) {
            ptr = p;
            len = l;
        }
    }
    void merge(const MinStr& o) {
        if (o.ptr != nullptr) update(o.ptr, o.len);
    }
};

int main(int argc, char* argv[]) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string gendb = argv[1];
    std::string results = argv[2];
    mkdir(results.c_str(), 0755);

    // ----- data loading -----
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cn_cc_dict_off;
    MmapColumn<uint8_t> cn_cc_dict_dat;
    MmapColumn<int64_t> cn_name_off;
    MmapColumn<uint8_t> cn_name_dat;

    MmapColumn<int64_t> ct_kind_off;
    MmapColumn<uint8_t> ct_kind_dat;

    MmapColumn<int64_t> kw_off;
    MmapColumn<uint8_t> kw_dat;

    MmapColumn<int64_t> lt_link_off;
    MmapColumn<uint8_t> lt_link_dat;

    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int32_t> mc_company_type_id;
    MmapColumn<int64_t> mc_note_off;

    MmapColumn<int32_t> mk_keyword_id;

    MmapColumn<int32_t> ml_link_type_id;

    MmapColumn<int32_t> t_py;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<uint8_t> t_title_dat;

    // indexes
    MmapColumn<int32_t> idx_mc_cn_off;
    MmapColumn<int32_t> idx_mc_cn_rowids;
    MmapColumn<int32_t> idx_mk_mv_off;
    MmapColumn<int32_t> idx_ml_mv_off;

    {
        GENDB_PHASE("data_loading");
        cn_cc.open(gendb + "/company_name/country_code.bin");
        cn_cc_dict_off.open(gendb + "/company_name/country_code.dict.off");
        cn_cc_dict_dat.open(gendb + "/company_name/country_code.dict.dat");
        cn_name_off.open(gendb + "/company_name/name.off");
        cn_name_dat.open(gendb + "/company_name/name.dat");

        ct_kind_off.open(gendb + "/company_type/kind.off");
        ct_kind_dat.open(gendb + "/company_type/kind.dat");

        kw_off.open(gendb + "/keyword/keyword.off");
        kw_dat.open(gendb + "/keyword/keyword.dat");

        lt_link_off.open(gendb + "/link_type/link.off");
        lt_link_dat.open(gendb + "/link_type/link.dat");

        mc_movie_id.open(gendb + "/movie_companies/movie_id.bin");
        mc_company_id.open(gendb + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gendb + "/movie_companies/company_type_id.bin");
        mc_note_off.open(gendb + "/movie_companies/note.off");

        mk_keyword_id.open(gendb + "/movie_keyword/keyword_id.bin");
        ml_link_type_id.open(gendb + "/movie_link/link_type_id.bin");

        t_py.open(gendb + "/title/production_year.bin");
        t_title_off.open(gendb + "/title/title.off");
        t_title_dat.open(gendb + "/title/title.dat");

        idx_mc_cn_off.open(gendb + "/_idx/movie_companies__company_id__offsets.bin");
        idx_mc_cn_rowids.open(gendb + "/_idx/movie_companies__company_id__rowids.bin");
        idx_mk_mv_off.open(gendb + "/_idx/movie_keyword__movie_id__offsets.bin");
        idx_ml_mv_off.open(gendb + "/_idx/movie_link__movie_id__offsets.bin");
    }

    // ----- resolve dims -----
    int16_t pl_code = 0;
    int32_t ct_id = 0;
    int32_t k_id = 0;
    // LT set
    std::vector<int32_t> LT_ids;
    std::vector<std::pair<uint32_t,uint32_t>> LT_off_len; // (off, len) into lt_link_dat

    {
        GENDB_PHASE("resolve_dims");
        // pl_code: scan country_code dict
        size_t K = cn_cc_dict_off.count - 1;
        const char* pl = "[pl]";
        size_t pl_len = 4;
        for (size_t i = 0; i < K; ++i) {
            uint64_t lo = cn_cc_dict_off.data[i];
            uint64_t hi = cn_cc_dict_off.data[i+1];
            if (hi - lo == pl_len && std::memcmp(cn_cc_dict_dat.data + lo, pl, pl_len) == 0) {
                // dict code is 1-based: code 0 = NULL, code i+1 references dict entry i
                pl_code = (int16_t)(i + 1);
                break;
            }
        }

        // ct_id: scan company_type kind
        size_t ctn = ct_kind_off.count - 1;
        const char* prod = "production companies";
        size_t prod_len = 20;
        for (size_t i = 0; i < ctn; ++i) {
            uint64_t lo = ct_kind_off.data[i];
            uint64_t hi = ct_kind_off.data[i+1];
            if (hi - lo == prod_len && std::memcmp(ct_kind_dat.data + lo, prod, prod_len) == 0) {
                ct_id = (int32_t)(i + 1);
                break;
            }
        }

        // k_id: scan keyword for "sequel"
        size_t kn = kw_off.count - 1;
        const char* kseq = "sequel";
        size_t kseq_len = 6;
        for (size_t i = 0; i < kn; ++i) {
            uint64_t lo = kw_off.data[i];
            uint64_t hi = kw_off.data[i+1];
            if (hi - lo == kseq_len && std::memcmp(kw_dat.data + lo, kseq, kseq_len) == 0) {
                k_id = (int32_t)(i + 1);
                break;
            }
        }

        // LT: link_type.link LIKE '%follow%'
        size_t ltn = lt_link_off.count - 1;
        const char* follow = "follow";
        size_t follow_len = 6;
        for (size_t i = 0; i < ltn; ++i) {
            uint64_t lo = lt_link_off.data[i];
            uint64_t hi = lt_link_off.data[i+1];
            uint32_t l = (uint32_t)(hi - lo);
            if (l < follow_len) continue;
            if (memmem(lt_link_dat.data + lo, l, follow, follow_len) != nullptr) {
                LT_ids.push_back((int32_t)(i + 1));
                LT_off_len.push_back({(uint32_t)lo, l});
            }
        }
    }

    if (ct_id == 0 || k_id == 0 || LT_ids.empty()) {
        // no rows
        FILE* fp = std::fopen((results + "/Q11a.csv").c_str(), "w");
        std::fprintf(fp, "from_company,movie_link_type,non_polish_sequel_movie\n");
        std::fclose(fp);
        return 0;
    }

    // Quick lookup: for a link_type_id, is it in LT? And get offset/len.
    // link_type_id values are 1..18. Build small lookup.
    int32_t lt_max = 0;
    for (auto v : LT_ids) if (v > lt_max) lt_max = v;
    std::vector<int8_t> lt_in_set(lt_max + 1, 0);
    std::vector<std::pair<uint32_t,uint32_t>> lt_str(lt_max + 1, {0u, 0u});
    for (size_t i = 0; i < LT_ids.size(); ++i) {
        lt_in_set[LT_ids[i]] = 1;
        lt_str[LT_ids[i]] = LT_off_len[i];
    }

    // ----- filter company_name -----
    std::vector<int32_t> cn_ids;
    {
        GENDB_PHASE("filter_company_name");
        size_t N = cn_cc.count;
        cn_ids.reserve(4096);
        const char* fpat = "Film";
        const char* wpat = "Warner";
        for (size_t i = 0; i < N; ++i) {
            int16_t cc = cn_cc.data[i];
            if (cc == 0 || cc == pl_code) continue;
            uint64_t lo = cn_name_off.data[i];
            uint64_t hi = cn_name_off.data[i+1];
            uint32_t l = (uint32_t)(hi - lo);
            const uint8_t* p = cn_name_dat.data + lo;
            bool match = false;
            if (l >= 4 && memmem(p, l, fpat, 4) != nullptr) match = true;
            else if (l >= 6 && memmem(p, l, wpat, 6) != nullptr) match = true;
            if (match) cn_ids.push_back((int32_t)(i + 1));
        }
    }

    // ----- main scan: parallel over cn_ids -----
    int num_threads = std::min<int>(12, (int)std::thread::hardware_concurrency());
    if (num_threads <= 0) num_threads = 1;
    if ((int)cn_ids.size() < num_threads) num_threads = std::max(1, (int)cn_ids.size());

    struct LocalMin {
        MinStr cn_name;
        MinStr lt_link;
        MinStr t_title;
    };
    std::vector<LocalMin> locals(num_threads);

    {
        GENDB_PHASE("main_scan");
        std::atomic<size_t> next_idx{0};
        const size_t CHUNK = 64;

        auto worker = [&](int tid) {
            LocalMin& M = locals[tid];
            while (true) {
                size_t start = next_idx.fetch_add(CHUNK, std::memory_order_relaxed);
                if (start >= cn_ids.size()) break;
                size_t end = std::min(start + CHUNK, cn_ids.size());
                for (size_t ci = start; ci < end; ++ci) {
                    int32_t cn_id = cn_ids[ci];
                    int32_t mc_lo = idx_mc_cn_off.data[cn_id];
                    int32_t mc_hi = idx_mc_cn_off.data[cn_id + 1];
                    if (mc_lo == mc_hi) continue;

                    // cn.name view
                    uint64_t nlo = cn_name_off.data[cn_id - 1];
                    uint64_t nhi = cn_name_off.data[cn_id];
                    const uint8_t* cn_name_p = cn_name_dat.data + nlo;
                    uint32_t cn_name_l = (uint32_t)(nhi - nlo);

                    for (int32_t k = mc_lo; k < mc_hi; ++k) {
                        int32_t mc_row = idx_mc_cn_rowids.data[k];
                        if (mc_company_type_id.data[mc_row] != ct_id) continue;
                        // note IS NULL
                        if (mc_note_off.data[mc_row] != mc_note_off.data[mc_row + 1]) continue;
                        int32_t mid = mc_movie_id.data[mc_row];
                        if (mid <= 0) continue;
                        // production_year filter
                        int32_t py = t_py.data[mid - 1];
                        if (py == INT32_MIN) continue;
                        if (py < 1950 || py > 2000) continue;

                        // semi-join movie_keyword
                        int32_t mk_lo = idx_mk_mv_off.data[mid];
                        int32_t mk_hi = idx_mk_mv_off.data[mid + 1];
                        bool has_kw = false;
                        for (int32_t kk = mk_lo; kk < mk_hi; ++kk) {
                            if (mk_keyword_id.data[kk] == k_id) { has_kw = true; break; }
                        }
                        if (!has_kw) continue;

                        // semi-join movie_link, capture matching link
                        int32_t ml_lo = idx_ml_mv_off.data[mid];
                        int32_t ml_hi = idx_ml_mv_off.data[mid + 1];
                        bool any_link = false;
                        for (int32_t kk = ml_lo; kk < ml_hi; ++kk) {
                            int32_t lt_id = ml_link_type_id.data[kk];
                            if (lt_id <= 0 || lt_id > lt_max) continue;
                            if (!lt_in_set[lt_id]) continue;
                            // update lt.link min
                            auto& s = lt_str[lt_id];
                            M.lt_link.update(lt_link_dat.data + s.first, s.second);
                            any_link = true;
                        }
                        if (!any_link) continue;

                        // update mins
                        M.cn_name.update(cn_name_p, cn_name_l);

                        // title view
                        uint64_t tlo = t_title_off.data[mid - 1];
                        uint64_t thi = t_title_off.data[mid];
                        M.t_title.update(t_title_dat.data + tlo, (uint32_t)(thi - tlo));
                    }
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) threads.emplace_back(worker, t);
        for (auto& th : threads) th.join();
    }

    // merge
    MinStr cn_min, lt_min, t_min;
    for (auto& L : locals) {
        cn_min.merge(L.cn_name);
        lt_min.merge(L.lt_link);
        t_min.merge(L.t_title);
    }

    // ----- output -----
    {
        GENDB_PHASE("output");
        std::string outpath = results + "/Q11a.csv";
        FILE* fp = std::fopen(outpath.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", outpath.c_str()); return 2; }
        std::fprintf(fp, "from_company,movie_link_type,non_polish_sequel_movie\n");
        if (cn_min.ptr) {
            std::fwrite(cn_min.ptr, 1, cn_min.len, fp);
            std::fputc(',', fp);
            std::fwrite(lt_min.ptr, 1, lt_min.len, fp);
            std::fputc(',', fp);
            std::fwrite(t_min.ptr, 1, t_min.len, fp);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
