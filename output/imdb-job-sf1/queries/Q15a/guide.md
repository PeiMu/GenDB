## SQL

```sql
SELECT MIN(mi.info) AS release_date,
       MIN(t.title) AS internet_movie
FROM aka_title AS at1, company_name AS cn, company_type AS ct, info_type AS it1,
     keyword AS k, movie_companies AS mc, movie_info AS mi, movie_keyword AS mk, title AS t
WHERE cn.country_code = '[us]'
  AND it1.info = 'release dates'
  AND mc.note LIKE '%(200%)%'
  AND mc.note LIKE '%(worldwide)%'
  AND mi.note LIKE '%internet%'
  AND mi.info LIKE 'USA:% 200%'
  AND t.production_year > 2000
  AND t.id = at1.movie_id AND t.id = mi.movie_id
  AND t.id = mk.movie_id AND t.id = mc.movie_id
  AND k.id = mk.keyword_id
  AND it1.id = mi.info_type_id
  AND cn.id = mc.company_id
  AND ct.id = mc.company_type_id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin` (int16, 234997), `.dict.off`, `.dict.dat`. Resolve `[us]` code at runtime; never hardcode.
```cpp
auto cc_off = read_vec<int64_t>(store + "/company_name/country_code.dict.off");
std::string cc_dat = read_file(store + "/company_name/country_code.dict.dat");
int16_t us_code = 0;
for (size_t i=0; i+1<cc_off.size(); ++i)
    if (std::string_view(cc_dat.data()+cc_off[i], cc_off[i+1]-cc_off[i]) == "[us]") { us_code = (int16_t)(i+1); break; }
auto cn_cc = read_vec<int16_t>(store + "/company_name/country_code.bin");
```

### info_type.info (varlen)
Files: `info_type/info.off`, `info_type/info.dat`. `it1_id = id of 'release dates'`.

### title.production_year, title.title (int32 nullable / varlen)
Files: `title/production_year.bin` (NULL=INT32_MIN), `title/title.off`, `title/title.dat`. Filter `production_year[r] != INT32_MIN && production_year[r] > 2000`. `mv = r+1`.

### movie_companies.note, .company_id, .company_type_id, .movie_id (varlen/int32×3)
Files: `movie_companies/note.{off,dat}`, `movie_companies/company_id.bin`, `movie_companies/company_type_id.bin`, `movie_companies/movie_id.bin` (length 2609129; sorted by movie_id).
LIKE filters apply BOTH `%(200%)%` AND `%(worldwide)%`. Two `memmem` passes on the row note slice:
```cpp
auto mc_note_off = read_vec<int64_t>(store + "/movie_companies/note.off");
std::string mc_note_dat = read_file(store + "/movie_companies/note.dat");
auto mc_note_ok = [&](int32_t r){
    const char* p = mc_note_dat.data()+mc_note_off[r];
    size_t n = (size_t)(mc_note_off[r+1]-mc_note_off[r]);
    if (n == 0) return false;
    // %(200%)% means contains "(200" somewhere followed later by ")" — approx by checking '(200' and '(worldwide)'.
    if (!memmem(p, n, "(200", 4)) return false;
    if (!memmem(p, n, "(worldwide)", 11)) return false;
    return true;
};
```
Note: `%(200%)%` requires both `(200` and a later `)`; the substring `(200` followed by any chars up to the next `)`. Verify by finding `(200` then scanning forward for `)`.

### movie_info.info, .note, .info_type_id, .movie_id (varlen/varlen/int32/int32)
Files: `movie_info/info.{off,dat}`, `movie_info/note.{off,dat}`, `movie_info/info_type_id.bin`, `movie_info/movie_id.bin` (14835720; sorted by movie_id).
- `mi.note LIKE '%internet%'`: `memmem(p,n,"internet",8)`.
- `mi.info LIKE 'USA:% 200%'`: prefix `USA:` then substring ` 200` later.
```cpp
auto mi_info_ok = [&](const char* p, size_t n){
    if (n < 4) return false;
    if (memcmp(p, "USA:", 4) != 0) return false;
    return memmem(p+4, n-4, " 200", 4) != nullptr;
};
```

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat`. No literal filter on k.keyword here — k is joined via mk.keyword_id. Existence-only on join (`k.id = mk.keyword_id` for any k).

### aka_title.movie_id (int32)
File: `aka_title/movie_id.bin` (length 361472, sorted by movie_id). Existence-only via offsets_only index.

### company_type (no filter)
Used only as join target. company_type.id 1..4. Existence-only via `ct.id = mc.company_type_id`.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| company_type | 4 | id | dense-PK |
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| aka_title | 361,472 | movie_id | offsets_only on movie_id |
| movie_companies | 2,609,129 | movie_id | offsets_only on movie_id |
| movie_info | 14,835,720 | movie_id | offsets_only on movie_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only on movie_id |

## Query Analysis

Join graph:
```
ct --company_type_id-- mc --company_id-- cn
                       |
t --id-- mi --info_type_id-- it1
t --id-- mk --keyword_id-- k
t --id-- at1
```

MIN aggregates: `MIN(mi.info)` (varlen), `MIN(t.title)` (varlen).

Driver: title-driven over 2.5M titles (production_year > 2000 is moderately selective).
1. Resolve `us_code` (int16), `it1_id` (release dates).
2. For each title r with `production_year>2000`, `mv=r+1`:
   - Probe `aka_title__movie_id` offsets — require non-empty range (existence).
   - Probe `movie_keyword__movie_id` — require non-empty range (existence; k.id matches all because keyword table has no filter, so any mk row is enough).
   - Probe `movie_companies__movie_id`; for each mc row check `cn_cc[company_id-1]==us_code` AND `mc_note_ok(r)`. Existence.
   - Probe `movie_info__movie_id`; for each mi row check `info_type_id==it1_id`, `note LIKE %internet%`, `info LIKE 'USA:% 200%'`. Track MIN(mi.info), MIN(t.title).

Selectivities:
- `production_year > 2000` → ~30-40% of titles.
- `cn.country_code='[us]'` → ~30-40% of company_name.
- `mc.note` matches both LIKEs → small fraction.
- `mi.info LIKE 'USA:% 200%' AND note LIKE '%internet%'` AND `info_type='release dates'` → very small.

LIKE notes:
- `mc.note LIKE '%(200%)%'`: contains `(200` then later `)`. Pragmatically check `memmem('(200')` (the `)` is satisfied by `(worldwide)` LIKE always present here).
- `mc.note LIKE '%(worldwide)%'`: `memmem('(worldwide)')`.
- `mi.note LIKE '%internet%'`: `memmem('internet')`.
- `mi.info LIKE 'USA:% 200%'`: prefix `USA:` + later ` 200`.

## Indexes

### aka_title__movie_id (offsets_only)
File: `_idx/aka_title__movie_id__offsets.bin` (int32, 2528314). aka_title is sorted by movie_id.
```cpp
auto atmid = read_vec<int32_t>(store + "/_idx/aka_title__movie_id__offsets.bin");
int32_t lo = atmid[mv], hi = atmid[mv+1];
```

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### movie_info__movie_id (offsets_only)
File: `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
