#define _GNU_SOURCE
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main(int argc, char* argv[]){
    std::string store=argv[1];
    MmapColumn<int64_t> it_off(store+"/info_type/info.off");
    MmapColumn<char>    it_dat(store+"/info_type/info.dat");
    MmapColumn<int32_t> it_id(store+"/info_type/id.bin");
    int32_t it1=-1;
    for(size_t i=0;i<it_off.size()-1;++i){int64_t a=it_off[i],b=it_off[i+1];if(b-a==13&&memcmp(it_dat.data+a,"release dates",13)==0){it1=it_id[i];}}
    MmapColumn<int32_t> csr_off(store+"/_idx/movie_info__info_type_id__offsets.bin");
    MmapColumn<int32_t> csr_row(store+"/_idx/movie_info__info_type_id__rowids.bin");
    int32_t lo=csr_off[it1],hi=csr_off[it1+1];
    MmapColumn<int32_t> mi_mid(store+"/movie_info/movie_id.bin");
    MmapColumn<int64_t> mi_io(store+"/movie_info/info.off");
    MmapColumn<char>    mi_id(store+"/movie_info/info.dat");
    MmapColumn<int64_t> mi_no(store+"/movie_info/note.off");
    MmapColumn<char>    mi_nd(store+"/movie_info/note.dat");
    MmapColumn<int32_t> t_year(store+"/title/production_year.bin");
    MmapColumn<int64_t> t_to(store+"/title/title.off");
    MmapColumn<char>    t_td(store+"/title/title.dat");
    MmapColumn<int16_t> cn_cc(store+"/company_name/country_code.bin");
    MmapColumn<int64_t> cn_do(store+"/company_name/country_code.dict.off");
    MmapColumn<char>    cn_dd(store+"/company_name/country_code.dict.dat");
    int16_t us=-1;
    for(size_t i=0;i<cn_do.size()-1;++i){int64_t a=cn_do[i],b=cn_do[i+1];if(b-a==4&&memcmp(cn_dd.data+a,"[us]",4)==0)us=(int16_t)i;}
    MmapColumn<int32_t> mc_off(store+"/_idx/movie_companies__movie_id__offsets.bin");
    MmapColumn<int32_t> mc_cid(store+"/movie_companies/company_id.bin");
    MmapColumn<int32_t> at_off(store+"/_idx/aka_title__movie_id__offsets.bin");
    MmapColumn<int32_t> mk_off(store+"/_idx/movie_keyword__movie_id__offsets.bin");

    // Find movie_id of "24: Day Six - Debrief"
    int32_t target_mv = -1;
    for (size_t i=0; i<t_to.size()-1; ++i) {
        size_t l = t_to[i+1]-t_to[i];
        if (l==21 && memcmp(t_td.data+t_to[i], "24: Day Six - Debrief", 21)==0) {
            target_mv = (int32_t)(i+1);
            printf("Found '24: Day Six - Debrief' at movie_id=%d year=%d\n", target_mv, t_year[i]);
        }
    }
    
    // Check this movie's mi rows for matching info
    if (target_mv >= 0) {
        // Scan all mi rows where movie_id==target_mv
        // mi is sorted by movie_id, find via mi_movie_id__offsets
        MmapColumn<int32_t> mi_off(store+"/_idx/movie_info__movie_id__offsets.bin");
        int32_t a=mi_off[target_mv], b=mi_off[target_mv+1];
        printf("mi rows for mv=%d: [%d, %d)\n", target_mv, a, b);
        for (int32_t r=a; r<b; ++r) {
            int64_t ia=mi_io[r],ib=mi_io[r+1]; size_t il=ib-ia;
            std::string info(mi_id.data+ia, il);
            int64_t na=mi_no[r],nb=mi_no[r+1]; size_t nl=nb-na;
            std::string note(mi_nd.data+na, nl);
            printf("  mi[%d]: info='%s' note='%s'\n", r, info.c_str(), note.c_str());
        }
        // mc rows
        int32_t mca=mc_off[target_mv],mcb=mc_off[target_mv+1];
        printf("mc rows: [%d,%d)\n", mca, mcb);
        for(int32_t j=mca;j<mcb;++j){
            int32_t cid=mc_cid[j];
            printf("  mc[%d]: company_id=%d country_code=%d\n", j, cid, cn_cc[cid-1]);
        }
        printf("at exists: %d, mk exists: %d\n", at_off[target_mv+1]>at_off[target_mv], mk_off[target_mv+1]>mk_off[target_mv]);
    }

    // Now scan all survivors
    int cnt=0;
    std::string best_info="\xff", best_title="\xff";
    for(int32_t k=lo;k<hi;++k){
        int32_t r=csr_row[k];
        int64_t na=mi_no[r],nb=mi_no[r+1]; size_t nl=nb-na;
        if(nl<8||!memmem(mi_nd.data+na,nl,"internet",8))continue;
        int64_t ia=mi_io[r],ib=mi_io[r+1]; size_t il=ib-ia;
        if(il<8)continue;
        const char* ip=mi_id.data+ia;
        if(memcmp(ip,"USA:",4)!=0)continue;
        if(!memmem(ip+4,il-4," 199",4)&&!memmem(ip+4,il-4," 200",4))continue;
        int32_t mv=mi_mid[r];
        int32_t py=t_year[mv-1];
        if(py==INT32_MIN||py<=1990)continue;
        int32_t ma=mc_off[mv],mb=mc_off[mv+1]; bool mc_ok=false;
        for(int32_t j=ma;j<mb;++j){int32_t cid=mc_cid[j];if(cid>0&&cn_cc[cid-1]==us){mc_ok=true;break;}}
        if(!mc_ok)continue;
        if(at_off[mv+1]<=at_off[mv])continue;
        if(mk_off[mv+1]<=mk_off[mv])continue;
        cnt++;
        std::string ci(ip,il), ct(t_td.data+t_to[mv-1],(size_t)(t_to[mv]-t_to[mv-1]));
        if(ci<best_info)best_info=ci;
        if(ct<best_title)best_title=ct;
        if(mv==target_mv)printf("TARGET SURVIVES: mi[%d] info='%s'\n", r, ci.c_str());
    }
    printf("survivors=%d MIN_info='%s' MIN_title='%s'\n", cnt, best_info.c_str(), best_title.c_str());
    return 0;
}
