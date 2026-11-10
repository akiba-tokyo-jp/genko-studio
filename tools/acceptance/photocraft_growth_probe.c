/* Synthetic-asset-only read growth probe. Not linked into the product. */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int injected;
static int matches(int fd) {
    const char *target = getenv("GENKO_PROBE_ASSET");
    if (!target || !*target) return 0;
    char name[64], path[PATH_MAX];
    snprintf(name, sizeof name, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(name, path, sizeof path-1);
    if (n < 0) return 0;
    path[n] = 0;
    return strcmp(path,target) == 0;
}
static void grow(int fd, long long size) {
    if (injected || !matches(fd)) return;
    injected = 1;
    const char *target = getenv("GENKO_PROBE_ASSET");
    int writer = open(target,O_WRONLY);
    int ok = writer >= 0 && ftruncate(writer,70000000) == 0;
    if (writer >= 0) close(writer);
    fprintf(stderr,"GENKO_GROWTH_PROBE opened_size=%lld grew=%d\n",size,ok);
}
int fstat(int fd, struct stat *st) {
    int (*real)(int,struct stat *) = dlsym(RTLD_NEXT,"fstat");
    int rc = real(fd,st);
    if (!rc) grow(fd,(long long)st->st_size);
    return rc;
}
int fstat64(int fd, struct stat64 *st) {
    int (*real)(int,struct stat64 *) = dlsym(RTLD_NEXT,"fstat64");
    int rc = real(fd,st);
    if (!rc) grow(fd,(long long)st->st_size);
    return rc;
}
ssize_t read(int fd, void *buffer, size_t count) {
    ssize_t (*real)(int,void *,size_t) = dlsym(RTLD_NEXT,"read");
    ssize_t rc = real(fd,buffer,count);
    if (injected && matches(fd)) fprintf(stderr,"GENKO_GROWTH_READ request=%zu actual=%zd\n",count,rc);
    return rc;
}
