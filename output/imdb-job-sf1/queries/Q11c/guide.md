## SQL

```sql
SELECT MIN(cn.name) AS from_company,
       MIN(mc.note) AS production_note,
       MIN(t.title) AS movie_based_on_book
FROM company_name AS cn, company_type AS ct, keyword AS k, link_type AS lt,
     movie_companies AS mc, movie_keyword AS mk, movie_link AS ml, title AS t
WHERE cn.country_code != '[pl]'
  AND (cn.name LIKE '20th Century Fox%' OR cn.name LIKE 'Twentieth Century Fox%')
  AND ct.kind != 'production companies' AND ct.kind IS NOT NULL
  AND k.keyword IN ('sequel','revenge','based-on-novel')
  AND mc.note IS NOT NULL
  AND t.production_year > 1950
  AND lt.id = ml.link_type_id AND ml.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_type_id = ct.id
  AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin`, `.dict.off`, `.dict.dat`. Resolve `[pl]` → `pl_code`. Predicate `cc != pl_code && cc != 0`.

### company_name.name (varlen)
Files: `company_name/name.off|.dat`. Two LIKE prefixes `'20th Century Fox%'` and `'Twentieth Century Fox%'` — anchored prefix, use `len >= prefix_len && memcmp(dat+off[i], prefix, prefix_len) == 0`.

### company_name.id (dense PK)
Identity; row = id-1.

### company_type.kind (varlen)
Files: `company_type/kind.off|.dat` (4 rows). Resolve `production companies` → `pc_id`; allowed `ct_id ∈ {1..4} \ {pc_id}` and skip empty-string rows (none expected; 4 rows are all valued).

### keyword.keyword (varlen)
Files: `keyword/keyword.off|.dat`. Scan once and collect ids for `sequel`, `revenge`, `based-on-novel` → set `K` (≤3 ids).

### link_type
Not filtered by predicate; only join `lt.id = ml.link_type_id`. `lt` join is effectively unconstrained (any non-NULL link_type_id passes). Output not required from lt.

### movie_companies (movie_id/company_id/company_type_id/note)
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin`, `note.off|.dat`. `note IS NOT NULL` ↔ `off[i] != off[i+1]`.

### movie_keyword (movie_id/keyword_id)
Files: `movie_keyword/movie_id.bin`, `keyword_id.bin`.

### movie_link (movie_id/link_type_id)
Files: `movie_link/movie_id.bin`, `link_type_id.bin`. Predicate only requires existence of a row (`link_type_id` not NULL).

### title (id/title/production_year)
Files: `title/id.bin` identity, `title/title.off|.dat`, `title/production_year.bin`. `> 1950` requires non-NULL: `py[i] != INT32_MIN && py[i] > 1950`.

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

Join graph: `cn → mc ← t → mk → k`, `t → ml`. lt join is structural.

Driver: company_name prefix `20th Century Fox%` / `Twentieth Century Fox%` is extremely selective (likely <20 cn_ids). Plan:

1. Resolve dims: `pl_code`, `pc_id` (excluded), `K` (set of ≤3 keyword ids).
2. Scan `company_name`: prefix-match names; check `cc != pl_code && cc != 0`. Collect `cn_ids`.
3. For each `cn_id`, expand via CSR `movie_companies__company_id`. Per mc row: require `note` non-empty, `company_type_id != pc_id && company_type_id != 0`. Capture mid and mc_row (for `mc.note` MIN).
4. For each mid:
   - title row index = mid-1: check `production_year > 1950`.
   - Probe `movie_keyword__movie_id` `[lo,hi)`, require some row with `keyword_id ∈ K`.
   - Probe `movie_link__movie_id` `[lo,hi)`, require non-empty (any row qualifies).
5. Update MIN(`cn.name`), MIN(`mc.note`), MIN(`t.title`).

MIN aggregation: byte-wise smallest. `mc.note` is per-row varlen; only updated when full predicate holds.

LIKE notes: prefix LIKE → no wildcard scan; direct `memcmp`. Combined prefix `Twentieth Century Fox` is 21 bytes; quick reject by length.

Selectivity: prefix match ≪ 1% of company_name; `mc.note IS NOT NULL` keeps ~40%; `ct.kind != 'production companies'` keeps 3/4 of company_type ids; production_year>1950 keeps most of title.

## Indexes

### movie_companies__company_id (CSR)
- `<storage>/_idx/movie_companies__company_id__offsets.bin` (int32, 234999)
- `<storage>/_idx/movie_companies__company_id__rowids.bin` (int32, 2609129)

```cpp
int32_t lo = off[cn_id], hi = off[cn_id+1];
for (int32_t k = lo; k < hi; ++k) {
    int32_t mc_row = rowids[k];
    if (mc_note_off[mc_row] == mc_note_off[mc_row+1]) continue;  // require NOT NULL
    int32_t ctid = mc_company_type_id[mc_row];
    if (ctid == 0 || ctid == pc_id) continue;
    int32_t mid = mc_movie_id[mc_row];
    /* probe title, mk, ml */
}
```

### movie_keyword__movie_id (offsets_only)
- `<storage>/_idx/movie_keyword__movie_id__offsets.bin`. Scan keyword_id in `K`.

### movie_link__movie_id (offsets_only)
- `<storage>/_idx/movie_link__movie_id__offsets.bin`. Existence probe: `hi > lo`.

No use of `movie_keyword__keyword_id` CSR (driver from cn is tighter).
