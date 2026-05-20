// Q8a — MIN(an1.name), MIN(t.title) actress/japanese dub
#include "timing_utils.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

struct Mmap {
    void* p = nullptr;
    size_t sz = 0;
    ~Mmap() { if (p) munmap(p, sz); }
};

static void map_file(const std::string& path, Mmap& m) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(path.c_str()); std::exit(1); }
    struct stat st{};
    if (fstat(fd, &st) != 0) { perror("fstat"); std::exit(1); }
    m.sz = (size_t)st.st_size;
    if (m.sz == 0) { m.p = nullptr; close(fd); return; }
    m.p = mmap(nullptr, m.sz, PROT_READ, MAP_SHARED, fd, 0);
    if (m.p == MAP_FAILED) { perror("mmap"); std::exit(1); }
    close(fd);
    madvise(m.p, m.sz, MADV_WILLNEED);
}

static std::string read_file_str(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(path.c_str()); std::exit(1); }
    struct stat st{};
    fstat(fd, &st);
    std::string s;
    s.resize(st.st_size);
    ssize_t n = read(fd, s.data(), st.st_size);
    (void)n;
    close(fd);
    return s;
}

template<typename T>
static std::vector<T> read_vec(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(path.c_str()); std::exit(1); }
    struct stat st{};
    fstat(fd, &st);
    size_t n = st.st_size / sizeof(T);
    std::vector<T> v(n);
    ssize_t got = read(fd, v.data(), st.st_size);
    (void)got;
    close(fd);
    return v;
}

static inline bool contains(const char* hay, size_t hl, const char* needle, size_t nl) {
    if (nl > hl) return false;
    return memmem(hay, hl, needle, nl) != nullptr;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <gendb_dir> <results_dir>\n", argv[0]);
        return 1;
    }
    std::string store = argv[1];
    std::string results_dir = argv[2];
    fs::create_directories(results_dir);

    std::string best_an_name;
    std::string best_title;

    {
        GENDB_PHASE("total");

        // ---- Data loading ----
        Mmap m_cipid_off, m_cipid_row, m_ci_role, m_ci_movie, m_ci_pid;
        Mmap m_ci_note_off, m_ci_note_dat;
        Mmap m_akpid_off, m_an_name_off, m_an_name_dat;
        Mmap m_mcmid_off, m_mc_company, m_mc_note_off, m_mc_note_dat;
        Mmap m_cn_cc;
        Mmap m_name_off, m_name_dat;
        Mmap m_title_off, m_title_dat;

        int32_t rt_id_actress = 0;
        int16_t jp_code = 0;

        {
            GENDB_PHASE("data_loading");

            // role_type.role
            auto rt_off = read_vec<int64_t>(store + "/role_type/role.off");
            std::string rt_dat = read_file_str(store + "/role_type/role.dat");
            for (size_t i = 0; i + 1 < rt_off.size(); ++i) {
                std::string_view s(rt_dat.data() + rt_off[i], rt_off[i+1] - rt_off[i]);
                if (s == "actress") { rt_id_actress = (int32_t)(i + 1); break; }
            }

            // company_name.country_code dict
            auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
            std::string cc_dat = read_file_str(store + "/company_name/country_code.dict.dat");
            for (size_t i = 0; i + 1 < cc_off.size(); ++i) {
                std::string_view s(cc_dat.data() + cc_off[i], cc_off[i+1] - cc_off[i]);
                if (s == "[jp]") { jp_code = (int16_t)(i + 1); break; }
            }

            map_file(store + "/company_name/country_code.bin", m_cn_cc);

            map_file(store + "/_idx/cast_info__person_id__offsets.bin", m_cipid_off);
            map_file(store + "/_idx/cast_info__person_id__rowids.bin", m_cipid_row);
            map_file(store + "/cast_info/role_id.bin", m_ci_role);
            map_file(store + "/cast_info/movie_id.bin", m_ci_movie);
            map_file(store + "/cast_info/person_id.bin", m_ci_pid);
            map_file(store + "/cast_info/note.off", m_ci_note_off);
            map_file(store + "/cast_info/note.dat", m_ci_note_dat);

            map_file(store + "/_idx/aka_name__person_id__offsets.bin", m_akpid_off);
            map_file(store + "/aka_name/name.off", m_an_name_off);
            map_file(store + "/aka_name/name.dat", m_an_name_dat);

            map_file(store + "/_idx/movie_companies__movie_id__offsets.bin", m_mcmid_off);
            map_file(store + "/movie_companies/company_id.bin", m_mc_company);
            map_file(store + "/movie_companies/note.off", m_mc_note_off);
            map_file(store + "/movie_companies/note.dat", m_mc_note_dat);

            map_file(store + "/name/name.off", m_name_off);
            map_file(store + "/name/name.dat", m_name_dat);

            map_file(store + "/title/title.off", m_title_off);
            map_file(store + "/title/title.dat", m_title_dat);
        }

        if (rt_id_actress == 0) {
            std::fprintf(stderr, "actress role_id not found\n");
            return 2;
        }
        if (jp_code == 0) {
            std::fprintf(stderr, "[jp] country_code not found\n");
            return 2;
        }

        const int16_t* cn_cc = (const int16_t*)m_cn_cc.p;
        const size_t cn_n = m_cn_cc.sz / sizeof(int16_t);  // 234997

        const int32_t* cipid_off = (const int32_t*)m_cipid_off.p;
        const int32_t* cipid_row = (const int32_t*)m_cipid_row.p;
        const int32_t* ci_role = (const int32_t*)m_ci_role.p;
        const int32_t* ci_movie = (const int32_t*)m_ci_movie.p;
        const int64_t* ci_note_off = (const int64_t*)m_ci_note_off.p;
        const char* ci_note_dat = (const char*)m_ci_note_dat.p;

        const int32_t* akpid_off = (const int32_t*)m_akpid_off.p;
        const int64_t* an_name_off = (const int64_t*)m_an_name_off.p;
        const char* an_name_dat = (const char*)m_an_name_dat.p;

        const int32_t* mcmid_off = (const int32_t*)m_mcmid_off.p;
        const int32_t* mc_company = (const int32_t*)m_mc_company.p;
        const int64_t* mc_note_off = (const int64_t*)m_mc_note_off.p;
        const char* mc_note_dat = (const char*)m_mc_note_dat.p;

        const int64_t* name_off = (const int64_t*)m_name_off.p;
        const char* name_dat = (const char*)m_name_dat.p;
        const size_t name_n = (m_name_off.sz / sizeof(int64_t)) - 1;

        const int64_t* title_off = (const int64_t*)m_title_off.p;
        const char* title_dat = (const char*)m_title_dat.p;

        // ---- Build cn_jp bitset ----
        std::vector<uint8_t> cn_jp(cn_n, 0);
        {
            GENDB_PHASE("build_cn_jp_bitset");
            for (size_t i = 0; i < cn_n; ++i) {
                if (cn_cc[i] == jp_code) cn_jp[i] = 1;
            }
        }

        // ---- Scan name for %Yo% AND NOT %Yu% ----
        std::vector<int32_t> pids;
        {
            GENDB_PHASE("scan_name");
            unsigned T = std::max(1u, std::thread::hardware_concurrency());
            std::vector<std::vector<int32_t>> per_thread(T);
            std::vector<std::thread> th;
            size_t chunk = (name_n + T - 1) / T;
            for (unsigned t = 0; t < T; ++t) {
                size_t a = t * chunk;
                size_t b = std::min(a + chunk, name_n);
                th.emplace_back([&, t, a, b]() {
                    auto& out = per_thread[t];
                    out.reserve((b - a) / 200);
                    for (size_t i = a; i < b; ++i) {
                        int64_t s = name_off[i], e = name_off[i+1];
                        size_t len = (size_t)(e - s);
                        if (len < 2) continue;
                        const char* p = name_dat + s;
                        if (!memmem(p, len, "Yo", 2)) continue;
                        if (memmem(p, len, "Yu", 2)) continue;
                        out.push_back((int32_t)(i + 1));
                    }
                });
            }
            for (auto& x : th) x.join();
            size_t total = 0;
            for (auto& v : per_thread) total += v.size();
            pids.reserve(total);
            for (auto& v : per_thread) pids.insert(pids.end(), v.begin(), v.end());
        }

        std::fprintf(stderr, "candidate pids: %zu\n", pids.size());

        // ---- Main scan: for each pid, walk CSR, filter; for each surviving ci, walk mc, etc. ----
        const std::string_view voice_lit = "(voice: English version)";
        const size_t voice_len = voice_lit.size();
        const char* voice_data = voice_lit.data();

        const char* japan_needle = "(Japan)";
        const size_t japan_len = 7;
        const char* usa_needle = "(USA)";
        const size_t usa_len = 5;

        struct Result {
            std::string an_name;
            std::string title;
        };

        {
            GENDB_PHASE("main_scan");
            unsigned T = std::max(1u, std::thread::hardware_concurrency());
            std::vector<Result> per_thread(T);
            std::vector<std::thread> th;
            std::atomic<size_t> next_idx{0};
            const size_t MORSEL = 256;
            size_t N = pids.size();

            auto better = [](const std::string& cur, std::string_view cand) -> bool {
                if (cur.empty()) return true;
                return std::string_view(cur) > cand;
            };

            for (unsigned t = 0; t < T; ++t) {
                th.emplace_back([&, t]() {
                    Result& res = per_thread[t];
                    while (true) {
                        size_t start = next_idx.fetch_add(MORSEL);
                        if (start >= N) break;
                        size_t end = std::min(start + MORSEL, N);
                        for (size_t k = start; k < end; ++k) {
                            int32_t pid = pids[k];
                            // semi-join with aka_name: range must be non-empty
                            int32_t alo = akpid_off[pid];
                            int32_t ahi = akpid_off[pid + 1];
                            if (alo >= ahi) continue;

                            int32_t lo = cipid_off[pid];
                            int32_t hi = cipid_off[pid + 1];
                            for (int32_t kk = lo; kk < hi; ++kk) {
                                int32_t r = cipid_row[kk];
                                if (ci_role[r] != rt_id_actress) continue;
                                int64_t na = ci_note_off[r], nb = ci_note_off[r+1];
                                if ((nb - na) != (int64_t)voice_len) continue;
                                if (memcmp(ci_note_dat + na, voice_data, voice_len) != 0) continue;

                                int32_t mv = ci_movie[r];
                                if (mv <= 0) continue;
                                // walk mc by movie_id
                                int32_t mlo = mcmid_off[mv];
                                int32_t mhi = mcmid_off[mv + 1];
                                bool any_pass = false;
                                for (int32_t mr = mlo; mr < mhi; ++mr) {
                                    int64_t mna = mc_note_off[mr], mnb = mc_note_off[mr + 1];
                                    size_t mnl = (size_t)(mnb - mna);
                                    if (mnl < japan_len) continue;
                                    const char* mnp = mc_note_dat + mna;
                                    if (!memmem(mnp, mnl, japan_needle, japan_len)) continue;
                                    if (mnl >= usa_len && memmem(mnp, mnl, usa_needle, usa_len)) continue;
                                    int32_t cid = mc_company[mr];
                                    if (cid <= 0 || (size_t)cid > cn_n) continue;
                                    if (!cn_jp[cid - 1]) continue;
                                    any_pass = true;
                                    break;
                                }
                                if (!any_pass) continue;

                                // Update MIN(t.title)
                                int64_t ta = title_off[mv - 1], tb = title_off[mv];
                                std::string_view tv(title_dat + ta, tb - ta);
                                if (better(res.title, tv)) {
                                    res.title.assign(tv.data(), tv.size());
                                }

                                // For each aka_name row in pid's range, update MIN
                                for (int32_t ar = alo; ar < ahi; ++ar) {
                                    int64_t aa = an_name_off[ar], ab = an_name_off[ar + 1];
                                    std::string_view av(an_name_dat + aa, ab - aa);
                                    if (better(res.an_name, av)) {
                                        res.an_name.assign(av.data(), av.size());
                                    }
                                }
                            }
                        }
                    }
                });
            }
            for (auto& x : th) x.join();

            // Final reduction
            for (auto& r : per_thread) {
                if (!r.an_name.empty()) {
                    if (best_an_name.empty() || best_an_name > r.an_name)
                        best_an_name = r.an_name;
                }
                if (!r.title.empty()) {
                    if (best_title.empty() || best_title > r.title)
                        best_title = r.title;
                }
            }
        }

        // ---- Output ----
        {
            GENDB_PHASE("output");
            std::string out_path = results_dir + "/Q8a.csv";
            FILE* f = fopen(out_path.c_str(), "w");
            if (!f) { perror("fopen output"); return 3; }
            fprintf(f, "actress_pseudonym,japanese_movie_dubbed\n");

            auto csv_quote = [](const std::string& s) -> std::string {
                bool need = false;
                for (char c : s) {
                    if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
                }
                if (!need) return s;
                std::string o;
                o.reserve(s.size() + 2);
                o.push_back('"');
                for (char c : s) {
                    if (c == '"') o.push_back('"');
                    o.push_back(c);
                }
                o.push_back('"');
                return o;
            };

            fprintf(f, "%s,%s\n", csv_quote(best_an_name).c_str(), csv_quote(best_title).c_str());
            fclose(f);
        }
    }

    return 0;
}
