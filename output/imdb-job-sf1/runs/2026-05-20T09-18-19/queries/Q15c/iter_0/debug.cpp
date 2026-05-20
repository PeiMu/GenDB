#define _GNU_SOURCE
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include "mmap_utils.h"
using namespace gendb;
int main(int argc, char* argv[]) {
    std::string store = argv[1];
    MmapColumn<int64_t> it_off(store + "/info_type/info.off");
    MmapColumn<char>    it_dat(store + "/info_type/info.dat");
    MmapColumn<int32_t> it_id(store + "/info_type/id.bin");
    int32_t it1 = -1;
    size_t n_it = it_off.size() - 1;
    for (size_t i = 0; i < n_it; ++i) {
        int64_t a = it_off[i], b = it_off[i+1];
        size_t l = (size_t)(b-a);
        if (l==13 && std::memcmp(it_dat.data+a,"release dates",13)==0) {
            it1 = it_id[i];
            printf("release dates: row=%zu id=%d\n", i, it1);
        }
    }
    MmapColumn<int32_t> csr_off(store + "/_idx/movie_info__info_type_id__offsets.bin");
    MmapColumn<int32_t> csr_row(store + "/_idx/movie_info__info_type_id__rowids.bin");
    int32_t lo = csr_off[it1], hi = csr_off[it1+1];
    printf("csr range: [%d, %d) = %d rows\n", lo, hi, hi-lo);

    MmapColumn<int32_t> mi_mid(store + "/movie_info/movie_id.bin");
    MmapColumn<int64_t> mi_io(store + "/movie_info/info.off");
    MmapColumn<char>    mi_id(store + "/movie_info/info.dat");
    MmapColumn<int64_t> mi_no(store + "/movie_info/note.off");
    MmapColumn<char>    mi_nd(store + "/movie_info/note.dat");
    MmapColumn<int32_t> t_year(store + "/title/production_year.bin");
    MmapColumn<int64_t> t_to(store + "/title/title.off");
    MmapColumn<char>    t_td(store + "/title/title.dat");
    MmapColumn<int16_t> cn_cc(store + "/company_name/country_code.bin");
    MmapColumn<int64_t> cn_do(store + "/company_name/country_code.dict.off");
    MmapColumn<char>    cn_dd(store + "/company_name/country_code.dict.dat");
    int16_t us=-1;
    for (size_t i=0;i<cn_do.size()-1;++i){
        int64_t a=cn_do[i],b=cn_do[i+1]; size_t l=(size_t)(b-a);
        if (l==4 && std::memcmp(cn_dd.data+a,"[us]",4)==0){ us=(int16_t)i; printf("us_code=%d\n",us); }
    }
    MmapColumn<int32_t> mc_off(store + "/_idx/movie_companies__movie_id__offsets.bin");
    MmapColumn<int32_t> mc_cid(store + "/movie_companies/company_id.bin");
    MmapColumn<int32_t> at_off(store + "/_idx/aka_title__movie_id__offsets.bin");
    MmapColumn<int32_t> mk_off(store + "/_idx/movie_keyword__movie_id__offsets.bin");

    // count survivors
    int cnt=0;
    std::string best_info, best_title;
    bool have=false;
    for (int32_t k=lo;k<hi;++k){
        int32_t r=csr_row[k];
        int64_t na=mi_no[r],nb=mi_no[r+1]; size_t nl=(size_t)(nb-na);
        if (nl<8) continue;
        if (memmem(mi_nd.data+na,nl,"internet",8)==nullptr) continue;
        int64_t ia=mi_io[r],ib=mi_io[r+1]; size_t il=(size_t)(ib-ia);
        if (il<8) continue;
        const char* ip=mi_id.data+ia;
        if (std::memcmp(ip,"USA:",4)!=0) continue;
        if (!memmem(ip+4,il-4," 199",4) && !memmem(ip+4,il-4," 200",4)) continue;
        int32_t mv=mi_mid[r];
        if (mv<=0) continue;
        int32_t py=t_year[mv-1];
        if (py==INT32_MIN || py<=1990) continue;
        int32_t ma=mc_off[mv+1]; bool mc_ok=false;
        for (int32_t j=ma;j<mb;++j){ int32_t cid=mc_cid[j]; if (cid>0 && cn_cc[cid-1]==us){mc_ok=true;break;} }
        if (!mc_ok) continue;
        if (at_off[mv+1]<=at_off[mv]) continue;
        if (mk_off[mv+1]<=mk_off[mv]) continue;
        std::string cur_i(ip, il);
        std::string cur_t(t_td.data+t_to[mv-1], (size_t)(t_to[mv]-t_to[mv-1]));
        cnt++;
        if (!have || cur_i<best_info){ best_info=cur_i; }
        if (!have || cur_t<best_title){ best_title=cur_t; }
        have=true;
        if (cur_t.size()<=3) printf("SHORT title: mv=%d title='%s' info='%s' year=%d\n", mv, cur_t.c_str(), cur_i.c_str(), py);
    }
    printf("survivors=%d\n", cnt);
    printf("MIN info='%s' title='%s'\n", best_info.c_str(), best_title.c_str());
    return 0;
}
