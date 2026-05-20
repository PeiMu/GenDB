#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "mmap_utils.h"
using namespace gendb;
int main() {
    std::string sdir = "/home/pei/Project/GenDB/output/imdb-job-sf1/storage";
    int32_t mv = 1638426;

    // resolve it1, it2
    MmapColumn<char> it_dat(sdir + "/info_type/info.dat");
    MmapColumn<char> it_off_r(sdir + "/info_type/info.off");
    MmapColumn<int32_t> it_id(sdir + "/info_type/id.bin");
    const int64_t* it_off = (int64_t*)it_off_r.data;
    int it1=-1, it2=-1;
    for (size_t i = 0; i < it_id.count; i++) {
        size_t l = it_off[i+1]-it_off[i];
        if (l==6 && std::memcmp(it_dat.data+it_off[i], "genres",6)==0) it1=it_id.data[i];
        if (l==5 && std::memcmp(it_dat.data+it_off[i], "votes",5)==0) it2=it_id.data[i];
    }
    std::printf("it1=%d it2=%d\n", it1, it2);

    // keyword ids
    MmapColumn<char> kw_dat(sdir+"/keyword/keyword.dat");
    MmapColumn<char> kw_off_r(sdir+"/keyword/keyword.off");
    MmapColumn<int32_t> kw_id(sdir+"/keyword/id.bin");
    const int64_t* kw_off = (int64_t*)kw_off_r.data;
    const char* kw[] = {"murder","violence","blood","gore","death","female-nudity","hospital"};
    size_t kwl[] = {6,8,5,4,5,13,8};
    for (size_t i = 0; i < kw_id.count; i++) {
        size_t l = kw_off[i+1]-kw_off[i];
        const char* s = kw_dat.data+kw_off[i];
        for (int j = 0; j < 7; j++) {
            if (l==kwl[j] && std::memcmp(s, kw[j], l)==0) {
                std::printf("k '%s' -> id=%d\n", kw[j], kw_id.data[i]);
            }
        }
    }

    // mk for movie 1638426
    MmapColumn<int32_t> mk_movie(sdir+"/movie_keyword/movie_id.bin");
    MmapColumn<int32_t> mk_kid(sdir+"/movie_keyword/keyword_id.bin");
    MmapColumn<int32_t> mk_off(sdir+"/_idx/movie_keyword__movie_id__offsets.bin");
    int lo = mk_off.data[mv], hi = mk_off.data[mv+1];
    std::printf("mk for mv=%d: rows [%d,%d)\n", mv, lo, hi);
    for (int r = lo; r < hi; r++) std::printf("  mk row=%d movie=%d kid=%d\n", r, mk_movie.data[r], mk_kid.data[r]);

    // mi for movie 1638426 with it1
    MmapColumn<int32_t> mi_movie(sdir+"/movie_info/movie_id.bin");
    MmapColumn<int32_t> mi_itid(sdir+"/movie_info/info_type_id.bin");
    MmapColumn<int32_t> mi_off(sdir+"/_idx/movie_info__movie_id__offsets.bin");
    MmapColumn<char> mi_dat(sdir+"/movie_info/info.dat");
    MmapColumn<char> mi_off_r(sdir+"/movie_info/info.off");
    const int64_t* mi_ioff = (int64_t*)mi_off_r.data;
    int lo2 = mi_off.data[mv], hi2 = mi_off.data[mv+1];
    std::printf("mi for mv=%d: rows [%d,%d)\n", mv, lo2, hi2);
    for (int r = lo2; r < hi2; r++) {
        size_t l = mi_ioff[r+1]-mi_ioff[r];
        std::printf("  mi row=%d movie=%d itid=%d info=%.*s\n", r, mi_movie.data[r], mi_itid.data[r], (int)l, mi_dat.data+mi_ioff[r]);
    }

    // mi_idx
    MmapColumn<int32_t> mix_movie(sdir+"/movie_info_idx/movie_id.bin");
    MmapColumn<int32_t> mix_itid(sdir+"/movie_info_idx/info_type_id.bin");
    MmapColumn<int32_t> mix_off(sdir+"/_idx/movie_info_idx__movie_id__offsets.bin");
    MmapColumn<char> mix_dat(sdir+"/movie_info_idx/info.dat");
    MmapColumn<char> mix_off_r(sdir+"/movie_info_idx/info.off");
    const int64_t* mix_ioff = (int64_t*)mix_off_r.data;
    int lo3 = mix_off.data[mv], hi3 = mix_off.data[mv+1];
    std::printf("mi_idx for mv=%d: rows [%d,%d)\n", mv, lo3, hi3);
    for (int r = lo3; r < hi3; r++) {
        size_t l = mix_ioff[r+1]-mix_ioff[r];
        std::printf("  mix row=%d movie=%d itid=%d info=%.*s\n", r, mix_movie.data[r], mix_itid.data[r], (int)l, mix_dat.data+mix_ioff[r]);
    }

    // cast_info
    MmapColumn<int32_t> ci_movie(sdir+"/cast_info/movie_id.bin");
    MmapColumn<int32_t> ci_pid(sdir+"/cast_info/person_id.bin");
    MmapColumn<int32_t> ci_off(sdir+"/_idx/cast_info__movie_id__offsets.bin");
    MmapColumn<char> ci_note_d(sdir+"/cast_info/note.dat");
    MmapColumn<char> ci_note_r(sdir+"/cast_info/note.off");
    const int64_t* ci_noff = (int64_t*)ci_note_r.data;
    int lo4 = ci_off.data[mv], hi4 = ci_off.data[mv+1];
    std::printf("ci for mv=%d: rows [%d,%d) count=%d\n", mv, lo4, hi4, hi4-lo4);
    int writer_count = 0;
    for (int r = lo4; r < hi4; r++) {
        size_t l = ci_noff[r+1]-ci_noff[r];
        if (l==0) continue;
        const char* s = ci_note_d.data + ci_noff[r];
        bool wm = (l==8 && std::memcmp(s,"(writer)",8)==0) ||
                  (l==13 && std::memcmp(s,"(head writer)",13)==0) ||
                  (l==12 && std::memcmp(s,"(written by)",12)==0) ||
                  (l==7 && std::memcmp(s,"(story)",7)==0) ||
                  (l==14 && std::memcmp(s,"(story editor)",14)==0);
        if (wm) {
            std::printf("  ci row=%d pid=%d note=%.*s\n", r, ci_pid.data[r], (int)l, s);
            writer_count++;
        }
    }
    std::printf("writer-note rows: %d\n", writer_count);
    return 0;
}
