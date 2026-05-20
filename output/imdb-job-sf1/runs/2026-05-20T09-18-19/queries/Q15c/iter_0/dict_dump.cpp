#include <cstdio>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main(int argc, char* argv[]){
    std::string store=argv[1];
    MmapColumn<int64_t> cn_do(store+"/company_name/country_code.dict.off");
    MmapColumn<char>    cn_dd(store+"/company_name/country_code.dict.dat");
    printf("dict entries=%zu\n", cn_do.size()-1);
    for(size_t i=0;i<cn_do.size()-1;++i){
        int64_t a=cn_do[i],b=cn_do[i+1]; size_t l=b-a;
        std::string s(cn_dd.data+a, l);
        printf("[%zu] '%s'\n", i, s.c_str());
        if (i>15) break;
    }
    return 0;
}
