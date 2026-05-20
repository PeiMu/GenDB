## SQL

```sql
SELECT MIN(cn.name) AS movie_company,
       MIN(mi_idx.info) AS rating,
       MIN(t.title) AS drama_horror_movie
FROM company_name AS cn, company_type AS ct,
     info_type AS it1, info_type AS it2,
     movie_companies AS mc, movie_info AS mi, movie_info_idx AS mi_idx, title AS t
WHERE cn.country_code = '[us]'
  AND ct.kind = 'production companies'
  AND it1.info = 'genres'
  AND it2.info = 'rating'
  AND mi.info IN ('Drama','Horror')
  AND mi_idx.info > '8.0'
  AND t.production_year BETWEEN 2005 AND 2008
  AND t.id = mi.movie_id AND t.id = mi_idx.movie_id
  AND mi.info_type_id = it1.id AND mi_idx.info_type_id = it2.id
  AND t.id = mc.movie_id AND ct.id = mc.company_type_id AND cn.id = mc.company_id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `[us]` → `us_code`. Predicate: `cc == us_code` (excludes NULL since code 0 ≠ us_code).

### company_name.name (varlen)
Files: `company_name/name.off|.dat`. Only used for MIN.

### company_name.id (dense PK)
Identity.

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat` (4 rows). Resolve `production companies` → `ct_id`.

### info_type.info (varlen)
Files: `info_type/info.off|.dat` (113 rows). Resolve two literals: `it1_id` for `genres`, `it2_id` for `rating`.

### movie_companies (movie_id/company_id/company_type_id)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`. No note filter here.

### movie_info (movie_id/info_type_id/info)
Files: `movie_info/movie_id.bin`, `info_type_id.bin` (int32, 14835720 rows), `info.off|.dat`. Filter `info IN ('Drama','Horror')` — string set membership (lengths 5 and 6); prune by length first.

### movie_info_idx (movie_id/info_type_id/info)
Files: `movie_info_idx/movie_id.bin`, `info_type_id.bin` (int32, 1380035 rows), `info.off|.dat`. `info > '8.0'` is lexicographic string comparison (raw bytes).

### title (id/title/production_year)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/production_year.bin`. `BETWEEN 2005 AND 2008` requires non-NULL.

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

Join graph: title is the hub. `t.id = mc.movie_id = mi.movie_id = mi_idx.movie_id`. Dim joins on info_type, company_type, company_name.

Driver: `movie_info_idx` filtered by `info_type_id == it2_id (rating)` AND `info > '8.0'` is the smallest selective set. Most ratings rows have info_type_id matching rating already; filter further by string compare. Or even tighter: title.production_year BETWEEN 2005 AND 2008 (~4 years out of ~140 active years).

Plan (title-anchored):

1. Resolve dims: `us_code`, `ct_id`, `it1_id (genres)`, `it2_id (rating)`.
2. Scan title.production_year: collect `mids = {i+1 : 2005 <= py[i] <= 2008}`. Size ~50–100k.
3. For each `mid ∈ mids`:
   - Probe `movie_info_idx__movie_id` offsets `[lo,hi)`. Scan rows where `info_type_id == it2_id`; among those, check `info > '8.0'`. Capture `mi_idx.info` (smallest) — may be multiple.
   - If none, skip.
   - Probe `movie_info__movie_id` offsets `[lo,hi)`. Scan rows where `info_type_id == it1_id` AND `info ∈ {'Drama','Horror'}`. Existence required.
   - Probe `movie_companies__movie_id` offsets `[lo,hi)`. Scan rows where `company_type_id == ct_id`; load `company_id`, check `company_name.country_code[cid-1] == us_code`.
4. For qualifying tuples: update MIN(cn.name), MIN(mi_idx.info), MIN(t.title).

MIN aggregation: byte-wise. For `mi_idx.info` we may emit multiple candidates per mid (one per qualifying mi_idx row).

LIKE notes: none. String compare `> '8.0'`: lex compare raw bytes of the varlen slice against literal `"8.0"`. Use `memcmp` then length tiebreak.

Selectivities: `t.production_year ∈ [2005,2008]` keeps a few %; `mi_idx info_type_id == rating` keeps ~50% of mi_idx (rating is one of the largest info_types); `info > '8.0'` keeps top ratings (~10%); `mi.info IN ('Drama','Horror')` after filtering by it1=genres is a few %.

## Indexes

### movie_info_idx__movie_id (offsets_only)
- `<storage>/_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314)
- `[lo,hi)` per mid, scan with `info_type_id == it2_id` and `info > '8.0'`.

```cpp
int32_t lo = mii_off[mid], hi = mii_off[mid+1];
for (int32_t r = lo; r < hi; ++r) {
    if (mii_info_type_id[r] != it2_id) continue;
    size_t s = mii_info_off[r], e = mii_info_off[r+1];
    if (lex_gt(dat+s, e-s, "8.0", 3)) { /* qualifies */ }
}
```

### movie_info__movie_id (offsets_only)
- `<storage>/_idx/movie_info__movie_id__offsets.bin`
- Used to confirm a Drama/Horror genres row exists for mid.

### movie_companies__movie_id (offsets_only)
- `<storage>/_idx/movie_companies__movie_id__offsets.bin`
- Find mc row with matching company_type_id, then look up company_name dict.

Alternative indexes (CSRs) and when to consider:
- `movie_info_idx__info_type_id` CSR: could be used to enumerate all mi_idx rows of type rating, then `info > '8.0'`, then join to title. With only 4 production years selected, the title-driven plan is tighter — skip.
- `movie_info__info_type_id` CSR: similarly skipped.
- `movie_companies__company_id` CSR: skipped — we drive from movies, not companies.
