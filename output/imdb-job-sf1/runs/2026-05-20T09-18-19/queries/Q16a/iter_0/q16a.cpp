// Q16a — character-name-in-title series with episode_nr in [50,100), US company
// SELECT MIN(an.name), MIN(t.title)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "timing_utils.h"
#include "mmap_utils.h"

using namespace gendb;

static void csv_write_field(FILE* fp, std::string_view s) {
    bool need_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { need_quote = true; break; }
    }
    if (!need_quote) {
        std::fwrite(s.data(), 1, s.size(), fp);
        return;
    }
    std::fputc('"', fp);
    for (char c : s) {
        if (c == '"') std::fputc('"', fp);
        std::fputc(c, fp);
    }
    std::fputc('"', fp);
}

int main(int argc, char** argv) {
    GENDB_PHASE("total");

    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results = argv[2];
    std::filesystem::create_directories(results);

    // ---------- mmap ----------
    MmapColumn<int16_t> cn_cc;
    MmapColumn<int64_t> cn_dict_off;
    MmapColumn<char>    cn_dict_dat;

    MmapColumn<int64_t> kw_off;
    MmapColumn<char>    kw_dat;

    MmapColumn<int32_t> mk_movie_id;
    MmapColumn<int32_t> mk_csr_off;
    MmapColumn<int32_t> mk_csr_rowids;

    MmapColumn<int32_t> t_ep;
    MmapColumn<int64_t> t_title_off;
    MmapColumn<char>    t_title_dat;

    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int32_t> mc_mv_off;

    MmapColumn<int32_t> ci_person_id;
    MmapColumn<int32_t> ci_mv_off;

    MmapColumn<int32_t> an_pid_off;
    MmapColumn<int64_t> an_name_off;
    MmapColumn<char>    an_name_dat;

    {
        GENDB_PHASE("data_loading");
        cn_cc.open(store + "/company_name/country_code.bin");
        cn_dict_off.open(store + "/company_name/country_code.dict.off");
        cn_dict_dat.open(store + "/company_name/country_code.dict.dat");

        kw_off.open(store + "/keyword/keyword.off");
        kw_dat.open(store + "/keyword/keyword.dat");

        mk_movie_id.open(store + "/movie_keyword/movie_id.bin");
        mk_csr_off.open(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
        mk_csr_rowids.open(store + "/_idx/movie_keyword__keyword_id__rowids.bin");

        t_ep.open(store + "/title/episode_nr.bin");
        t_title_off.open(store + "/title/title.off");
        t_title_dat.open(store + "/title/title.dat");

        mc_company_id.open(store + "/movie_companies/company_id.bin");
        mc_mv_off.open(store + "/_idx/movie_companies__movie_id__offsets.bin");

        ci_person_id.open(store + "/cast_info/person_id.bin");
        ci_mv_off.open(store + "/_idx/cast_info__movie_id__offsets.bin");

        an_pid_off.open(store + "/_idx/aka_name__person_id__offsets.bin");
        an_name_off.open(store + "/aka_name/name.off");
        an_name_dat.open(store + "/aka_name/name.dat");

        // access pattern hints
        mc_company_id.advise_random();
        ci_person_id.advise_random();
        an_name_off.advise_random();
        an_name_dat.advise_random();
        t_title_off.advise_random();
        t_title_dat.advise_random();
        t_ep.advise_random();
        mk_csr_rowids.advise_random();
        mk_movie_id.advise_random();
    }

    // ---------- resolve us_code in dict ----------
    int16_t us_code = 0;
    {
        GENDB_PHASE("resolve_us_code");
        size_t n = cn_dict_off.count;
        static const char US[] = "[us]";
        size_t L = sizeof(US) - 1;
        for (size_t i = 0; i + 1 < n; ++i) {
            int64_t lo = cn_dict_off[i];
            int64_t hi = cn_dict_off[i + 1];
            size_t sl = (size_t)(hi - lo);
            if (sl == L && std::memcmp(cn_dict_dat.data + lo, US, L) == 0) {
                us_code = (int16_t)(i + 1);
                break;
            }
        }
        if (us_code == 0) {
            std::fprintf(stderr, "Could not resolve '[us]'\n");
            return 2;
        }
    }

    // ---------- resolve k_id ----------
    int32_t k_id = 0;
    {
        GENDB_PHASE("resolve_k_id");
        size_t n = kw_off.count;
        static const char K[] = "character-name-in-title";
        size_t L = sizeof(K) - 1;
        for (size_t i = 0; i + 1 < n; ++i) {
            int64_t lo = kw_off[i];
            int64_t hi = kw_off[i + 1];
            size_t sl = (size_t)(hi - lo);
            if (sl == L && std::memcmp(kw_dat.data + lo, K, L) == 0) {
                k_id = (int32_t)(i + 1);
                break;
            }
        }
        if (k_id == 0) {
            std::fprintf(stderr, "Could not resolve k_id\n");
            return 3;
        }
    }

    // ---------- build cn_us bitset (over company_name.id 1..234997) ----------
    const size_t CN_ROWS = cn_cc.count;  // 234997
    std::vector<uint8_t> cn_us(CN_ROWS + 2, 0);
    {
        GENDB_PHASE("build_cn_us_bitset");
        const int16_t* cc = cn_cc.data;
        for (size_t i = 0; i < CN_ROWS; ++i) {
            if (cc[i] == us_code) cn_us[i + 1] = 1;
        }
    }

    // ---------- enumerate movie_keyword via CSR; dedup mvs; filter title.episode_nr ----------
    const size_t TITLE_ROWS = t_ep.count; // 2528312
    std::vector<uint8_t> mv_seen(TITLE_ROWS + 2, 0);
    std::vector<int32_t> mv_after_title;
    mv_after_title.reserve(512);

    {
        GENDB_PHASE("main_scan");
        int32_t lo = mk_csr_off[k_id];
        int32_t hi = mk_csr_off[k_id + 1];
        const int32_t* rowids = mk_csr_rowids.data;
        const int32_t* mkmv = mk_movie_id.data;
        const int32_t* ep = t_ep.data;
        for (int32_t p = lo; p < hi; ++p) {
            int32_t r = rowids[p];
            int32_t mv = mkmv[r];
            if (mv <= 0 || (size_t)mv > TITLE_ROWS) continue;
            if (mv_seen[mv]) continue;
            mv_seen[mv] = 1;
            int32_t epv = ep[mv - 1];
            if (epv == INT32_MIN) continue;
            if (epv < 50 || epv >= 100) continue;
            mv_after_title.push_back(mv);
        }
    }

    // ---------- aggregation ----------
    std::string_view min_an_name;
    std::string_view min_t_title;
    bool has_an = false, has_title = false;

    {
        GENDB_PHASE("probe_and_aggregate");
        const int32_t* mc_off = mc_mv_off.data;
        const int32_t* mc_cid = mc_company_id.data;
        const int32_t* ci_off = ci_mv_off.data;
        const int32_t* ci_pid = ci_person_id.data;
        const int32_t* an_off = an_pid_off.data;
        const int64_t* an_noff = an_name_off.data;
        const char*    an_ndat = an_name_dat.data;
        const int64_t* t_toff = t_title_off.data;
        const char*    t_tdat = t_title_dat.data;
        const size_t   AN_PID_OFF_N = an_pid_off.count; // 4167493 ⇒ pid up to 4167491

        for (int32_t mv : mv_after_title) {
            // semi-join: any mc row with cn_us[company_id]?
            int32_t mlo = mc_off[mv];
            int32_t mhi = mc_off[mv + 1];
            bool us_match = false;
            for (int32_t r = mlo; r < mhi; ++r) {
                int32_t cid = mc_cid[r];
                if (cid > 0 && (size_t)cid <= CN_ROWS && cn_us[cid]) {
                    us_match = true;
                    break;
                }
            }
            if (!us_match) continue;

            // t.title for this mv (constant across this mv's output rows)
            int64_t to = t_toff[mv - 1];
            int64_t te = t_toff[mv];
            std::string_view t_title_sv(t_tdat + to, (size_t)(te - to));

            // enumerate cast_info rows for this mv
            int32_t clo = ci_off[mv];
            int32_t chi = ci_off[mv + 1];
            for (int32_t r = clo; r < chi; ++r) {
                int32_t pid = ci_pid[r];
                if (pid <= 0) continue;
                if ((size_t)pid + 1 >= AN_PID_OFF_N) continue;
                int32_t alo = an_off[pid];
                int32_t ahi = an_off[pid + 1];
                if (alo == ahi) continue; // no aka_name for this person → contributes nothing
                // contributes at least one output row → update MIN(t.title)
                if (!has_title || t_title_sv < min_t_title) {
                    min_t_title = t_title_sv;
                    has_title = true;
                }
                for (int32_t s = alo; s < ahi; ++s) {
                    int64_t no = an_noff[s];
                    int64_t ne = an_noff[s + 1];
                    std::string_view nm(an_ndat + no, (size_t)(ne - no));
                    if (!has_an || nm < min_an_name) {
                        min_an_name = nm;
                        has_an = true;
                    }
                }
            }
        }
    }

    // ---------- output ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results + "/Q16a.csv";
        FILE* fp = std::fopen(out_path.c_str(), "wb");
        if (!fp) {
            std::fprintf(stderr, "Cannot open %s\n", out_path.c_str());
            return 4;
        }
        std::fputs("cool_actor_pseudonym,series_named_after_char\n", fp);
        if (has_an || has_title) {
            if (has_an) csv_write_field(fp, min_an_name);
            std::fputc(',', fp);
            if (has_title) csv_write_field(fp, min_t_title);
            std::fputc('\n', fp);
        }
        std::fclose(fp);
    }

    return 0;
}
