// Q8c: MIN(a1.name), MIN(t.title) over writer cast_info join with US movie_companies.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "timing_utils.h"
#include "mmap_utils.h"

using gendb::MmapColumn;

static std::string read_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::perror(path.c_str()); std::exit(1); }
    struct stat st; fstat(fd, &st);
    std::string s; s.resize(st.st_size);
    ssize_t need = st.st_size, got = 0;
    while (got < need) {
        ssize_t n = ::read(fd, s.data() + got, need - got);
        if (n <= 0) break;
        got += n;
    }
    ::close(fd);
    return s;
}

static bool ensure_dir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return true;
    return mkdir(path.c_str(), 0755) == 0;
}

static void csv_escape(std::string& out, std::string_view s) {
    bool needs = false;
    for (char c : s) { if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs = true; break; } }
    if (!needs) { out.append(s.data(), s.size()); return; }
    out.push_back('"');
    for (char c : s) {
        if (c == '"') { out.push_back('"'); out.push_back('"'); }
        else out.push_back(c);
    }
    out.push_back('"');
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    GENDB_PHASE("total");
    std::string store = argv[1];
    std::string results_dir = argv[2];
    ensure_dir(results_dir);

    // ---------- Data loading ----------
    // Columns / index files we need
    MmapColumn<int64_t> rt_off_col;
    MmapColumn<int64_t> cc_dict_off_col;
    MmapColumn<int16_t> cn_cc_col;
    MmapColumn<int32_t> ci_role_id;
    MmapColumn<int32_t> ci_movie_id;
    MmapColumn<int32_t> ci_person_id;
    MmapColumn<int32_t> mc_company_id;
    MmapColumn<int32_t> crid_off_col;
    MmapColumn<int32_t> crid_row_col;
    MmapColumn<int32_t> mcmid_off_col;
    MmapColumn<int32_t> akpid_off_col;
    MmapColumn<int64_t> title_off_col;
    MmapColumn<int64_t> aka_name_off_col;

    std::string rt_dat_s, cc_dat_s, title_dat_s, aka_name_dat_s;

    int16_t us_code = 0;
    int32_t rt_id_writer = 0;

    {
        GENDB_PHASE("data_loading");

        rt_off_col.open(store + "/role_type/role.off");
        rt_dat_s = read_file(store + "/role_type/role.dat");

        cc_dict_off_col.open(store + "/company_name/country_code.dict.off");
        cc_dat_s = read_file(store + "/company_name/country_code.dict.dat");
        cn_cc_col.open(store + "/company_name/country_code.bin");

        ci_role_id.open(store + "/cast_info/role_id.bin");
        ci_movie_id.open(store + "/cast_info/movie_id.bin");
        ci_person_id.open(store + "/cast_info/person_id.bin");

        mc_company_id.open(store + "/movie_companies/company_id.bin");

        crid_off_col.open(store + "/_idx/cast_info__role_id__offsets.bin");
        crid_row_col.open(store + "/_idx/cast_info__role_id__rowids.bin");
        mcmid_off_col.open(store + "/_idx/movie_companies__movie_id__offsets.bin");
        akpid_off_col.open(store + "/_idx/aka_name__person_id__offsets.bin");

        title_off_col.open(store + "/title/title.off");
        title_dat_s = read_file(store + "/title/title.dat");
        aka_name_off_col.open(store + "/aka_name/name.off");
        aka_name_dat_s = read_file(store + "/aka_name/name.dat");

        // We'll use random access on ci columns (driven by CSR rowids)
        ci_movie_id.advise_random();
        ci_person_id.advise_random();
        // mc_company_id accessed via mc range lookup
        mc_company_id.advise_random();

        // Resolve rt_id_writer
        const int64_t* rt_off = rt_off_col.data;
        size_t rt_n = rt_off_col.count;
        for (size_t i = 0; i + 1 < rt_n; ++i) {
            std::string_view sv(rt_dat_s.data() + rt_off[i], rt_off[i + 1] - rt_off[i]);
            if (sv == "writer") { rt_id_writer = (int32_t)(i + 1); break; }
        }

        // Resolve us_code
        const int64_t* cc_off = cc_dict_off_col.data;
        size_t cc_n = cc_dict_off_col.count;
        for (size_t i = 0; i + 1 < cc_n; ++i) {
            std::string_view sv(cc_dat_s.data() + cc_off[i], cc_off[i + 1] - cc_off[i]);
            if (sv == "[us]") { us_code = (int16_t)(i + 1); break; }
        }
    }

    if (rt_id_writer == 0 || us_code == 0) {
        std::fprintf(stderr, "Failed to resolve rt_id_writer or us_code\n");
        return 1;
    }

    // ---------- Build is_us_company bitset ----------
    size_t cn_n = cn_cc_col.count;
    std::vector<uint8_t> is_us;
    {
        GENDB_PHASE("build_is_us_company_bitset");
        is_us.assign((cn_n + 7) / 8, 0);
        const int16_t* cc = cn_cc_col.data;
        for (size_t i = 0; i < cn_n; ++i) {
            if (cc[i] == us_code) {
                is_us[i >> 3] |= (uint8_t)(1u << (i & 7));
            }
        }
    }
    auto check_us = [&](int32_t company_id) -> bool {
        // company_id is 1-based dense PK
        uint32_t idx = (uint32_t)(company_id - 1);
        if (idx >= (uint32_t)cn_n) return false;
        return (is_us[idx >> 3] >> (idx & 7)) & 1u;
    };

    // ---------- Main scan ----------
    const int32_t* crid_off = crid_off_col.data;
    const int32_t* crid_row = crid_row_col.data;
    int32_t lo = crid_off[rt_id_writer];
    int32_t hi = crid_off[rt_id_writer + 1];
    int32_t total_writers = hi - lo;

    const int32_t* ci_pid = ci_person_id.data;
    const int32_t* ci_mid = ci_movie_id.data;
    const int32_t* mcmid_off = mcmid_off_col.data;
    const int32_t* akpid_off = akpid_off_col.data;
    const int32_t* mc_cid = mc_company_id.data;

    size_t title_n = title_off_col.count > 0 ? title_off_col.count - 1 : 0;
    size_t akn_n = aka_name_off_col.count > 0 ? aka_name_off_col.count - 1 : 0;
    (void)akn_n;
    size_t akpid_n = akpid_off_col.count > 0 ? akpid_off_col.count - 1 : 0;
    size_t mcmid_n = mcmid_off_col.count > 0 ? mcmid_off_col.count - 1 : 0;

    const int64_t* title_off = title_off_col.data;
    const char* title_dat = title_dat_s.data();
    const int64_t* akn_off = aka_name_off_col.data;
    const char* akn_dat = aka_name_dat_s.data();

    auto title_sv = [&](size_t row) -> std::string_view {
        int64_t s = title_off[row], e = title_off[row + 1];
        return std::string_view(title_dat + s, e - s);
    };
    auto akn_sv = [&](size_t row) -> std::string_view {
        int64_t s = akn_off[row], e = akn_off[row + 1];
        return std::string_view(akn_dat + s, e - s);
    };

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    if ((int32_t)hw > total_writers) hw = std::max(1, total_writers);

    std::vector<int64_t> best_title_row(hw, -1);
    std::vector<int64_t> best_akn_row(hw, -1);

    {
        GENDB_PHASE("main_scan");
        std::vector<std::thread> threads;
        threads.reserve(hw);
        int32_t chunk = (total_writers + (int32_t)hw - 1) / (int32_t)hw;
        for (unsigned t = 0; t < hw; ++t) {
            int32_t s = lo + (int32_t)t * chunk;
            int32_t e = std::min(s + chunk, hi);
            if (s >= e) continue;
            threads.emplace_back([&, t, s, e]() {
                int64_t local_title = -1;
                int64_t local_akn = -1;
                std::string_view local_title_sv;
                std::string_view local_akn_sv;

                for (int32_t k = s; k < e; ++k) {
                    int32_t r = crid_row[k];
                    int32_t pid = ci_pid[r];
                    int32_t mv = ci_mid[r];

                    // aka_name existence on person_id
                    if ((uint32_t)pid >= (uint32_t)akpid_n) continue;
                    int32_t alo = akpid_off[pid];
                    int32_t ahi = akpid_off[pid + 1];
                    if (ahi <= alo) continue;

                    // movie_companies US existence on movie_id
                    if (mv <= 0 || (uint32_t)mv >= (uint32_t)mcmid_n) continue;
                    int32_t mlo = mcmid_off[mv];
                    int32_t mhi = mcmid_off[mv + 1];
                    bool us_ok = false;
                    for (int32_t mi = mlo; mi < mhi; ++mi) {
                        if (check_us(mc_cid[mi])) { us_ok = true; break; }
                    }
                    if (!us_ok) continue;

                    // Update MIN(title) at title row mv-1
                    if (mv >= 1 && (size_t)(mv - 1) < title_n) {
                        std::string_view ts = title_sv(mv - 1);
                        if (local_title < 0 || ts < local_title_sv) {
                            local_title = mv - 1;
                            local_title_sv = ts;
                        }
                    }

                    // Update MIN(name) over aka_name range
                    for (int32_t ai = alo; ai < ahi; ++ai) {
                        std::string_view ns = akn_sv((size_t)ai);
                        if (local_akn < 0 || ns < local_akn_sv) {
                            local_akn = ai;
                            local_akn_sv = ns;
                        }
                    }
                }
                best_title_row[t] = local_title;
                best_akn_row[t] = local_akn;
            });
        }
        for (auto& th : threads) th.join();
    }

    // ---------- Reduce ----------
    int64_t final_title = -1;
    int64_t final_akn = -1;
    std::string_view final_title_sv;
    std::string_view final_akn_sv;
    for (unsigned t = 0; t < hw; ++t) {
        if (best_title_row[t] >= 0) {
            std::string_view ts = title_sv((size_t)best_title_row[t]);
            if (final_title < 0 || ts < final_title_sv) {
                final_title = best_title_row[t];
                final_title_sv = ts;
            }
        }
        if (best_akn_row[t] >= 0) {
            std::string_view ns = akn_sv((size_t)best_akn_row[t]);
            if (final_akn < 0 || ns < final_akn_sv) {
                final_akn = best_akn_row[t];
                final_akn_sv = ns;
            }
        }
    }

    // ---------- Output ----------
    {
        GENDB_PHASE("output");
        std::string out_path = results_dir + "/Q8c.csv";
        FILE* fp = std::fopen(out_path.c_str(), "wb");
        if (!fp) { std::perror(out_path.c_str()); return 1; }
        std::string buf;
        buf.reserve(256);
        buf.append("writer_pseudo_name,movie_title\n");
        if (final_akn >= 0) csv_escape(buf, final_akn_sv);
        buf.push_back(',');
        if (final_title >= 0) csv_escape(buf, final_title_sv);
        buf.push_back('\n');
        std::fwrite(buf.data(), 1, buf.size(), fp);
        std::fclose(fp);
    }

    return 0;
}
