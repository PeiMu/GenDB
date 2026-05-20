## SQL

```sql
SELECT MIN(at1.title) AS aka_title,
       MIN(t.title) AS internet_movie_title
FROM aka_title AS at1, company_name AS cn, company_type AS ct, info_type AS it1,
     keyword AS k, movie_companies AS mc, movie_info AS mi, movie_keyword AS mk, title AS t
WHERE cn.country_code = '[us]'
  AND it1.info = 'release dates'
  AND mi.note LIKE '%internet%'
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
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `us_code` at runtime.

### info_type.info (varlen)
Files: `info_type/info.{off,dat}`. `it1_id = id of 'release dates'`.

### title.production_year, title.title (int32 nullable / varlen)
Files: `title/production_year.bin` (NULL=INT32_MIN), `title/title.{off,dat}`. Filter `production_year[r] != INT32_MIN && production_year[r] > 1990`.

### aka_title.movie_id, aka_title.title (int32 / varlen)
Files: `aka_title/movie_id.bin` (int32, 361472), `aka_title/title.off`, `aka_title/title.dat`. Sorted by movie_id → use `aka_title__movie_id` offsets_only to fetch all at1 rows for a given mv.
```cpp
auto at1_title_off = read_vec<int64_t>(store + "/aka_title/title.off");
std::string at1_title_dat = read_file(store + "/aka_title/title.dat");
```

### movie_companies.company_id, .movie_id (int32)
Files: `movie_companies/company_id.bin`, `movie_companies/movie_id.bin` (2609129; sorted by movie_id). No mc.note filter.

### movie_info.note, .info_type_id, .movie_id (varlen/int32/int32)
Files: `movie_info/note.{off,dat}`, `movie_info/info_type_id.bin`, `movie_info/movie_id.bin` (14835720). Filter `info_type_id==it1_id` and `note LIKE '%internet%'` (memmem).

### keyword, movie_keyword, company_type
No filter on these; existence-only joins.

## Table Stats

| Table | Rows | Sort | Notes |
|---|---|---|---|
| info_type | 113 | id | dense-PK |
| company_type | 4 | id | dense-PK |
| company_name | 234,997 | id | dense-PK; country_code int16 dict |
| keyword | 134,170 | id | dense-PK |
| title | 2,528,312 | id | dense-PK |
| aka_title | 361,472 | movie_id | offsets_only |
| movie_companies | 2,609,129 | movie_id | offsets_only |
| movie_info | 14,835,720 | movie_id | offsets_only; CSR on info_type_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only |

## Query Analysis

Same star as Q15a/b/c with weakest filters. MIN(at1.title) requires accessing aka_title row(s), not just existence.

Driver: mi-driven via CSR on info_type_id.
1. Resolve `us_code`, `it1_id`.
2. CSR-iterate mi rows with `info_type_id == it1_id`.
3. For each mi row r: require `note LIKE '%internet%'` (memmem('internet')). `mv = movie_id[r]`.
4. Title filter: `production_year[mv-1] != INT32_MIN && > 1990`.
5. Existence checks for `mc` with `cn.country_code='[us]'` (probe `movie_companies__movie_id` for mv; verify any mc row has `cn_cc[company_id-1]==us_code`).
6. Existence for `movie_keyword__movie_id`.
7. For each `at1` row in `aka_title__movie_id` range for mv, update `MIN(at1.title)`. Also update `MIN(t.title)`.

Selectivities:
- info_type='release dates' → relatively small slice of mi.
- mi.note LIKE '%internet%' → small.
- cn '[us]' → ~30% of companies.
- production_year > 1990 → vast majority.

LIKE notes:
- `mi.note LIKE '%internet%'`: `memmem(p, n, "internet", 8)`.

Important: MIN(at1.title) requires reading at1.title per surviving mv; iterate the offsets_only range and lex-compare each at1 title with the running minimum.

## Indexes

### movie_info__info_type_id (CSR)
Files: `_idx/movie_info__info_type_id__offsets.bin` (int32, 115), `_idx/movie_info__info_type_id__rowids.bin` (int32, 14835720).
```cpp
auto mi_it_off = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__offsets.bin");
auto mi_it_row = read_vec<int32_t>(store + "/_idx/movie_info__info_type_id__rowids.bin");
int32_t lo = mi_it_off[it1_id], hi = mi_it_off[it1_id+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r = mi_it_row[k]; }
```

### aka_title__movie_id (offsets_only)
File: `_idx/aka_title__movie_id__offsets.bin` (int32, 2528314). aka_title sorted by movie_id.

### movie_companies__movie_id (offsets_only)
File: `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314).
