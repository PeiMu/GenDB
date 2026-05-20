## SQL

```sql
SELECT MIN(t.title) AS complete_downey_ironman_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     char_name AS chn, cast_info AS ci, keyword AS k, kind_type AS kt,
     movie_keyword AS mk, name AS n, title AS t
WHERE cct1.kind='cast'
  AND cct2.kind LIKE '%complete%'
  AND chn.name NOT LIKE '%Sherlock%'
  AND (chn.name LIKE '%Tony%Stark%' OR chn.name LIKE '%Iron%Man%')
  AND k.keyword IN ('superhero','sequel','second-part','marvel-comics','based-on-comic','tv-special','fight','violence')
  AND kt.kind='movie'
  AND n.name LIKE '%Downey%Robert%'
  AND t.production_year>2000
  AND kt.id=t.kind_id AND t.id=mk.movie_id AND t.id=ci.movie_id AND t.id=cc.movie_id
  AND chn.id=ci.person_role_id AND n.id=ci.person_id
  AND k.id=mk.keyword_id
  AND cct1.id=cc.subject_id AND cct2.id=cc.status_id;
```

## Column Reference

Same shape as Q20a. Differences: adds `n.name LIKE '%Downey%Robert%'` and tightens `production_year > 2000`.

### comp_cast_type.kind (varlen, 4 rows)
Resolve `cct1_id` ('cast') and `cct2_ids` (subset whose kind LIKE `%complete%`). See Q20a snippet.

### kind_type.kind, keyword.keyword
Resolve `kt_movie`, `K_ids` (8 keyword ids).

### char_name.name (varlen, 3140339)
LIKE filters identical to Q20a.

### name.name (varlen, 4167491)
LIKE `%Downey%Robert%` — two-pointer `memmem("Downey")` then `memmem("Robert")` in tail. Likely tens of rows. Build dense set `N_ids` (`vector<bool>` of size 4167491+1).

### cast_info.person_id, movie_id, person_role_id
Files: `cast_info/person_id.bin`, `.../movie_id.bin`, `.../person_role_id.bin`. ci sorted by movie_id; CSR on person_id available.

### complete_cast.movie_id, subject_id, status_id
135086 rows; sorted by movie_id.

### movie_keyword.movie_id, keyword_id
4523930 rows; sorted by movie_id.

### title.id, title, production_year, kind_id
`production_year > 2000 AND != INT32_MIN`; `kind_id == kt_movie`.

## Table Stats

Same as Q20a.

| Table | Rows | Sort | Notes |
|---|---|---|---|
| comp_cast_type | 4 | id | dense-PK |
| kind_type | 7 | id | dense-PK |
| keyword | 134,170 | id | dense-PK |
| char_name | 3,140,339 | id | dense-PK |
| name | 4,167,491 | id | dense-PK |
| complete_cast | 135,086 | movie_id | offsets_only movie_id; CSR subject_id/status_id |
| cast_info | 36,244,344 | movie_id | offsets_only movie_id; CSR person_id/person_role_id |
| movie_keyword | 4,523,930 | movie_id | offsets_only movie_id; CSR keyword_id |
| title | 2,528,312 | id | dense-PK; CSR kind_id |

## Query Analysis

Join graph same as Q20a, plus name filter on the actor side.

`n.name LIKE '%Downey%Robert%'` is extremely selective (handful of rows, probably 1-2). This is the best driver.

Plan:
1. Resolve `cct1_id`, `cct2_ids`, `kt_movie`, `K_ids`.
2. Scan name.name → small `N_ids` (likely {Downey Jr., Robert; etc.}).
3. Build candidate `CHN`: scan char_name.name with Tony/Iron AND NOT Sherlock.
4. For each `pid` in `N_ids`: walk `cast_info__person_id` CSR; for each ci row check `person_role_id ∈ CHN`. Collect `mv=movie_id[r]` into `M`.
5. For each `mv` in `M`:
   - Check `title.kind_id[mv-1]==kt_movie` and `production_year[mv-1] > 2000 AND != INT32_MIN`.
   - Check `complete_cast__movie_id` range non-empty with `subject_id==cct1_id` AND `status_id ∈ cct2_ids`.
   - Check `movie_keyword__movie_id` range — at least one row with `keyword_id ∈ K_ids` (small set; use unordered_set<int32>).
   - Update MIN(title[mv-1]).

Selectivities:
- n.name `%Downey%Robert%` → tiny (1-3 ids).
- CHN intersect Tony/Iron NOT Sherlock → small.
- After ci join, mv set is tiny.
- All downstream probes verify cheaply.

LIKE notes:
- `%Downey%Robert%`: two-pointer (Downey, then Robert after it).
- `%Tony%Stark%`, `%Iron%Man%`: same pattern.
- `%Sherlock%`: simple `memmem` for NOT.

## Indexes

### cast_info__person_id (CSR)
Files: `_idx/cast_info__person_id__{offsets,rowids}.bin`. Drive from N_ids.
```cpp
auto ci_off = read_vec<int32_t>(store + "/_idx/cast_info__person_id__offsets.bin");
auto ci_row = read_vec<int32_t>(store + "/_idx/cast_info__person_id__rowids.bin");
int32_t lo=ci_off[pid], hi=ci_off[pid+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r=ci_row[k]; int32_t mv=ci_mid[r]; int32_t prid=ci_prid[r]; }
```

### complete_cast__movie_id (offsets_only)
File: `_idx/complete_cast__movie_id__offsets.bin`.

### movie_keyword__movie_id (offsets_only)
File: `_idx/movie_keyword__movie_id__offsets.bin`.

### movie_keyword__keyword_id (CSR) — alt
Drive from K_ids if N_ids ends up large.

### cast_info__person_role_id (CSR) — alt
Drive from CHN set if smaller than N_ids × matches.

### complete_cast__subject_id, complete_cast__status_id (CSR) — small, only 4 keys
Useful but cc is only 135K, full scan is also fast.
