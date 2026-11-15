/* text 库实现 — 导出 VX ABI 符号 _vx_text_*。
 * 覆盖 string 入参、int/bool 返回与 void 返回；返回 string 的函数
 * 一律 malloc 新串（调用方 free），入参 NULL 按空串处理。 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *safe(const char *s) { return s ? s : ""; }

/* 分配并拷贝 [start, start+len)（已由调用方裁剪到合法范围） */
static char *dup_range(const char *s, size_t start, size_t len) {
  char *out = (char *)malloc(len + 1);
  if (!out) return NULL;
  if (len > 0) memcpy(out, s + start, len);
  out[len] = '\0';
  return out;
}

int32_t _vx_text_length(const char *s) {
  return s ? (int32_t)strlen(s) : 0;
}

int32_t _vx_text_is_empty(const char *s) { return (s == NULL || *s == '\0'); }

void _vx_text_shout(const char *s) {
  if (!s) return;
  for (const char *p = s; *p; ++p) putchar(*p);
  putchar('\n');
}

/* ---- 查找与匹配 ---- */

int32_t _vx_text_find(const char *s, const char *needle) {
  s = safe(s);
  needle = safe(needle);
  const char *p = strstr(s, needle);
  return p ? (int32_t)(p - s) : -1;
}

int32_t _vx_text_contains(const char *s, const char *needle) {
  return _vx_text_find(s, needle) >= 0;
}

int32_t _vx_text_starts_with(const char *s, const char *prefix) {
  s = safe(s);
  prefix = safe(prefix);
  const size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

int32_t _vx_text_ends_with(const char *s, const char *suffix) {
  s = safe(s);
  suffix = safe(suffix);
  const size_t ns = strlen(s);
  const size_t nf = strlen(suffix);
  if (nf > ns) return 0;
  return memcmp(s + (ns - nf), suffix, nf) == 0;
}

/* ---- 截取与清理 ---- */

char *_vx_text_substr(const char *s, int32_t start, int32_t len) {
  s = safe(s);
  const int32_t n = (int32_t)strlen(s);
  if (start < 0) start = 0;
  if (len < 0) len = 0;
  if (start > n) start = n;
  if (len > n - start) len = n - start;
  return dup_range(s, (size_t)start, (size_t)len);
}

char *_vx_text_trim(const char *s) {
  s = safe(s);
  const char *b = s;
  while (*b && isspace((unsigned char)*b)) ++b;
  const char *e = b + strlen(b);
  while (e > b && isspace((unsigned char)e[-1])) --e;
  return dup_range(b, 0, (size_t)(e - b));
}

int32_t _vx_text_char_at(const char *s, int32_t index) {
  s = safe(s);
  if (index < 0) return -1;
  const size_t n = strlen(s);
  if ((size_t)index >= n) return -1;
  return (int32_t)(unsigned char)s[index];
}

char *_vx_text_from_char(int32_t c) {
  char *out = (char *)malloc(2);
  if (!out) return NULL;
  out[0] = (char)(c & 0xFF);
  out[1] = '\0';
  return out;
}

/* ---- 数值转换 ---- */

int32_t _vx_text_is_int(const char *s) {
  s = safe(s);
  while (*s && isspace((unsigned char)*s)) ++s;
  if (*s == '+' || *s == '-') ++s;
  if (*s == '\0') return 0;
  for (; *s; ++s) {
    if (!isdigit((unsigned char)*s)) return 0;
  }
  return 1;
}

int32_t _vx_text_parse_int(const char *s) {
  s = safe(s);
  while (*s && isspace((unsigned char)*s)) ++s;
  int negative = 0;
  if (*s == '+' || *s == '-') {
    negative = (*s == '-');
    ++s;
  }
  long long v = 0;
  for (; *s && isdigit((unsigned char)*s); ++s) {
    v = v * 10 + (*s - '0');
    if (v > 2147483647LL) { /* 饱和到 int32 上界，避免未定义行为 */
      v = negative ? 2147483648LL : 2147483647LL;
      break;
    }
    if (negative && v > 2147483648LL) v = 2147483648LL;
  }
  return (int32_t)(negative ? -v : v);
}

char *_vx_text_format_int(int32_t n) {
  char buf[32];
  snprintf(buf, sizeof buf, "%d", n);
  return dup_range(buf, 0, strlen(buf));
}

/* ---- 行切分 ---- */

int32_t _vx_text_line_count(const char *s) {
  s = safe(s);
  if (*s == '\0') return 0;
  int32_t count = 1;
  for (const char *p = s; *p; ++p) {
    if (*p == '\n' && p[1] != '\0') ++count;
  }
  return count;
}

char *_vx_text_line_at(const char *s, int32_t index) {
  s = safe(s);
  if (index < 0) return dup_range("", 0, 0);
  int32_t cur = 0;
  const char *b = s;
  for (const char *p = s;; ++p) {
    if (*p == '\n' || *p == '\0') {
      if (cur == index) {
        const char *e = p;
        if (e > b && e[-1] == '\r') --e; /* CRLF：去掉行尾 \r */
        return dup_range(b, 0, (size_t)(e - b));
      }
      if (*p == '\0') break;
      ++cur;
      b = p + 1;
    }
  }
  return dup_range("", 0, 0);
}
