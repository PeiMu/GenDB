#include <cstdio>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main(int argc, char* argv[]){
    std::string store=argv[1];
    MmapColumn<int16_t> cn_cc(store+"/company_name/country_code.bin");
    MmapColumn<int64_t> cn_no(store+"/company_name/name.off");
    MmapColumn<char>    cn_nd(store+"/company_name/name.dat");
    MmapColumn<int64_t> cn_do(store+"/company_name/country_code.dict.off");
    MmapColumn<char>    cn_dd(store+"/company_name/country_code.dict.dat");
    MmapColumn<int32_t> cn_id(store+"/company_name/id.bin");
    // company_name is dense PK; id 327 -> row 326
    int row=326;
    printf("cn[326]: id=%d cc=%d (cc_str='%s') name='%.*s'\n",
        cn_id[row], cn_cc[row],
        std::string(cn_dd.data+cn_do[cn_cc[row]], cn_do[cn_cc[row]+1]-cn_do[cn_cc[row]]).c_str(),
        (int)(cn_no[row+1]-cn_no[row]), cn_nd.data+cn_no[row]);
    // Also try cn id=327
    int row2=327;
    printf("cn[327]: id=%d cc=%d (cc_str='%s') name='%.*s'\n",
        cn_id[row2], cn_cc[row2],
        std::string(cn_dd.data+cn_do[cn_cc[row2]], cn_do[cn_cc[row2]+1]-cn_do[cn_cc[row2]]).c_str(),
        (int)(cn_no[row2+1]-cn_no[row2]), cn_nd.data+cn_no[row2]);
    return 0;
}
