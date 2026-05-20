## SQL

```sql
SELECT MIN(cn.name) AS movie_company,
       MIN(mi_idx.info) AS rating,
       MIN(t.title) AS mainstream_movie
FROM company_name AS cn, company_type AS ct,
     info_type AS it1, info_type AS it2,
     movie_companies AS mc, movie_info AS mi, movie_info_idx AS mi_idx, title AS t
WHERE cn.country_code = '[us]'
  AND ct.kind = 'production companies'
  AND it1.info = 'genres'
  AND it2.info = 'rating'
  AND mi.info IN ('Drama','Horror','Western','Family')
  AND mi_idx.info > '7.0'
  AND t.production_year BETWEEN 2000 AND 2010
  AND t.id = mi.movie_id AND t.id = mi_idx.movie_id
  AND mi.info_type_id = it1.id AND mi_idx.info_type_id = it2.id
  AND t.id = mc.movie_id AND ct.id = mc.company_type_id AND cn.id = mc.company_id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `[us]` → `us_code`.

### company_name.name (varlen)
Files: `company_name/name.off|.dat`. Used in MIN.

### company_name.id (dense PK)
Identity.

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat`. Resolve `production companies` → `ct_id`.

### info_type.info (varlen)
Files: `info_type/info.off|.dat`. Resolve `genres` → `it1_id`, `rating` → `it2_id`.

### movie_companies (movie_id/company_id/company_type_id)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`.

### movie_info (movie_id/info_type_id/info)
Files: `movie_info/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Predicate: `info_type_id == it1_id` AND `info ∈ {'Drama','Horror','Western','Family'}`. Build hashset of 4 string_views.

### movie_info_idx (movie_id/info_type_id/info)
Files: `movie_info_idx/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Predicate: `info_type_id == it2_id` AND `info > '7.0'` (lex compare).

### title (id/title/production_year)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/production_year.bin`. `BETWEEN 2000 AND 2010`.

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

Same skeleton as Q12a, with wider predicates: 11-year window, 4-element genre set, rating > 7.0.

Driver: title.production_year window — 11 years out of ~140 → ~10% of title (250k mids). Then probe mi_idx, mi, mc per mid.

Plan:

1. Resolve dims: `us_code`, `ct_id`, `it1_id`, `it2_id`. Build hashset `GENRES = {"Drama","Horror","Western","Family"}` (string_view).
2. Scan title.production_year; collect mids where `py ∈ [2000,2010]`.
3. For each `mid`:
   - Probe `movie_info_idx__movie_id` `[lo,hi)`. Scan rows with `info_type_id == it2_id` and `info > '7.0'`. Need ≥1; capture mi_idx.info for MIN over each.
   - Probe `movie_info__movie_id` `[lo,hi)`. Scan rows with `info_type_id == it1_id` and `info ∈ GENRES`. Need ≥1.
   - Probe `movie_companies__movie_id` `[lo,hi)`. Scan rows with `company_type_id == ct_id`. For each, lookup `company_name.country_code[cid-1] == us_code`. Capture cn.name.
4. MIN(cn.name), MIN(mi_idx.info), MIN(t.title).

MIN aggregation: byte-wise. Each qualifying tuple updates all three MINs.

LIKE notes: none. `info > '7.0'`: lex compare raw bytes; e.g. info `"7.0"` itself excluded (strict).

Selectivities: 11-year window ≈ 10%; mi_idx filtered to it2 ≈ ~50% of 1.38M (large); `info > '7.0'` cuts that to top ~30%; genres set in mi ≈ several %. The final join via mc → cn US-filter is moderately selective.

## Indexes

### movie_info_idx__movie_id (offsets_only)
- `<storage>/_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314)

```cpp
int32_t lo = mii_off[mid], hi = mii_off[mid+1];
for (int32_t r = lo; r < hi; ++r) {
    if (mii_info_type_id[r] != it2_id) continue;
    size_t s = mii_info_off[r], e = mii_info_off[r+1];
    if (lex_gt(dat+s, e-s, "7.0", 3)) { /* keep */ }
}
```

### movie_info__movie_id (offsets_only)
- `<storage>/_idx/movie_info__movie_id__offsets.bin`. Filter `info_type_id == it1_id` and `info ∈ GENRES`.

### movie_companies__movie_id (offsets_only)
- `<storage>/_idx/movie_companies__movie_id__offsets.bin`. Filter `company_type_id == ct_id`, then check us_code.

Skipped: CSRs on info_type_id — title-driven plan is balanced here. `movie_companies__company_id` CSR — not the driver.
