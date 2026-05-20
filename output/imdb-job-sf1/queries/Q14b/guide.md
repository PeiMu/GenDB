## SQL

```sql
SELECT MIN(mi_idx.info) AS rating,
       MIN(t.title) AS western_dark_production
FROM info_type AS it1, info_type AS it2, keyword AS k, kind_type AS kt,
     movie_info AS mi, movie_info_idx AS mi_idx, movie_keyword AS mk, title AS t
WHERE it1.info = 'countries'
  AND it2.info = 'rating'
  AND k.keyword IN ('murder','murder-in-title')
  AND kt.kind = 'movie'
  AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish','Norwegian','German','USA','American')
  AND mi_idx.info > '6.0'
  AND t.production_year > 2010
  AND (t.title LIKE '%murder%' OR t.title LIKE '%Murder%' OR t.title LIKE '%Mord%')
  AND kt.id = t.kind_id
  AND t.id = mi.movie_id AND t.id = mk.movie_id AND t.id = mi_idx.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. Resolve `it1_id=id of 'countries'`, `it2_id=id of 'rating'` via single dictionary scan.

### kind_type.kind (varlen)
Files: `kind_type/kind.off`, `kind_type/kind.dat`. `kt_id = id of 'movie'`.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. IN('murder','murder-in-title') → small set of keyword ids.

### title.kind_id, title.production_year, title.title (int32/int32/varlen)
Files: `title/kind_id.bin` (int32, 2528312), `title/production_year.bin` (int32 nullable, NULL=INT32_MIN), `title/title.off`, `title/title.dat`. LIKE %murder%/%Murder%/%Mord% applied via `memmem` on title slice.
```cpp
auto t_title_off = read_vec<int64_t>(store + "/title/title.off");
std::string t_title_dat = read_file(store + "/title/title.dat");
auto title_match = [&](int32_t r){
    const char* p = t_title_dat.data()+t_title_off[r];
    size_t n = (size_t)(t_title_off[r+1]-t_title_off[r]);
    return memmem(p,n,"murder",6) || memmem(p,n,"Murder",6) || memmem(p,n,"Mord",4);
};
```

### movie_keyword.keyword_id, movie_keyword.movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (length 4523930). Sorted by movie_id.

### movie_info.info_type_id, movie_info.info, movie_info.movie_id (int32/varlen/int32)
Files: `movie_info/info_type_id.bin`, `movie_info/info.off`, `movie_info/info.dat`, `movie_info/movie_id.bin` (length 14835720). Build flat hash set of the 10 country strings, length-prefilter then memcmp.

### movie_info_idx.info_type_id, movie_info_idx.info, movie_info_idx.movie_id (int32/varlen/int32)
Files: `movie_info_idx/*` (length 1380035). `mi_idx.info > '6.0'` is STRING compare. C++:
```cpp
auto lex_gt = [](std::string_view a, std::string_view b){
    size_t n = std::min(a.size(), b.size());
    int c = memcmp(a.data(), b.data(), n);
    return c > 0 || (c == 0 && a.size() > b.size());
};
```

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| kind_type | 7 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| movie_keyword | 4,523,930 | movie_id | offsets_only on movie_id |
| movie_info | 14,835,720 | movie_id | offsets_only on movie_id |
| movie_info_idx | 1,380,035 | movie_id | offsets_only on movie_id |

## Query Analysis

Same star as Q14a plus title-LIKE.

Driver: title-driven.
1. Resolve `it1_id`, `it2_id`, `kt_id`, `k_ids`.
2. Scan title rows; require `production_year[r] != INT32_MIN && production_year[r] > 2010 && kind_id[r] == kt_id` and `title_match(r)`.
3. For surviving title id `mv=r+1`, use `movie_keyword__movie_id` to find any mk row with `keyword_id ∈ k_ids` (existence).
4. Use `movie_info__movie_id` to find any mi row with `info_type_id==it1_id` and `info` ∈ country set.
5. Use `movie_info_idx__movie_id` to scan mi_idx rows; require `info_type_id==it2_id` and `lex_gt(info,"6.0")`. Update MIN(mi_idx.info) and MIN(t.title).

Selectivities:
- title LIKE %murder%/%Murder%/%Mord% → ~thousands of titles.
- production_year > 2010 + kind='movie' → further trim.
- k.keyword IN(2) → smaller than Q14a.
- mi_idx.info > '6.0' lex → roughly half of ratings.

LIKE notes: title scans use `memmem`; combine the three patterns with short-circuit OR.

## Indexes

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mkmid = read_vec<int32_t>(store + "/_idx/movie_keyword__movie_id__offsets.bin");
int32_t lo = mkmid[mv], hi = mkmid[mv+1];
```

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).

### movie_info_idx__movie_id (offsets_only)
File: `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314).
