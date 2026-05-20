## SQL

```sql
SELECT MIN(cn.name) AS from_company,
       MIN(lt.link) AS movie_link_type,
       MIN(t.title) AS sequel_movie
FROM company_name AS cn, company_type AS ct, keyword AS k, link_type AS lt,
     movie_companies AS mc, movie_keyword AS mk, movie_link AS ml, title AS t
WHERE cn.country_code != '[pl]'
  AND (cn.name LIKE '%Film%' OR cn.name LIKE '%Warner%')
  AND ct.kind = 'production companies'
  AND k.keyword = 'sequel'
  AND lt.link LIKE '%follows%'
  AND mc.note IS NULL
  AND t.production_year = 1998
  AND t.title LIKE '%Money%'
  AND lt.id = ml.link_type_id AND ml.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_type_id = ct.id
  AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Code 0 = NULL. Resolve `[pl]` at runtime; predicate is `cc != pl_code && cc != 0`.

### company_name.name (varlen)
Files: `company_name/name.off`, `name.dat`. Two-pattern `%Film%` / `%Warner%` via `memmem`.

### company_name.id (int32, dense PK)
Identity file; row = id-1.

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat` (4 rows). Resolve `production companies` → `ct_id`.

### keyword.keyword (varlen)
Files: `keyword/keyword.off|.dat`. Resolve `sequel` → `k_id`.

### link_type.link (varlen)
Files: `link_type/link.off|.dat` (18 rows). Scan for `%follows%` (substring); collect `LT` set.

### movie_companies (movie_id/company_id/company_type_id/note)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`, `note.off|.dat`. Sorted by movie_id.

### movie_keyword (movie_id/keyword_id)
Files: `movie_keyword/movie_id.bin`, `keyword_id.bin`. Sorted by movie_id.

### movie_link (movie_id/link_type_id)
Files: `movie_link/movie_id.bin`, `link_type_id.bin`. 29997 rows, sorted by movie_id.

### title (id/title/production_year)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/production_year.bin` (int32, NULL=INT32_MIN). Predicate `= 1998` is exact int32 equality.

## Table Stats

| Table | Rows | Sort |
|---|---|---|
| company_name | 234997 | id |
| company_type | 4 | id |
| keyword | 134170 | id |
| link_type | 18 | id |
| movie_companies | 2609129 | movie_id |
| movie_keyword | 4523930 | movie_id |
| movie_link | 29997 | movie_id |
| title | 2528312 | id |

## Query Analysis

Join graph: `cn → mc ← t → mk → k`, `t → ml → lt`. Title is central; everything joins on movie_id = t.id.

Driver choice: `production_year = 1998 AND title LIKE '%Money%'` is extremely selective on title (~a few hundred rows). Plan:

1. Resolve dim ids at startup: `pl_code`, `ct_id`, `k_id`, `LT` (link_type ids matching `%follows%`).
2. Scan title (2.5M rows) once: keep `mid = i+1` where `production_year[i] == 1998` AND `memmem(name_dat + off[i], len, "Money", 5)` matches. → small candidate set `M`.
3. For each `mid ∈ M`:
   - Probe `movie_link__movie_id` offsets; require ≥1 row with `link_type_id ∈ LT`. Capture its `link` for MIN.
   - Probe `movie_keyword__movie_id` offsets; require ≥1 row with `keyword_id == k_id`.
   - Probe `movie_companies__movie_id` offsets; iterate `mc` rows, require `note` empty, `company_type_id == ct_id`, then look up `company_name` by `company_id`: check `cc != pl_code && cc != 0` and name LIKE `%Film%`/`%Warner%`.
4. Update MIN aggregates over qualifying tuples.

MIN aggregation: maintain best (`cn.name`, `lt.link`, `t.title`) as `string_view`s, update with byte-wise `<`.

LIKE notes: `%Money%` is 5 bytes — fast `memmem`. `%follows%` runs over 18 link_type rows once. `%Film%`/`%Warner%` runs only over visited company_name rows (small).

Selectivity: `t.production_year=1998` keeps ~1–2% of title (~25k rows); `%Money%` further cuts to hundreds; remaining joins are point lookups via offsets indexes.

## Indexes

### movie_link__movie_id (offsets_only)
- `<storage>/_idx/movie_link__movie_id__offsets.bin` (int32, 2528314 entries)

```cpp
int32_t lo = ml_off[mid], hi = ml_off[mid + 1];
for (int32_t r = lo; r < hi; ++r) {
    int32_t ltid = ml_link_type_id[r];
    if (LT.contains(ltid)) { /* lt.link from link_type row ltid-1 */ }
}
```

### movie_keyword__movie_id (offsets_only)
- `<storage>/_idx/movie_keyword__movie_id__offsets.bin`
- `[lo,hi)` scan over `keyword_id == k_id`.

### movie_companies__movie_id (offsets_only)
- `<storage>/_idx/movie_companies__movie_id__offsets.bin`
- Apply filter `note empty && company_type_id == ct_id`; load `company_id` and check cn predicates.

No need for `movie_companies__company_id` CSR here — title-driven plan is tighter.
