## SQL

```sql
SELECT MIN(mi.info) AS release_date,
       MIN(t.title) AS youtube_movie
FROM aka_title AS at1, company_name AS cn, company_type AS ct, info_type AS it1,
     keyword AS k, movie_companies AS mc, movie_info AS mi, movie_keyword AS mk, title AS t
WHERE cn.country_code = '[us]'
  AND cn.name = 'YouTube'
  AND it1.info = 'release dates'
  AND mc.note LIKE '%(200%)%'
  AND mc.note LIKE '%(worldwide)%'
  AND mi.note LIKE '%internet%'
  AND mi.info LIKE 'USA:% 200%'
  AND t.production_year BETWEEN 2005 AND 2010
  AND t.id = at1.movie_id AND t.id = mi.movie_id
  AND t.id = mk.movie_id AND t.id = mc.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id;
```

## Column Reference

### company_name.country_code (int16 dict), company_name.name (varlen)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`; `company_name/name.off`, `company_name/name.dat`. Resolve `us_code` at runtime AND the `cn_id` for `name='YouTube'`.
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t us_code=0;
for (size_t i=0; i+1<cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[us]") { us_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");
auto cn_name_off = read_vec<int64_t>(store + "/company_name/name.off");
std::string cn_name_dat = read_file(store + "/company_name/name.dat");
// Find row(s) where name == "YouTube" AND country_code == us_code
std::vector<int32_t> cn_ids;
for (int32_t i=0; i+1<(int32_t)cn_name_off.size(); ++i) {
    if (cn_cc[i] != us_code) continue;
    int64_t a=cn_name_off[i], b=cn_name_off[i+1];
    if (b-a == 7 && memcmp(cn_name_dat.data()+a, "YouTube", 7)==0) cn_ids.push_back(i+1);
}
```

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. `it1_id = id of 'release dates'`.

### title.production_year, title.title
Files: `title/production_year.bin` (NULL=INT32_MIN), `title/title.{off,dat}`. Filter `production_year != INT32_MIN && production_year >= 2005 && production_year <= 2010`.

### movie_companies.note, .company_id, .movie_id (varlen/int32/int32)
Files: `movie_companies/note.{off,dat}`, `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (length 2609129). Apply LIKE patterns; require `company_id` ∈ `cn_ids` (typically just 1 id for YouTube).

### movie_info.info, .note, .info_type_id, .movie_id
Files: `movie_info/*` (14835720). LIKE filters as in Q15a:
```cpp
auto mi_info_match = [](const char* p, size_t n){
    if (n < 4 || memcmp(p,"USA:",4)!=0) return false;
    return memmem(p+4,n-4," 200",4)!=nullptr;
};
auto mi_note_match = [](const char* p, size_t n){ return memmem(p,n,"internet",8)!=nullptr; };
```

### keyword, aka_title, company_type
Existence-only joins (no literal filter on these tables in WHERE besides the join).

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| company_type | 4 | id | dense-PK |
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| aka_title | 361,472 | movie_id | offsets_only |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id; CSR on company_id |
| movie_info | 14,835,720 | movie_id | offsets_only |
| movie_keyword | 4,523,930 | movie_id | offsets_only |

## Query Analysis

Same star as Q15a with the extra `cn.name='YouTube'` filter — extremely selective on cn.

MIN: `MIN(mi.info)`, `MIN(t.title)` (varlen).

Driver: company-driven via `movie_companies__company_id` (CSR).
1. Resolve `us_code`, `it1_id`, `cn_ids` (typically 1).
2. For each `cn_id` in cn_ids, use CSR `movie_companies__company_id` to enumerate mc rows.
3. For each mc row r: check `mc_note_ok(r)` (both LIKEs). Get `mv = mc.movie_id[r]`.
4. Check title r-1 = `mv-1`: `production_year ∈ [2005,2010]`.
5. Verify presence in `aka_title__movie_id`, `movie_keyword__movie_id` (existence).
6. Walk `movie_info__movie_id` for mv, filter `info_type_id==it1_id` and LIKE filters. Update MINs.

Selectivities:
- `cn.name='YouTube' AND country_code='[us]'` → very small (~1 row).
- mc rows per company: small for YouTube.
- production_year BETWEEN 2005 AND 2010 → ~10% of titles, but already narrowed.

LIKE notes:
- `mc.note LIKE '%(200%)%'` and `'%(worldwide)%'`: two memmem calls.
- `mi.note LIKE '%internet%'`: memmem.
- `mi.info LIKE 'USA:% 200%'`: prefix + memmem.

## Indexes

### movie_companies__company_id (CSR)
Files: `_idx/movie_companies__company_id__offsets.bin` (int32, 234999), `_idx/movie_companies__company_id__rowids.bin` (int32, 2609129).
```cpp
auto mc_co_off = read_vec<int32_t>(store + "/_idx/movie_companies__company_id__offsets.bin");
auto mc_co_row = read_vec<int32_t>(store + "/_idx/movie_companies__company_id__rowids.bin");
int32_t lo = mc_co_off[cn_id], hi = mc_co_off[cn_id+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r = mc_co_row[k]; /* mc row r */ }
```

### aka_title__movie_id (offsets_only)
File: `_idx/aka_title__movie_id__offsets.bin` (int32, 2528314).

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
