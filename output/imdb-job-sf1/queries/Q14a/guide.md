## SQL

```sql
SELECT MIN(mi_idx.info) AS rating,
       MIN(t.title) AS northern_dark_movie
FROM info_type AS it1, info_type AS it2, keyword AS k, kind_type AS kt,
     movie_info AS mi, movie_info_idx AS mi_idx, movie_keyword AS mk, title AS t
WHERE it1.info = 'countries'
  AND it2.info = 'rating'
  AND k.keyword IN ('murder','murder-in-title','blood','violence')
  AND kt.kind = 'movie'
  AND mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish','Norwegian','German','USA','American')
  AND mi_idx.info < '8.5'
  AND t.production_year > 2010
  AND kt.id = t.kind_id
  AND t.id = mi.movie_id AND t.id = mk.movie_id AND t.id = mi_idx.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### info_type.info (varlen)
Files: `info_type/info.off` (int64, 114), `info_type/info.dat`. Resolve two ids by scan.
```cpp
auto it_off = read_vec<int64_t>(store + "/info_type/info.off");
std::string it_dat = read_file(store + "/info_type/info.dat");
int32_t it1_id=0, it2_id=0;
for (size_t i=0; i+1<it_off.size(); ++i) {
    std::string_view s(it_dat.data()+it_off[i], it_off[i+1]-it_off[i]);
    if (s == "countries") it1_id = (int32_t)(i+1);
    else if (s == "rating") it2_id = (int32_t)(i+1);
}
```

### kind_type.kind (varlen)
Files: `kind_type/kind.off` (int64, 8), `kind_type/kind.dat`. Resolve `kt_id = id where kind=='movie'`.

### keyword.keyword (varlen)
Files: `keyword/keyword.off` (int64, 134171), `keyword/keyword.dat`. Scan once, build a set of keyword ids matching IN(...).
```cpp
std::unordered_set<int32_t> k_ids;
std::vector<std::string_view> wanted = {"murder","murder-in-title","blood","violence"};
auto k_off = read_vec<int64_t>(store + "/keyword/keyword.off");
std::string k_dat = read_file(store + "/keyword/keyword.dat");
for (size_t i=0; i+1<k_off.size(); ++i) {
    std::string_view s(k_dat.data()+k_off[i], k_off[i+1]-k_off[i]);
    for (auto w : wanted) if (s == w) { k_ids.insert((int32_t)(i+1)); break; }
}
```

### title.kind_id, title.production_year, title.title (int32/int32/varlen)
Files: `title/kind_id.bin` (int32, 2528312), `title/production_year.bin` (int32, nullable, INT32_MIN=NULL), `title/title.off`, `title/title.dat`. Filter `production_year[r] != INT32_MIN && production_year[r] > 2010 && kind_id[r] == kt_id`.

### movie_keyword.keyword_id, movie_keyword.movie_id (int32)
Files: `movie_keyword/keyword_id.bin`, `movie_keyword/movie_id.bin` (length 4523930). Sorted by movie_id.

### movie_info.info_type_id, movie_info.info, movie_info.movie_id (int32/varlen/int32)
Files: `movie_info/info_type_id.bin`, `movie_info/info.off`, `movie_info/info.dat`, `movie_info/movie_id.bin` (length 14835720). Filter `info_type_id==it1_id` and `info` in set of country strings via length+`memcmp`.

### movie_info_idx.info_type_id, movie_info_idx.info, movie_info_idx.movie_id (int32/varlen/int32)
Files: `movie_info_idx/info_type_id.bin`, `movie_info_idx/info.off`, `movie_info_idx/info.dat`, `movie_info_idx/movie_id.bin` (length 1380035). `mi_idx.info < '8.5'` is STRING compare, not numeric.
```cpp
auto lex_lt = [](std::string_view a, std::string_view b){
    size_t n = std::min(a.size(), b.size());
    int c = memcmp(a.data(), b.data(), n);
    return c < 0 || (c == 0 && a.size() < b.size());
};
// per row r: lex_lt(info_at(r), "8.5")
```

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| kind_type | 7 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| movie_keyword | 4,523,930 | movie_id | offsets_only on movie_id; CSR on keyword_id |
| movie_info | 14,835,720 | movie_id | offsets_only on movie_id; CSR on info_type_id |
| movie_info_idx | 1,380,035 | movie_id | offsets_only on movie_id; CSR on info_type_id |

## Query Analysis

Join graph (star around title):
```
kt --kind_id-- t --id-- mi --info_type_id-- it1
              t --id-- mk --keyword_id-- k
              t --id-- mi_idx --info_type_id-- it2
```

MIN aggregates: `MIN(mi_idx.info)` (varlen lexical), `MIN(t.title)` (varlen lexical).

Driver: title-driven scan.
1. Resolve `it1_id` (countries), `it2_id` (rating), `kt_id` (movie), `k_ids` set.
2. Walk titles `r=0..2528311`. Require `kind_id[r]==kt_id && production_year[r]!=INT32_MIN && production_year[r]>2010`. `mv = r+1`.
3. For each mv use `movie_keyword__movie_id` offsets to scan mk rows; check `keyword_id` ∈ `k_ids`. Break on first match (only need existence).
4. Use `movie_info__movie_id` offsets to scan mi rows; require `info_type_id==it1_id` and `info` in country set. Existence.
5. Use `movie_info_idx__movie_id` offsets to scan mi_idx rows; require `info_type_id==it2_id` and `lex_lt(info, "8.5")`. For surviving rows update MIN(mi_idx.info) and MIN(t.title).

Selectivities (rough):
- `production_year > 2010` → ~10-15% of 2.5M titles.
- `kind_id=='movie'` → ~7% of titles.
- `k.keyword` IN(4 vals) → tens of thousands of mk rows total.
- `mi.info` IN 10 country strings with `it1='countries'` → small (mi has 14M rows but `info_type='countries'` is ~250K).
- `mi_idx.info < '8.5'` lex → majority of ratings.

LIKE notes: no LIKE here; only equality on dictionaries and IN on varlen.

## Indexes

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314). Slot 0 = count of rows with movie_id<1.
```cpp
auto mkmid = read_vec<int32_t>(store + "/_idx/movie_keyword__movie_id__offsets.bin");
int32_t lo = mkmid[mv], hi = mkmid[mv+1];
for (int32_t r=lo; r<hi; ++r) { /* mk row r */ }
```

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mimid = read_vec<int32_t>(store + "/_idx/movie_info__movie_id__offsets.bin");
int32_t lo = mimid[mv], hi = mimid[mv+1];
```

### movie_info_idx__movie_id (offsets_only)
File: `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314).
```cpp
auto mixmid = read_vec<int32_t>(store + "/_idx/movie_info_idx__movie_id__offsets.bin");
int32_t lo = mixmid[mv], hi = mixmid[mv+1];
```
