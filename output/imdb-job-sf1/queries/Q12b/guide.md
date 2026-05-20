## SQL

```sql
SELECT MIN(mi.info) AS budget,
       MIN(t.title) AS unsuccsessful_movie
FROM company_name AS cn, company_type AS ct,
     info_type AS it1, info_type AS it2,
     movie_companies AS mc, movie_info AS mi, movie_info_idx AS mi_idx, title AS t
WHERE cn.country_code = '[us]'
  AND ct.kind IS NOT NULL
  AND (ct.kind = 'production companies' OR ct.kind = 'distributors')
  AND it1.info = 'budget'
  AND it2.info = 'bottom 10 rank'
  AND t.production_year > 2000
  AND (t.title LIKE 'Birdemic%' OR t.title LIKE '%Movie%')
  AND t.id = mi.movie_id AND t.id = mi_idx.movie_id
  AND mi.info_type_id = it1.id AND mi_idx.info_type_id = it2.id
  AND t.id = mc.movie_id AND ct.id = mc.company_type_id AND cn.id = mc.company_id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `[us]` → `us_code`. Predicate `cc == us_code`.

### company_name.name (dense PK side)
Identity ids. `company_name/name.off|.dat` not used (no MIN over cn.name in Q12b).

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat` (4 rows). Resolve ids for `production companies` and `distributors` → set `CT` of ≤2 ids (both non-NULL).

### info_type.info (varlen)
Files: `info_type/info.off|.dat`. Resolve `budget` → `it1_id`, `bottom 10 rank` → `it2_id`.

### movie_companies (movie_id/company_id/company_type_id)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`.

### movie_info (movie_id/info_type_id/info)
Files: `movie_info/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. No predicate on mi.info other than join + it1_id; mi.info goes into MIN.

### movie_info_idx (movie_id/info_type_id/info)
Files: `movie_info_idx/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Predicate: existence of row with `info_type_id == it2_id`.

### title (id/title/production_year)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/production_year.bin`. `> 2000` non-NULL.

## Table Stats

| Table | Rows | Sort |
|---|---|---|
| company_name | 234997 | id |
| company_type | 4 | id |
| info_type | 113 | id |
| movie_companies | 2609129 | movie_id |
| movie_info | 14835720 | movie_id |
| movie_info_idx | 1380035 | movie_id |
| title | 2528312 | id |

## Query Analysis

Join graph: title hub; mi, mi_idx, mc all join on t.id.

Driver: `mi_idx.info_type_id = bottom 10 rank` is extremely rare (a few thousand rows). Plus title LIKE `Birdemic%` or `%Movie%` narrows title rows substantially.

Plan (mi_idx-driven via CSR):

1. Resolve dims: `us_code`, `CT` (≤2 ids), `it1_id (budget)`, `it2_id (bottom 10 rank)`.
2. Use CSR `movie_info_idx__info_type_id` with `it2_id`: enumerate all mi_idx rows for `bottom 10 rank`. Collect `mid` candidates (and remember mi_idx row for any later checks — only existence is required here).
3. For each `mid`:
   - title row mid-1: require `production_year != INT32_MIN && py > 2000`. Test title prefix `Birdemic` (8 bytes, `memcmp`) OR substring `%Movie%` (5 bytes, `memmem`). Use length prune.
   - Probe `movie_info__movie_id` offsets `[lo,hi)`: scan rows with `info_type_id == it1_id` — capture mi.info (for MIN).
   - Probe `movie_companies__movie_id` offsets `[lo,hi)`: scan rows where `company_type_id ∈ CT`; load `company_id`, check `company_name.country_code[cid-1] == us_code`.
4. Update MIN(mi.info), MIN(t.title).

MIN aggregation: byte-wise. MIN(mi.info) captured per qualifying mi row.

LIKE notes: `Birdemic%` is anchored prefix (8 bytes) — `len>=8 && memcmp == 0`. `%Movie%` is unanchored — `memmem` on title bytes.

Selectivities: `it2 = 'bottom 10 rank'` mi_idx rows are few — possibly the dominant funnel. `t.production_year > 2000` keeps ~25% of titles. Title LIKE narrows further.

## Indexes

### movie_info_idx__info_type_id (CSR)
- `<storage>/_idx/movie_info_idx__info_type_id__offsets.bin` (int32, 115)
- `<storage>/_idx/movie_info_idx__info_type_id__rowids.bin` (int32, 1380035)

```cpp
int32_t lo = mii_it_off[it2_id], hi = mii_it_off[it2_id+1];
for (int32_t k = lo; k < hi; ++k) {
    int32_t mii_row = mii_it_rowids[k];
    int32_t mid = mii_movie_id[mii_row];
    /* probe title, movie_info, movie_companies */
}
```

### movie_info__movie_id (offsets_only)
- `<storage>/_idx/movie_info__movie_id__offsets.bin`
- Scan rows with `info_type_id == it1_id` to read mi.info.

### movie_companies__movie_id (offsets_only)
- `<storage>/_idx/movie_companies__movie_id__offsets.bin`
- Scan rows with `company_type_id ∈ CT`, lookup company_name.country_code.

Skipped: `movie_info__info_type_id` CSR — title hub is already narrow after mi_idx funnel. `movie_companies__company_id` CSR — driving from companies is too wide.
