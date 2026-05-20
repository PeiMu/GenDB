#define _GNU_SOURCE
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <string>
#include <vector>
#include "mmap_utils.h"
using gendb::MmapColumn;
int main() {
    std::string g = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    const int32_t V = 1710108;
    
    // Look up dims first
    MmapColumn<int32_t> rt_id_col(g + "/role_type/id.bin");
    MmapColumn<int64_t> rt_off(g + "/role_type/role.off");
    MmapColumn<char> rt_dat(g + "/role_type/role.dat");
    int32_t rt_id = INT32_MIN;
    for (size_t i = 0; i < rt_id_col.count; i++) {
        size_t l = rt_off[i+1] - rt_off[i];
        if (l == 7 && memcmp(rt_dat.data + rt_off[i], "actress", 7) == 0) rt_id = rt_id_col[i];
    }
    printf("rt_id (actress) = %d\n", rt_id);
    
    MmapColumn<int32_t> it_id_col(g + "/info_type/id.bin");
    MmapColumn<int64_t> it_off(g + "/info_type/info.off");
    MmapColumn<char> it_dat(g + "/info_type/info.dat");
    int32_t it_id = INT32_MIN;
    for (size_t i = 0; i < it_id_col.count; i++) {
        size_t l = it_off[i+1] - it_off[i];
        if (l == 13 && memcmp(it_dat.data + it_off[i], "release dates", 13) == 0) it_id = it_id_col[i];
    }
    printf("it_id (release dates) = %d\n", it_id);
    
    MmapColumn<int32_t> kw_id_col(g + "/keyword/id.bin");
    MmapColumn<int64_t> kw_off(g + "/keyword/keyword.off");
    MmapColumn<char> kw_dat(g + "/keyword/keyword.dat");
    const char* tgt[3] = {"hero", "martial-arts", "hand-to-hand-combat"};
    size_t tlen[3] = {4, 12, 19};
    int32_t kw_arr[3] = {0,0,0};
    int kn = 0;
    for (size_t i = 0; i < kw_id_col.count; i++) {
        size_t l = kw_off[i+1] - kw_off[i];
        const char* p = kw_dat.data + kw_off[i];
        for (int t = 0; t < 3; t++) {
            if (l == tlen[t] && memcmp(p, tgt[t], l) == 0) {
                kw_arr[kn++] = kw_id_col[i];
                printf("kw '%s' = %d\n", tgt[t], kw_id_col[i]);
            }
        }
    }
    
    // mk for V
    MmapColumn<int32_t> mk_off(g + "/_idx/movie_keyword__movie_id__offsets.bin");
    MmapColumn<int32_t> mk_kid(g + "/movie_keyword/keyword_id.bin");
    int32_t lo = mk_off[V], hi = mk_off[V+1];
    printf("mk range [%d,%d) count=%d\n", lo, hi, hi-lo);
    for (int32_t i = lo; i < hi; i++) {
        printf("  kid=%d\n", mk_kid[i]);
    }
    
    // mc for V
    MmapColumn<int32_t> mc_off(g + "/_idx/movie_companies__movie_id__offsets.bin");
    MmapColumn<int32_t> mc_cid(g + "/movie_companies/company_id.bin");
    MmapColumn<int16_t> cn_cc(g + "/company_name/country_code.bin");
    
    MmapColumn<int64_t> cd_off(g + "/company_name/country_code.dict.off");
    MmapColumn<char> cd_dat(g + "/company_name/country_code.dict.dat");
    int16_t us_code = -1;
    for (size_t i = 0; i + 1 < cd_off.count; i++) {
        size_t l = cd_off[i+1] - cd_off[i];
        if (l == 4 && memcmp(cd_dat.data + cd_off[i], "[us]", 4) == 0) us_code = (int16_t)i;
    }
    printf("us_code=%d\n", us_code);
    
    lo = mc_off[V]; hi = mc_off[V+1];
    printf("mc range [%d,%d) count=%d\n", lo, hi, hi-lo);
    for (int32_t i = lo; i < hi; i++) {
        int32_t cid = mc_cid[i];
        printf("  cid=%d cc=%d\n", cid, cn_cc[cid-1]);
    }
    
    // mi for V
    MmapColumn<int32_t> mi_off(g + "/_idx/movie_info__movie_id__offsets.bin");
    MmapColumn<int32_t> mi_itid(g + "/movie_info/info_type_id.bin");
    MmapColumn<int64_t> mi_io_off(g + "/movie_info/info.off");
    MmapColumn<char> mi_io_dat(g + "/movie_info/info.dat");
    lo = mi_off[V]; hi = mi_off[V+1];
    printf("mi range [%d,%d) count=%d\n", lo, hi, hi-lo);
    for (int32_t i = lo; i < hi; i++) {
        int64_t lo2 = mi_io_off[i], hi2 = mi_io_off[i+1];
        std::string s(mi_io_dat.data + lo2, hi2-lo2);
        printf("  it=%d info='%s'\n", mi_itid[i], s.c_str());
    }
    
    // ci for V
    MmapColumn<int32_t> ci_off(g + "/_idx/cast_info__movie_id__offsets.bin");
    MmapColumn<int32_t> ci_role(g + "/cast_info/role_id.bin");
    MmapColumn<int32_t> ci_pid(g + "/cast_info/person_id.bin");
    MmapColumn<int32_t> ci_prid(g + "/cast_info/person_role_id.bin");
    MmapColumn<int64_t> ci_n_off(g + "/cast_info/note.off");
    MmapColumn<char> ci_n_dat(g + "/cast_info/note.dat");
    lo = ci_off[V]; hi = ci_off[V+1];
    printf("ci range [%d,%d) count=%d\n", lo, hi, hi-lo);
    
    MmapColumn<int64_t> n_off(g + "/name/name.off");
    MmapColumn<char> n_dat(g + "/name/name.dat");
    MmapColumn<int8_t> gender(g + "/name/gender.bin");
    MmapColumn<int64_t> gd_off(g + "/name/gender.dict.off");
    MmapColumn<char> gd_dat(g + "/name/gender.dict.dat");
    int8_t f_code = -1;
    for (size_t i = 0; i + 1 < gd_off.count; i++) {
        size_t l = gd_off[i+1] - gd_off[i];
        if (l == 1 && gd_dat.data[gd_off[i]] == 'f') f_code = (int8_t)i;
    }
    printf("f_code=%d\n", (int)f_code);
    
    MmapColumn<int32_t> aka_off(g + "/_idx/aka_name__person_id__offsets.bin");
    
    int n_role_actress = 0;
    int n_voice_note = 0;
    int n_female_an = 0;
    int n_aka_exist = 0;
    int n_prid_ok = 0;
    
    for (int32_t r = lo; r < hi; r++) {
        int32_t role = ci_role[r];
        if (role != rt_id) continue;
        n_role_actress++;
        int32_t prid = ci_prid[r];
        if (prid == INT32_MIN || prid < 1) continue;
        n_prid_ok++;
        int32_t pid = ci_pid[r];
        // Check note
        int64_t nlo = ci_n_off[r], nhi = ci_n_off[r+1];
        std::string note(ci_n_dat.data + nlo, nhi - nlo);
        // Check name
        int64_t nmlo = n_off[pid-1], nmhi = n_off[pid];
        std::string pname(n_dat.data + nmlo, nmhi - nmlo);
        int8_t g_code = gender[pid-1];
        int32_t aka_lo = aka_off[pid], aka_hi = aka_off[pid+1];
        printf("  ci r=%d pid=%d prid=%d name='%s' g=%d note='%s' aka=[%d,%d)\n",
               r, pid, prid, pname.c_str(), (int)g_code, note.c_str(), aka_lo, aka_hi);
        if (note == "(voice)" || note == "(voice: Japanese version)" || note == "(voice) (uncredited)" || note == "(voice: English version)") n_voice_note++;
        if (g_code == f_code && pname.find("An") != std::string::npos) n_female_an++;
        if (aka_lo < aka_hi) n_aka_exist++;
    }
    printf("role=actress: %d, prid!=MIN: %d, voice note: %d, female+An: %d, aka exists: %d\n",
           n_role_actress, n_prid_ok, n_voice_note, n_female_an, n_aka_exist);
    
    return 0;
}
