/* vxrt — VX 后端内置最小运行时 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

void out(const char *s) { fputs(s, stdout); }
void out_int(int v) { printf("%d", v); }
void out_bool(int v) { fputs(v ? "true" : "false", stdout); }
void out_float(double v) { printf("%g", v); }

const char *vx_str_concat(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *r = (char *)malloc(la + lb + 1);
  if (!r) return a;
  memcpy(r, a, la);
  memcpy(r + la, b, lb);
  r[la + lb] = '\0';
  return r;
}

/* 整数快速幂（VX ^ 运算符）：负指数按 0 处理 */
int32_t vx_pow_i32(int32_t base, int32_t exp) {
  if (exp < 0) return 0;
  int32_t r = 1;
  while (exp > 0) {
    if (exp & 1) r *= base;
    base *= base;
    exp >>= 1;
  }
  return r;
}

/* P4: 动态数组（元素统一为 8 字节槽） */
void *vx_array_new(int64_t n) {
  if (n < 0) n = 0;
  int64_t *p = (int64_t *)malloc((n + 1) * 8);
  if (!p) return 0;
  p[0] = n;
  for (int64_t i = 1; i <= n; i++) p[i] = 0;
  return (void *)p;
}
int64_t vx_array_len(void *a) {
  if (!a) return 0;
  return ((int64_t *)a)[0];
}
int64_t vx_array_get(void *a, int64_t i) {
  if (!a) return 0;
  int64_t *p = (int64_t *)a;
  int64_t n = p[0];
  if (i < 0 || i >= n) return 0;
  return p[i + 1];
}
void vx_array_set(void *a, int64_t i, int64_t v) {
  if (!a) return;
  int64_t *p = (int64_t *)a;
  int64_t n = p[0];
  if (i < 0 || i >= n) return;
  p[i + 1] = v;
}

/* P4: 简单字符串/整数键值映射（开放寻址，容量 2n+1，n>=1） */
typedef struct { int64_t key; int64_t is_str; char *keystr; int64_t val; int used; } VXMapEntry;
typedef struct { int64_t cap; int64_t size; VXMapEntry *entries; } VXMap;
void *vx_map_new(int64_t n) {
  if (n < 1) n = 1;
  int64_t cap = n * 2 + 1;
  VXMap *m = (VXMap *)malloc(sizeof(VXMap));
  if (!m) return 0;
  m->cap = cap;
  m->size = 0;
  m->entries = (VXMapEntry *)calloc((size_t)cap, sizeof(VXMapEntry));
  if (!m->entries) { free(m); return 0; }
  return (void *)m;
}
static int64_t vx_map_hash(int64_t key, const char *ks, int64_t is_str) {
  uint64_t h;
  if (is_str && ks) {
    h = 5381;
    for (const char *p = ks; *p; p++) h = h * 33 + (unsigned char)*p;
  } else {
    h = (uint64_t)key * 0x9e3779b97f4a7c15ULL;
  }
  return (int64_t)h;
}
void vx_map_set(void *m0, int64_t key, int64_t is_str, const char *keystr, int64_t val) {
  if (!m0) return;
  VXMap *m = (VXMap *)m0;
  int64_t idx = vx_map_hash(key, keystr, is_str) % m->cap;
  if (idx < 0) idx += m->cap;
  for (int64_t i = 0; i < m->cap; i++) {
    int64_t j = (idx + i) % m->cap;
    VXMapEntry *e = &m->entries[j];
    if (!e->used) {
      e->used = 1; e->key = key; e->is_str = is_str; e->keystr = keystr ? strdup(keystr) : 0; e->val = val; m->size++; return;
    }
    int match = 0;
    if (e->is_str && is_str) {
      match = (e->keystr && keystr && strcmp(e->keystr, keystr) == 0);
    } else if (!e->is_str && !is_str) {
      match = (e->key == key);
    }
    if (match) { e->val = val; return; }
  }
}
int64_t vx_map_get(void *m0, int64_t key, int64_t is_str, const char *keystr) {
  if (!m0) return 0;
  VXMap *m = (VXMap *)m0;
  int64_t idx = vx_map_hash(key, keystr, is_str) % m->cap;
  if (idx < 0) idx += m->cap;
  for (int64_t i = 0; i < m->cap; i++) {
    int64_t j = (idx + i) % m->cap;
    VXMapEntry *e = &m->entries[j];
    if (!e->used) return 0;
    int match = 0;
    if (e->is_str && is_str) {
      match = (e->keystr && keystr && strcmp(e->keystr, keystr) == 0);
    } else if (!e->is_str && !is_str) {
      match = (e->key == key);
    }
    if (match) return e->val;
  }
  return 0;
}
