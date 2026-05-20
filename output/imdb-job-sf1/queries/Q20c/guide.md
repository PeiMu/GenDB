## SQL

```sql
SELECT MIN(n.name) AS cast_member,
       MIN(t.title) AS complete_dynamic_hero_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     char_name AS chn, cast_info AS ci, keyword AS k, kind_type AS kt,
     movie_keyword AS mk, name AS n, title AS t
WHERE cct1.kind='cast'
  AND cct2.kind LIKE '%complete%'
  AND chn.name IS NOT NULL
  AND (chn.name LIKE '%man%' OR chn.name LIKE '%Man%')
  AND k.keyword IN ('superhero','marvel-comics','based-on-comic','tv-special','fight','violence','magnet','web','claw','laser')
  AND kt.kind='movie'
  AND t.production_year>2000
  AND kt.id=t.kind_id AND t.id=mk.movie_id AND t.id=ci.movie_id AND t.id=cc.movie_id
  AND chn.id=ci.person_role_id AND n.id=ci.person_id
  AND k.id=mk.keyword_id
  AND cct1.id=cc.subject_id AND cct2.id=cc.status_id;
```

## Column Reference

### comp_cast_type.kind
Resolve `cct1_id` and `cct2_ids` (LIKE %complete%) via 4-row scan.

### kind_type.kind, keyword.keyword
Resolve `kt_movie`, `K_ids` (10 keyword ids).

### char_name.name (varlen, 3140339)
LIKE `%man%` OR `%Man%` (case-sensitive). `chn.name IS NOT NULL` ⇔ `name.off[i]!=name.off[i+1]`. Because `%man%` is broad, expect many CHN ids.

### name.name (varlen, 4167491)
No filter on name in Q20c; MIN(n.name) is the aggregate.

### cast_info.person_id, movie_id, person_role_id
Same files as Q20a/b.

### complete_cast.movie_id, subject_id, status_id
135086 rows.

### movie_keyword.movie_id, keyword_id
4523930 rows.

### title.id, title, production_year, kind_id
`production_year > 2000 AND != INT32_MIN`; `kind_id == kt_movie`.

## Table Stats

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

Join graph identical to Q20a/b.

Aggregates: MIN(n.name), MIN(t.title).

`chn.name` LIKE `%man%`/`%Man%` is broad; n.name has no filter. The most selective starting point is complete_cast — only 135K rows, and rows must satisfy `subject_id==cct1_id` AND `status_id ∈ cct2_ids`.

Plan:
1. Resolve `cct1_id`, `cct2_ids`, `kt_movie`, `K_ids`.
2. Scan complete_cast linearly (135K rows). Build set `M_cc = {movie_id[r] : movie_id!=INT32_MIN AND subject_id==cct1_id AND status_id ∈ cct2_ids}`.
3. Intersect M_cc with title filter: keep `mv` where `kind_id[mv-1]==kt_movie` AND `production_year[mv-1] > 2000 AND != INT32_MIN`.
4. Intersect M_cc with movies that have at least one mk row with `keyword_id ∈ K_ids`. (Either pre-build M_keyword by walking `movie_keyword__keyword_id` CSR for each kid in K_ids and union the rowids → mv; or probe `movie_keyword__movie_id` range per mv.) With M_cc small after step 3, prefer per-mv probing.
5. Build dense `CHN` bitset by scanning char_name.name (3.14M) once: `IS NOT NULL` AND (`%man%` OR `%Man%`).
6. For each `mv` in surviving M_cc:
   - Walk `cast_info__movie_id` range. For each ci row r: `prid=person_role_id[r]`; if `prid!=INT32_MIN AND CHN[prid]` then `pid=person_id[r]`; update MIN(n.name) from `name.name[pid-1]`, MIN(t.title) from `title[mv-1]`.

Selectivities:
- complete_cast cast+complete% → few-K rows.
- Joined with year>2000 + kind=movie → smaller.
- mk K_ids existence → many movies but tightly intersected.
- chn `%man%`/`%Man%` → broad; many CHN ids.

LIKE notes:
- `%man%` and `%Man%` are case-sensitive — apply both via `memmem`; OR them.
- `%complete%` on 4-row comp_cast_type at startup.

## Indexes

### complete_cast__movie_id (offsets_only, primary)
File: `_idx/complete_cast__movie_id__offsets.bin` (int32, 2528314).

### complete_cast__subject_id (CSR)
Files: `_idx/complete_cast__subject_id__{offsets,rowids}.bin`. parent_max=4.
```cpp
auto ccs_off = read_vec<int32_t>(store + "/_idx/complete_cast__subject_id__offsets.bin");
auto ccs_row = read_vec<int32_t>(store + "/_idx/complete_cast__subject_id__rowids.bin");
int32_t lo=ccs_off[cct1_id], hi=ccs_off[cct1_id+1];
for (int32_t k=lo; k<hi; ++k) { int32_t r=ccs_row[k]; /* cc row r */ }
```

### complete_cast__status_id (CSR)
Files: `_idx/complete_cast__status_id__{offsets,rowids}.bin`. parent_max=4. Iterate union of `cct2_ids`.

Since cc is only 135K rows, a linear scan is also reasonable and avoids the CSR.

### cast_info__movie_id (offsets_only)
File: `_idx/cast_info__movie_id__offsets.bin`.

### movie_keyword__movie_id (offsets_only) / movie_keyword__keyword_id (CSR)
Files: `_idx/movie_keyword__movie_id__offsets.bin`; `_idx/movie_keyword__keyword_id__{offsets,rowids}.bin`. Pick based on driver direction.

### cast_info__person_role_id (CSR) — alt
Use to drive from CHN set if it ends up small.
