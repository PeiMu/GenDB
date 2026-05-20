## SQL

```sql
SELECT MIN(mi.info) AS release_date,
       MIN(miidx.info) AS rating,
       MIN(t.title) AS german_movie
FROM company_name AS cn, company_type AS ct,
     info_type AS it, info_type AS it2,
     kind_type AS kt,
     movie_companies AS mc, movie_info AS mi, movie_info_idx AS miidx, title AS t
WHERE cn.country_code = '[de]'
  AND ct.kind = 'production companies'
  AND it.info = 'rating'
  AND it2.info = 'release dates'
  AND kt.kind = 'movie'
  AND mi.movie_id = t.id AND it2.id = mi.info_type_id
  AND kt.id = t.kind_id
  AND mc.movie_id = t.id AND cn.id = mc.company_id AND ct.id = mc.company_type_id
  AND miidx.movie_id = t.id AND it.id = miidx.info_type_id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `[de]` → `de_code`. Predicate `cc == de_code`.

### company_name.id (dense PK)
Identity. `company_name/name.off|.dat` not used (no MIN over cn.name in Q13a).

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat`. Resolve `production companies` → `ct_id`.

### info_type.info (varlen)
Files: `info_type/info.off|.dat` (113 rows). Resolve two values: `it_id` for `rating` (used by miidx); `it2_id` for `release dates` (used by mi).

### kind_type.kind (varlen)
Files: `kind_type/kind.off|.dat` (7 rows). Resolve `movie` → `kt_id`.

### movie_companies (movie_id/company_id/company_type_id)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`.

### movie_info (movie_id/info_type_id/info)
Files: `movie_info/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Used for `info_type_id == it2_id` and for MIN(mi.info).

### movie_info_idx (movie_id/info_type_id/info)
Files: `movie_info_idx/movie_id.bin`, `info_type_id.bin`, `info.off|.dat`. Used for `info_type_id == it_id` and MIN(miidx.info).

### title (id/title/kind_id/production_year unused)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/kind_id.bin` (int32). Predicate `t.kind_id == kt_id`. No production_year filter in Q13a.

## Table Stats

| Table | Rows | Sort |
|---|---|---|
| company_name | 234997 | id |
| company_type | 4 | id |
| info_type | 113 | id |
| kind_type | 7 | id |
| movie_companies | 2609129 | movie_id |
| movie_info | 14835720 | movie_id |
| movie_info_idx | 1380035 | movie_id |
| title | 2528312 | id |

## Query Analysis

Join graph: title hub. mc, mi, miidx all join via t.id. kind_type filters title.kind_id. info_type used twice (it, it2) — separate aliases.

Driver options:
- (A) `company_name.country_code == de_code`: scan 234997 int16 → produce de_company_ids (a few thousand). Then CSR `movie_companies__company_id` to expand to mc rows; filter `company_type_id == ct_id`; collect mids.
- (B) `title__kind_id` CSR with kt_id (movie) — enumerates all movie-kind title rows. Useful but very large (most titles are kind=movie).
- (C) `movie_info_idx__info_type_id` with it_id (rating) — many rows.

Best: (A) drives from companies → mids, then for each mid verify kind, miidx, mi.

Plan:

1. Resolve dims: `de_code`, `ct_id`, `it_id` (rating), `it2_id` (release dates), `kt_id` (movie).
2. Scan `company_name.country_code` → `cn_de` = ids where `cc == de_code`.
3. For each `cn_id ∈ cn_de`, use CSR `movie_companies__company_id`: enumerate mc rows; require `company_type_id == ct_id`; collect mids (with mc_row).
4. For each `mid`:
   - title row mid-1: require `kind_id == kt_id`.
   - Probe `movie_info_idx__movie_id` `[lo,hi)`: scan with `info_type_id == it_id`; capture miidx.info for MIN.
   - Probe `movie_info__movie_id` `[lo,hi)`: scan with `info_type_id == it2_id`; capture mi.info for MIN.
5. Update MIN(mi.info), MIN(miidx.info), MIN(t.title).

MIN aggregation: byte-wise. mi.info is the release_date string; miidx.info is the rating string. Each qualifying mid emits potentially multiple mi/miidx rows.

LIKE notes: none in Q13a.

Selectivities: `[de]` company_names → few thousand cn_ids; `ct == production companies` → ~25% of mc rows under those ids; kind=movie keeps most titles. Strong funnel through the cn_id list.

## Indexes

### movie_companies__company_id (CSR)
- `<storage>/_idx/movie_companies__company_id__offsets.bin` (int32, 234999)
- `<storage>/_idx/movie_companies__company_id__rowids.bin` (int32, 2609129)

```cpp
for (int32_t cn_id : cn_de) {
    int32_t lo = mc_cid_off[cn_id], hi = mc_cid_off[cn_id+1];
    for (int32_t k = lo; k < hi; ++k) {
        int32_t mc_row = mc_cid_rowids[k];
        if (mc_company_type_id[mc_row] != ct_id) continue;
        int32_t mid = mc_movie_id[mc_row];
        if (t_kind_id[mid-1] != kt_id) continue;
        /* probe mi, miidx */
    }
}
```

### movie_info__movie_id (offsets_only)
- `<storage>/_idx/movie_info__movie_id__offsets.bin`
- Filter rows by `info_type_id == it2_id` (release dates).

### movie_info_idx__movie_id (offsets_only)
- `<storage>/_idx/movie_info_idx__movie_id__offsets.bin`
- Filter rows by `info_type_id == it_id` (rating).

Skipped: `title__kind_id` CSR (kind=movie is too wide); `movie_info__info_type_id` / `movie_info_idx__info_type_id` CSR (driving from companies is tighter).
