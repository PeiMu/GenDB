## SQL

```sql
SELECT MIN(mi.info) AS release_date,
       MIN(t.title) AS modern_american_internet_movie
FROM aka_title AS at1, company_name AS cn, company_type AS ct, info_type AS it1,
     keyword AS k, movie_companies AS mc, movie_info AS mi, movie_keyword AS mk, title AS t
WHERE cn.country_code = '[us]'
  AND it1.info = 'release dates'
  AND mi.note LIKE '%internet%'
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'USA:% 199%' OR mi.info LIKE 'USA:% 200%')
  AND t.production_year > 1990
  AND t.id = at1.movie_id AND t.id = mi.movie_id
  AND t.id = mk.movie_id AND t.id = mc.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin` (int16, 234997), `.dict.off`, `.dict.dat`. Resolve `us_code` at runtime; never hardcode.

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. `it1_id = id of 'release dates'`.

### title.production_year, title.title
Files: `title/production_year.bin` (NULL=INT32_MIN), `title/title.{off,dat}`. Filter `production_year != INT32_MIN && production_year > 1990`.

### movie_companies.company_id, .movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (2609129; sorted by movie_id). No LIKE on mc.note in Q15c.

### movie_info.info, .note, .info_type_id, .movie_id
Files: `movie_info/*` (14835720). `mi.info IS NOT NULL` → varlen non-empty (`off[r+1] > off[r]`).
```cpp
auto mi_info_off = read_vec<int64_t>(store + "/movie_info/info.off");
std::string mi_info_dat = read_file(store + "/movie_info/info.dat");
auto mi_info_match = [&](int32_t r){
    int64_t a=mi_info_off[r], b=mi_info_off[r+1];
    size_t n = (size_t)(b-a);
    if (n < 4) return false;
    const char* p = mi_info_dat.data()+a;
    if (memcmp(p,"USA:",4)!=0) return false;
    // " 199" or " 200" anywhere after the 4-byte prefix
    return memmem(p+4,n-4," 199",4) || memmem(p+4,n-4," 200",4);
};
auto mi_note_off = read_vec<int64_t>(store + "/movie_info/note.off");
std::string mi_note_dat = read_file(store + "/movie_info/note.dat");
auto mi_note_match = [&](int32_t r){
    const char* p = mi_note_dat.data()+mi_note_off[r];
    size_t n = (size_t)(mi_note_off[r+1]-mi_note_off[r]);
    return memmem(p,n,"internet",8)!=nullptr;
};
```

### keyword, aka_title, company_type
Existence-only joins (no filter beyond join).

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
| movie_info | 14,835,720 | movie_id | offsets_only; CSR on info_type_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only |

## Query Analysis

Same star as Q15a but no `mc.note` filter and no specific cn.name. Looser year filter.

MIN aggregates: `MIN(mi.info)`, `MIN(t.title)` (both varlen).

Driver: mi-driven (or title-driven with movie_info CSR).

Approach (mi-driven via CSR on info_type_id):
1. Resolve `us_code`, `it1_id`.
2. Use `movie_info__info_type_id` CSR to enumerate mi rows with `info_type_id == it1_id` (~few hundred thousand rows).
3. For each such mi row r: apply `mi_note_match(r) && mi_info_match(r)`.
4. Take `mv = movie_id[r]`. Check `production_year[mv-1] != INT32_MIN && production_year[mv-1] > 1990`.
5. Probe `movie_companies__movie_id` for mv; for each mc row require `cn_cc[company_id-1]==us_code`. Existence.
6. Probe `aka_title__movie_id`, `movie_keyword__movie_id` for existence.
7. Update MIN(mi.info), MIN(t.title).

Selectivities:
- `info_type='release dates'` → ~tens of thousands in mi.
- `mi.note LIKE '%internet%'` → small subset.
- `mi.info LIKE USA:% 19/200%` → small.
- cn '[us]' → ~30% of company_name.
- production_year > 1990 → most.

LIKE notes: 4 substring matches via memmem; combine USA:% 199% / USA:% 200% by OR after the `USA:` prefix.

## Indexes

### movie_info__info_type_id (CSR)
Files: `_idx/movie_info__info_type_id__offsets.bin` (int32, 115), `_idx/movie_info__info_type_id__rowids.bin` (int32, 14835720).
```cpp
auto mi_it_off = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__offsets.bin");
auto mi_it_row = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__rowids.bin");
int32_t lo = mi_it_off[it1_id], hi = mi_it_off[it1_id+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r = mi_it_row[k]; /* mi row r */ }
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### aka_title__movie_id (offsets_only)
File: `_idx/aka_title__movie_id__offsets.bin` (int32, 2528314).

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
