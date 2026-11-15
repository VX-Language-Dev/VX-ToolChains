/* fs 库实现 — 导出 VX ABI 符号 _vx_fs_*。
 * 目录/文件操作基于 POSIX（macOS / Linux），路径运算为纯字符串处理。
 * 返回 string 的函数 malloc 新串（调用方 free），失败返回空串。 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *safe(const char *s) { return s ? s : ""; }

static char *dup_str(const char *s) {
  const size_t n = strlen(s);
  char *out = (char *)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n + 1);
  return out;
}

/* ---- 目录与文件 ---- */

int32_t _vx_fs_mkdir(const char *path) {
  if (!path || !*path) return -1;
  return mkdir(path, 0777) == 0 ? 0 : -1;
}

/* 递归创建：逐段 mkdir，已存在（EEXIST）视为成功 */
int32_t _vx_fs_mkdir_p(const char *path) {
  if (!path || !*path) return -1;
  char *buf = dup_str(path);
  if (!buf) return -1;
  for (char *p = buf + 1; *p; ++p) {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(buf, 0777) != 0 && access(buf, F_OK) != 0) {
      free(buf);
      return -1;
    }
    *p = '/';
  }
  const int rc = (mkdir(buf, 0777) == 0 || access(buf, F_OK) == 0) ? 0 : -1;
  free(buf);
  return rc;
}

int32_t _vx_fs_dir_exists(const char *path) {
  struct stat st;
  if (!path || stat(path, &st) != 0) return 0;
  return S_ISDIR(st.st_mode) ? 1 : 0;
}

int32_t _vx_fs_is_file(const char *path) {
  struct stat st;
  if (!path || stat(path, &st) != 0) return 0;
  return S_ISREG(st.st_mode) ? 1 : 0;
}

/* 列出目录：每行一个名称（跳过 . 与 ..），末尾带 \n；失败返回空串 */
char *_vx_fs_list_dir(const char *path) {
  DIR *d = path ? opendir(path) : NULL;
  if (!d) return dup_str("");
  size_t cap = 256;
  size_t len = 0;
  char *out = (char *)malloc(cap);
  if (!out) {
    closedir(d);
    return NULL;
  }
  out[0] = '\0';
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    const size_t n = strlen(ent->d_name);
    if (len + n + 2 > cap) {
      cap *= 2;
      char *grown = (char *)realloc(out, cap);
      if (!grown) {
        free(out);
        closedir(d);
        return NULL;
      }
      out = grown;
    }
    memcpy(out + len, ent->d_name, n);
    len += n;
    out[len++] = '\n';
    out[len] = '\0';
  }
  closedir(d);
  return out;
}

int32_t _vx_fs_remove_file(const char *path) {
  if (!path || !*path) return -1;
  return remove(path) == 0 ? 0 : -1;
}

int32_t _vx_fs_file_size(const char *path) {
  struct stat st;
  if (!path || stat(path, &st) != 0) return -1;
  return (int32_t)st.st_size;
}

/* ---- 路径运算 ---- */

char *_vx_fs_path_join(const char *a, const char *b) {
  a = safe(a);
  b = safe(b);
  if (b[0] == '/') return dup_str(b);
  if (a[0] == '\0') return dup_str(b);
  const size_t na = strlen(a);
  const size_t nb = strlen(b);
  const int need_sep = a[na - 1] != '/';
  char *out = (char *)malloc(na + nb + 2);
  if (!out) return NULL;
  memcpy(out, a, na);
  size_t n = na;
  if (need_sep) out[n++] = '/';
  memcpy(out + n, b, nb);
  n += nb;
  out[n] = '\0';
  return out;
}

char *_vx_fs_path_base(const char *path) {
  path = safe(path);
  const char *slash = strrchr(path, '/');
  return dup_str(slash ? slash + 1 : path);
}

char *_vx_fs_path_dir(const char *path) {
  path = safe(path);
  const size_t n = strlen(path);
  if (n == 0) return dup_str(".");
  /* 去掉末尾连续 '/' 再找最后一个分隔符 */
  size_t end = n;
  while (end > 1 && path[end - 1] == '/') --end;
  const char *slash = NULL;
  for (size_t i = end; i > 0; --i) {
    if (path[i - 1] == '/') {
      slash = path + i - 1;
      break;
    }
  }
  if (!slash) return dup_str(".");
  if (slash == path) return dup_str("/"); /* 根目录 */
  char *out = (char *)malloc((size_t)(slash - path) + 1);
  if (!out) return NULL;
  const size_t len = (size_t)(slash - path);
  memcpy(out, path, len);
  out[len] = '\0';
  return out;
}
