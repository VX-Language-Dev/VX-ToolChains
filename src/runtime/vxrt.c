/* vxrt — VX 后端内置最小运行时 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

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

/* 浮点幂（VX ^ 运算符）：libc pow，支持负指数与分数指数 */
double vx_pow_f64(double base, double exp) { return pow(base, exp); }

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

/* ---- 系统能力：命令行参数 ----
 * 进程启动时由构造函数捕获 argc/argv；vxrt 全部符号位于同一编译单元，
 * 链接器只要拉入其中任意一个符号即会带入本文件，构造函数必然执行。 */
static int vx_argc = 0;
static char **vx_argv = 0;

#if defined(__GNUC__) || defined(__clang__)
#define VX_CTOR __attribute__((constructor))
#else
#define VX_CTOR
#endif

VX_CTOR static void vx_rt_capture_argv(int argc, char **argv) {
  vx_argc = argc;
  vx_argv = argv;
}

/* 参数个数（含 argv[0] 可执行文件名） */
int32_t vx_argv_count(void) { return (int32_t)vx_argc; }

/* 第 i 个参数；越界返回空串（不返回 NULL，避免调用方空指针） */
const char *vx_argv_get(int32_t i) {
  if (!vx_argv || i < 0 || i >= vx_argc) return "";
  return vx_argv[i] ? vx_argv[i] : "";
}

/* ---- 系统能力：文件 I/O ---- */

/* 读取整个文件为以 '\0' 结尾的字符串（调用方不释放）。
 * 失败（不存在/权限/内存不足）返回空串；是否可用请配合 vx_file_exists。 */
const char *vx_file_read(const char *path) {
  if (!path) return "";
  FILE *f = fopen(path, "rb");
  if (!f) return "";
  size_t cap = 4096, len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) {
    fclose(f);
    return "";
  }
  for (;;) {
    if (len + 1 >= cap) { /* 预留终止符位置 */
      size_t ncap = cap * 2;
      char *nb = (char *)realloc(buf, ncap);
      if (!nb) {
        free(buf);
        fclose(f);
        return "";
      }
      buf = nb;
      cap = ncap;
    }
    size_t n = fread(buf + len, 1, cap - len - 1, f);
    if (n == 0) break;
    len += n;
  }
  fclose(f);
  buf[len] = '\0';
  return buf;
}

/* 覆写写入文件，返回写入字节数；失败返回 -1 */
int32_t vx_file_write(const char *path, const char *content) {
  if (!path) return -1;
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t len = content ? strlen(content) : 0;
  size_t written = len ? fwrite(content, 1, len, f) : 0;
  fclose(f);
  if (written != len) return -1;
  return (int32_t)written;
}

/* 文件是否存在（可读） */
int32_t vx_file_exists(const char *path) {
  if (!path) return 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

/* 执行 shell 命令，返回 libc system() 的退出码 */
int32_t vx_system(const char *cmd) {
  if (!cmd) return -1;
  return (int32_t)system(cmd);
}
