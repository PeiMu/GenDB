# Q24b Guide

## SQL
```sql
SELECT MIN(chn.name) AS voiced_char_name,
       MIN(n.name) AS voicing_actress_name,
       MIN(t.title) AS kung_fu_panda
FROM aka_name AS an,
     char_name AS chn,
     cast_info AS ci,
     company_name AS cn,
     info_type AS it,
     keyword AS k,
     movie_companies AS mc,
     movie_info AS mi,
     movie_keyword AS mk,
     name AS n,
     role_type AS rt,
     title AS t
WHERE ci.note IN ('(voice)', '(voice: Japanese version)',
                  '(voice) (uncredited)', '(voice: English version)')
  AND cn.country_code = '[us]'
  AND cn.name = 'DreamWorks Animation'
  AND it.info = 'release dates'
  AND k.keyword IN ('hero','martial-arts','hand-to-hand-combat',
                    'computer-animated-movie','kung-fu')
  AND mi.info IS NOT NULL
  AND (mi.info LIKE 'Japan:%201%' OR mi.info LIKE 'USA:%201%')
  AND n.gender = 'f'
  AND n.name LIKE '%An%'
  AND rt.role = 'actress'
  AND t.production_year > 2010
  AND t.title LIKE 'Kung Fu Panda%'
  AND t.id = mi.movie_id
  AND t.id = mc.movie_id
  AND t.id = ci.movie_id
  AND t.id = mk.movie_id
  AND mc.movie_id = ci.movie_id
  AND mc.movie_id = mi.movie_id
  AND mc.movie_id = mk.movie_id
  AND mi.movie_id = ci.movie_id
  AND mi.movie_id = mk.movie_id
  AND ci.movie_id = mk.movie_id
  AND cn.id = mc.company_id
  AND it.id = mi.info_type_id
  AND n.id = ci.person_id
  AND rt.id = ci.role_id
  AND n.id = an.person_id
  AND ci.person_id = an.person_id
  AND chn.id = ci.person_role_id
  AND k.id = mk.keyword_id;
```
Differs from Q24a: `cn.name = 'DreamWorks Animation'` added; keyword IN expanded to 5; mi.info LIKE uses `'201%'`; production_year > 2010; `t.title LIKE 'Kung Fu Panda%'`.

## Column Reference

### aka_name.person_id
- File: `aka_name/person_id.bin` (rows = 901343); FK-sorted.
- Use: offsets_only `aka_name__person_id` for existence per person.

### char_name.id, .name
- Files: `char_name/id.bin` (rows = 3140339), `name.off|.dat`
- Use: identity id; project `MIN(chn.name)`.

### cast_info.movie_id, .person_id, .person_role_id, .role_id, .note
- Files: `cast_info/{movie_id,person_id,person_role_id,role_id}.bin`, `note.off|.dat`
- Use: offsets_only `cast_info__movie_id`; filter `role_id == rt_id`, `valid_persons[person_id-1]`, `note ∈ voice-set`, `person_role_id != INT32_MIN`.

### company_name.id, .name, .country_code
- Files: `company_name/id.bin`, `name.off|.dat`, `country_code.bin|.dict.off|.dict.dat`
- Use: resolve `us_code`. Pre-pass: scan `company_name.name` for the literal `'DreamWorks Animation'` → `target_cn_id` (single id). Then filter `country_code[target_cn_id-1] == us_code` (must hold). The mc filter reduces to `mc.company_id == target_cn_id`.

### info_type.id, .info
- Files: `info_type/id.bin`, `info.off|.dat`
- Use: scan → `it_id` for `'release dates'`.

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword.off|.dat`
- Use: scan for 5 literals → `kw_ids` (5 entries).

### movie_companies.movie_id, .company_id
- Files: `movie_companies/{movie_id,company_id}.bin`
- Use: offsets_only by movie_id; filter `company_id == target_cn_id`.

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat`
- Use: filter `info_type_id == it_id`. Then info length>0 AND (LIKE `'Japan:%201%'` OR LIKE `'USA:%201%'`): memcmp prefix + memmem `"201"`.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin`
- Use: offsets_only by movie_id; filter `kw_ids.contains(keyword_id)`.

### name.id, .name, .gender
- Files: `name/id.bin`, `name.off|.dat`, `gender.bin|.dict.off|.dict.dat`
- Use: resolve `f_code`; pre-pass to build `valid_persons` bitset for `gender == f_code AND name LIKE '%An%'`. Project `MIN(n.name)`.

### role_type.id, .role
- Files: `role_type/id.bin`, `role.off|.dat` (rows = 12)
- Use: scan → `rt_id` for `'actress'`.

### title.id, .title, .production_year
- Files: `title/id.bin`, `title.off|.dat`, `production_year.bin`
- Use: driver. Two text/int filters: `production_year > 2010` AND `t.title LIKE 'Kung Fu Panda%'`. The title prefix is 13 bytes — use memcmp on `[off[v-1], off[v-1]+13)` (after length check). Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| role_type | 12 | dim | id | — |
| info_type | 113 | dim | id | — |
| keyword | 134,170 | dim | id | 100000 |
| company_name | 234,997 | dim | id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |
| aka_name | 901,343 | fact | person_id | 100000 |
| name | 4,167,491 | dim/fact | id | 100000 |
| char_name | 3,140,339 | dim | id | 100000 |
| cast_info | 36,244,344 | fact | movie_id | 200000 |

## Query Analysis
- Resolve before driver: `us_code`, `target_cn_id` (DreamWorks Animation), `it_id`, `kw_ids` (5), `rt_id` ('actress'), `f_code`. Pre-pass `valid_persons` bitset.
- Driver: title v=1..2528312. Apply prefix filter `title LIKE 'Kung Fu Panda%'` first (very selective via 13-byte memcmp on `title.off|.dat`), then `production_year > 2010`. This reduces driver to a handful of rows before any fact lookup.
- Per-survivor: mc range filtered by `company_id == target_cn_id` (very selective — single company id); mk by kw_ids; mi by it_id + info LIKE `'201%'`; ci by rt_id, valid_persons, voice-note set, person_role_id non-NULL.
- aka_name: existence via `aka_name__person_id` offsets_only.
- Selectivities: title prefix dominates and bounds the work; the rest mostly filters down to one or two combos.
- MIN aggregation: running min for chn.name, n.name, t.title.
- LIKE: `t.title LIKE 'Kung Fu Panda%'` → prefix memcmp; `n.name LIKE '%An%'` → memmem in pre-pass; `mi.info` LIKE → prefix memcmp + memmem `"201"`.

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin` (int32, 2528314).
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.
- `aka_name__person_id` (offsets_only): `_idx/aka_name__person_id__offsets.bin` (int32, 4167493).

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- chn fetched by direct index `chn.name[ci.person_role_id - 1]` via dense PK.
- No invented indexes on title/note/info text.
