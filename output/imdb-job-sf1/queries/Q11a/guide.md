## SQL

```sql
SELECT MIN(cn.name) AS from_company,
       MIN(lt.link) AS movie_link_type,
       MIN(t.title) AS non_polish_sequel_movie
FROM company_name AS cn, company_type AS ct, keyword AS k, link_type AS lt,
     movie_companies AS mc, movie_keyword AS mk, movie_link AS ml, title AS t
WHERE cn.country_code != '[pl]'
  AND (cn.name LIKE '%Film%' OR cn.name LIKE '%Warner%')
  AND ct.kind = 'production companies'
  AND k.keyword = 'sequel'
  AND lt.link LIKE '%follow%'
  AND mc.note IS NULL
  AND t.production_year BETWEEN 1950 AND 2000
  AND lt.id = ml.link_type_id AND ml.movie_id = t.id
  AND t.id = mk.movie_id AND mk.keyword_id = k.id
  AND t.id = mc.movie_id AND mc.company_type_id = ct.id
  AND mc.company_id = cn.id;
```

## Column Reference

### company_name.country_code (int16 dict)
Files: `company_name/country_code.bin` (int16, 234997 rows), `company_name/country_code.dict.off` (int64 K+1), `company_name/country_code.dict.dat`. Code `0` = NULL. Resolve `[pl]` at runtime by scanning the dict; reject `cc[i] == pl_code` OR `cc[i] == 0`.

### company_name.name (varlen)
Files: `company_name/name.off`, `company_name/name.dat`. Two-pattern LIKE `%Film%` / `%Warner%` — full byte scan with `memmem` per row.

### company_name.id (int32, dense PK)
Row index = id - 1. File `company_name/id.bin` is identity.

### company_type.kind (varlen)
Files: `company_type/kind.off`, `company_type/kind.dat` (4 rows). Linear scan to resolve `production companies` → `ct_id = i + 1`.

### keyword.keyword (varlen)
Files: `keyword/keyword.off`, `keyword/keyword.dat` (134170 rows). Linear scan for literal `sequel` → `k_id`.

### link_type.link (varlen)
Files: `link_type/link.off`, `link_type/link.dat` (18 rows). Scan with `memmem` for `%follow%`; collect all matching ids into a small set `LT`.

### movie_companies.movie_id, company_id, company_type_id, note
Files: `movie_companies/movie_id.bin`, `company_id.bin`, `company_type_id.bin` (all int32, 2609129 rows), `movie_companies/note.off|.dat`. `note IS NULL` ↔ `off[i] == off[i+1]`.

### movie_keyword.movie_id, keyword_id
Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin` (int32, 4523930 rows). Sorted by `movie_id`.

### movie_link.movie_id, link_type_id
Files: `movie_link/movie_id.bin`, `movie_link/link_type_id.bin` (int32, 29997 rows). Sorted by `movie_id`.

### title.id, title, production_year
`title/id.bin` identity (2528312 rows). `title/title.off|.dat`. `title/production_year.bin` int32, NULL = `INT32_MIN`.

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

Join graph: `cn → mc ← t → mk → k`, `t → ml → lt`. All movie-side tables join through `t.id = *.movie_id`.

Driver: `movie_link` (29997 rows) is the tightest funnel after filtering `lt.link LIKE '%follow%'` (handful of link_type ids). Alternative: `keyword.keyword = 'sequel'` resolves to 1 keyword_id; then `movie_keyword__keyword_id` CSR yields a moderate set of movie ids. Best plan:

1. Resolve dict / dimension ids: `pl_code`, `ct_id`, `k_id`, `LT` (set of link_type_ids matching `%follow%`).
2. Scan `company_name`: prefilter rows where `cc != pl_code && cc != 0` AND name LIKE `%Film%`/`%Warner%`. Collect `cn_ids`.
3. For each `cn_id`, use CSR `movie_companies__company_id` to get candidate `mc` rows. Apply `note IS NULL` (off[i]==off[i+1]) and `company_type_id == ct_id`. Collect `mc_movie_ids` (dedup or per-row).
4. For each candidate `movie_id`: check `title.production_year` (row `mid-1`) in `[1950, 2000]` (skip `INT32_MIN`).
5. Verify `mk` link: probe `movie_keyword__movie_id` offsets index; scan `keyword_id == k_id` in `[lo, hi)`.
6. Verify `ml` link: probe `movie_link__movie_id` offsets index; scan rows whose `link_type_id ∈ LT`.
7. Aggregate `MIN(cn.name)`, `MIN(lt.link)`, `MIN(t.title)` over qualifying tuples (each MIN over a varlen — track current best lexicographic bytes).

MIN aggregation: maintain three `std::string_view` best-so-far values; update only when a shorter-or-lex-smaller candidate is found. Compare via `memcmp` on length-prefixed bytes.

LIKE notes: `cn.name` is unanchored `%X%` — `memmem` scan over `dat[off[i] .. off[i+1])`. Prune by `len < 4` for `%Film%` and `len < 6` for `%Warner%`. `lt.link LIKE '%follow%'` is a one-shot 18-row scan at startup.

Selectivities (rough): `cn.country_code != '[pl]'` keeps ~99% of company_name; the LIKE narrows to a few thousand cn_ids; `mc.note IS NULL` keeps ~60%; production_year window prunes heavily.

## Indexes

### movie_companies__company_id (CSR)
- `<storage>/_idx/movie_companies__company_id__offsets.bin` (int32, 234999 entries)
- `<storage>/_idx/movie_companies__company_id__rowids.bin` (int32, 2609129 entries)
- Use to expand each filtered `cn_id` → list of mc rows in O(deg).

```cpp
int32_t lo = off[cn_id], hi = off[cn_id + 1];
for (int32_t k = lo; k < hi; ++k) {
    int32_t mc_row = rowids[k];
    if (mc_company_type_id[mc_row] != ct_id) continue;
    if (mc_note_off[mc_row] != mc_note_off[mc_row+1]) continue;  // note IS NULL
    int32_t mid = mc_movie_id[mc_row];
    /* probe title, mk, ml */
}
```

### movie_keyword__movie_id (offsets_only)
- `<storage>/_idx/movie_keyword__movie_id__offsets.bin` (int32, 2528314 entries)
- For each candidate `mid`, `[off[mid], off[mid+1])` lists matching mk rows.

### movie_link__movie_id (offsets_only)
- `<storage>/_idx/movie_link__movie_id__offsets.bin` (int32, 2528314 entries)
- Slot 0 holds NULL count; sentinel slot 2528313 is N.

### title row anchor
No index needed for title; row index = `mid - 1`.

Skip: `movie_keyword__keyword_id` CSR would also work as an alternative driver from `k.keyword='sequel'`, but the company-side driver is tighter here.
