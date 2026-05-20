// Q11d - generated
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
#include <unordered_set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace gendb;

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

    MmapColumn<int32_t> mc_movie_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int32_t> mc_company_type_id;
    MmapColumn<int64_t> mc_note_off;
    MmapColumn<uint8_t> mc_note_dat;

    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> mk_keyword_id;

    MmapColumn<int32_t> ml_movie_id;

    MmapColumn<int32_t> t_py;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<uint8_t> t_title_dat;

    // indexes
    MmapColumn<int32_t> idx_mk_kw_off;
    MmapColumn<int32_t> idx_mk_kw_rowids;
    MmapColumn<int32_t> idx_mc_mv_off;

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

        mc_movie_id.open(gendb + "/movie_companies/movie_id.bin");
        mc_company_id.open(gendb + "/movie_companies/company_id.bin");
        mc_company_type_id.open(gendb + "/movie_companies/company_type_id.bin");
        mc_note_off.open(gendb + "/movie_companies/note.off");
        mc_note_dat.open(gendb + "/movie_companies/note.dat");

        mk_movie_id.open(gendb + "/movie_keyword/movie_id.bin");
        mk_keyword_id.open(gendb + "/movie_keyword/keyword_id.bin");

        ml_movie_id.open(gendb + "/movie_link/movie_id.bin");

        t_py.open(gendb + "/title/production_year.bin");
        t_title_off.open(gendb + "/title/title.off");
        t_title_dat.open(gendb + "/title/title.dat");

        idx_mk_kw_off.open(gendb + "/_idx/movie_keyword__keyword_id__offsets.bin");
        idx_mk_kw_rowids.open(gendb + "/_idx/movie_keyword__keyword_id__rowids.bin");
        idx_mc_mv_off.open(gendb + "/_idx/movie_companies__movie_id__offsets.bin");
    }

    // ----- resolve dims -----
    // Dict convention: empty -> code 0 (NULL). Real codes assigned 1..K.
    // dict.off has K+1 entries (or K with K+1 offsets); entry at dict index i
    // corresponds to code (i+1). Find "[pl]" → pl_code = i+1.
    int16_t pl_code = 0;
    int32_t pc_id = 0;
    std::vector<int32_t> K_kids; // keyword ids for sequel/revenge/based-on-novel

    {
        GENDB_PHASE("resolve_dims");
        // pl_code
        size_t K = cn_cc_dict_off.count - 1;
        const char* pl = "[pl]";
        size_t pl_len = 4;
        for (size_t i = 0; i < K; ++i) {
            uint64_t lo = cn_cc_dict_off.data[i];
            uint64_t hi = cn_cc_dict_off.data[i+1];
            if (hi - lo == pl_len && std::memcmp(cn_cc_dict_dat.data + lo, pl, pl_len) == 0) {
                pl_code = (int16_t)(i + 1);
                break;
            }
        }

        // pc_id: company_type sorted by id (1..N), kind[i] corresponds to ct.id = i+1
        size_t ctn = ct_kind_off.count - 1;
        const char* prod = "production companies";
        size_t prod_len = 20;
        for (size_t i = 0; i < ctn; ++i) {
            uint64_t lo = ct_kind_off.data[i];
            uint64_t hi = ct_kind_off.data[i+1];
            if (hi - lo == prod_len && std::memcmp(ct_kind_dat.data + lo, prod, prod_len) == 0) {
                pc_id = (int32_t)(i + 1);
                break;
            }
        }

        // keyword ids
        size_t kn = kw_off.count - 1;
        struct Needle { const char* s; size_t l; };
        Needle targets[3] = {
            {"sequel", 6},
            {"revenge", 7},
            {"based-on-novel", 14}
        };
        for (size_t i = 0; i < kn; ++i) {
            uint64_t lo = kw_off.data[i];
            uint64_t hi = kw_off.data[i+1];
            uint32_t l = (uint32_t)(hi - lo);
            for (int t = 0; t < 3; ++t) {
                if (l == targets[t].l && std::memcmp(kw_dat.data + lo, targets[t].s, l) == 0) {
                    K_kids.push_back((int32_t)(i + 1));
                    break;
                }
            }
            if (K_kids.size() >= 3) break;
        }
    }

    if (pc_id == 0 || K_kids.empty()) {
        FILE* fp = std::fopen((results + "/Q11d.csv").c_str(), "w");
        std::fprintf(fp, "from_company,production_note,movie_based_on_book\n");
        std::fclose(fp);
        return 0;
    }

    // ----- build K_movies bitmap via CSR walk over movie_keyword keyword index -----
    // Use bitmap over [1..max_title_id] for O(1) probe (title.id is 1..2528312)
    size_t num_titles = t_py.count;
    std::vector<uint8_t> in_K_movies(num_titles + 2, 0);

    {
        GENDB_PHASE("build_K_movies");
        for (int32_t kid : K_kids) {
            int32_t lo = idx_mk_kw_off.data[kid];
            int32_t hi = idx_mk_kw_off.data[kid + 1];
            for (int32_t i = lo; i < hi; ++i) {
                int32_t mk_row = idx_mk_kw_rowids.data[i];
                int32_t mid = mk_movie_id.data[mk_row];
                if (mid > 0 && (size_t)mid <= num_titles) {
                    in_K_movies[mid] = 1;
                }
            }
        }
    }

    // ----- scan movie_link to collect candidate mids (in K_movies) -----
    std::vector<int32_t> cand_mids;
    {
        GENDB_PHASE("scan_movie_link");
        size_t mln = ml_movie_id.count;
        cand_mids.reserve(8192);
        // de-dup on the fly using a visited bitmap
        std::vector<uint8_t> seen(num_titles + 2, 0);
        for (size_t i = 0; i < mln; ++i) {
            int32_t mid = ml_movie_id.data[i];
            if (mid <= 0 || (size_t)mid > num_titles) continue;
            if (!in_K_movies[mid]) continue;
            if (seen[mid]) continue;
            seen[mid] = 1;
            // production_year filter at scan time to shrink candidate list
            int32_t py = t_py.data[mid - 1];
            if (py == INT32_MIN) continue;
            if (py <= 1950) continue;
            cand_mids.push_back(mid);
        }
    }

    // ----- main scan: parallel over candidate mids -----
    int num_threads = std::min<int>(12, (int)std::thread::hardware_concurrency());
    if (num_threads <= 0) num_threads = 1;
    if ((int)cand_mids.size() < num_threads) num_threads = std::max(1, (int)cand_mids.size());

    struct LocalMin {
        MinStr cn_name;
        MinStr mc_note;
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
                if (start >= cand_mids.size()) break;
                size_t end = std::min(start + CHUNK, cand_mids.size());
                for (size_t ci = start; ci < end; ++ci) {
                    int32_t mid = cand_mids[ci];

                    // title view
                    uint64_t tlo = t_title_off.data[mid - 1];
                    uint64_t thi = t_title_off.data[mid];
                    const uint8_t* t_p = t_title_dat.data + tlo;
                    uint32_t t_l = (uint32_t)(thi - tlo);

                    // iterate mc rows for this movie. Offsets index has
                    // (max_title_id + 2) entries. Row range for mid = i is
                    // [offsets[i], offsets[i+1]).
                    int32_t mc_lo = idx_mc_mv_off.data[mid];
                    int32_t mc_hi = idx_mc_mv_off.data[mid + 1];

                    for (int32_t k = mc_lo; k < mc_hi; ++k) {
                        // note IS NOT NULL → off[k+1] > off[k]
                        uint64_t nlo = mc_note_off.data[k];
                        uint64_t nhi = mc_note_off.data[k + 1];
                        if (nhi == nlo) continue;
                        int32_t ct_id = mc_company_type_id.data[k];
                        if (ct_id == 0 || ct_id == pc_id) continue;
                        int32_t cid = mc_company_id.data[k];
                        if (cid <= 0) continue;
                        int16_t cc = cn_cc.data[cid - 1];
                        if (cc == 0 || cc == pl_code) continue;

                        // cn.name view
                        uint64_t clo = cn_name_off.data[cid - 1];
                        uint64_t chi = cn_name_off.data[cid];
                        M.cn_name.update(cn_name_dat.data + clo, (uint32_t)(chi - clo));
                        M.mc_note.update(mc_note_dat.data + nlo, (uint32_t)(nhi - nlo));
                        M.t_title.update(t_p, t_l);
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
    MinStr cn_min, mc_min, t_min;
    for (auto& L : locals) {
        cn_min.merge(L.cn_name);
        mc_min.merge(L.mc_note);
        t_min.merge(L.t_title);
    }

    // ----- output -----
    {
        GENDB_PHASE("output");
        std::string outpath = results + "/Q11d.csv";
        FILE* fp = std::fopen(outpath.c_str(), "w");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", outpath.c_str()); return 2; }
        std::fprintf(fp, "from_company,production_note,movie_based_on_book\n");
        if (cn_min.ptr) {
            // CSV-quote fields containing comma or quote
            auto write_csv = [&](const uint8_t* p, uint32_t l) {
                bool needs = false;
                for (uint32_t i = 0; i < l; ++i) {
                    if (p[i] == ',' || p[i] == '"' || p[i] == '\n' || p[i] == '\r') { needs = true; break; }
                }
                if (!needs) {
                    std::fwrite(p, 1, l, fp);
                } else {
                    std::fputc('"', fp);
                    for (uint32_t i = 0; i < l; ++i) {
                        if (p[i] == '"') std::fputc('"', fp);
                        std::fputc(p[i], fp);
                    }
                    std::fputc('"', fp);
                }
            };
            write_csv(cn_min.ptr, cn_min.len);
            std::fputc(',', fp);
            write_csv(mc_min.ptr, mc_min.len);
            std::fputc(',', fp);
            write_csv(t_min.ptr, t_min.len);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
