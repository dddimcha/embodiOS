/* Kernel stub for unistd.h
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_UNISTD_H
#define _COMPAT_UNISTD_H

#include "stddef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard file descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Type definitions */
typedef int pid_t;
typedef int uid_t;
typedef int gid_t;
typedef long off_t;
typedef long ssize_t;

/* Seek constants (also in stdio.h) */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* Access modes for access() */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

/* Page size */
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE

/* Function declarations - most are stubs */
int access(const char* pathname, int mode);
int close(int fd);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char* pathname);
int rmdir(const char* pathname);
char* getcwd(char* buf, size_t size);
int chdir(const char* path);
pid_t getpid(void);
uid_t getuid(void);
gid_t getgid(void);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
long sysconf(int name);
int isatty(int fd);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_UNISTD_H */
