/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _GGPO_LINUX_H_
#define _GGPO_LINUX_H_

/* Voldorium: the Windows names the library uses, on BSD sockets and libc (the upstream Linux
   platform did not compile). Wire format and behaviour are the same as on Windows. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include "types.h"

typedef int SOCKET;
typedef unsigned long u_long;
typedef uint32_t DWORD;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define WSAEWOULDBLOCK EWOULDBLOCK
#define closesocket close
#define ioctlsocket ioctl
#define WSAGetLastError() errno
#define DebugBreak() abort()
#define CreateDirectoryA(name, attr) mkdir((name), 0777)
#define Sleep(ms) usleep((ms) * 1000)
#define INFINITE (-1)
#define MAX_PATH 260
#define OutputDebugStringA(s) fputs((s), stderr)
#define strcpy_s(dst, src) (snprintf((dst), sizeof(dst), "%s", (src)), 0)
#define sprintf_s snprintf
#define FALSE 0
#define TRUE 1
static inline int strncat_s(char *dst, size_t size, const char *src, size_t count)
{
   const size_t n = strlen(dst);
   if (n + 1 < size)
      strncat(dst, src, count < size - n - 1 ? count : size - n - 1);
   return 0;
}
#define vsprintf_s vsnprintf
#define fopen_s(fp, name, mode) ((*(fp) = fopen((name), (mode))) ? 0 : errno)
#define SO_DONTLINGER (-1)	/* not on Linux: that setsockopt fails, as an unknown option */

/* poll.cpp waits on one dummy event that is never set: a sleep with a timeout */
typedef void *HANDLE;
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define CreateEvent(attr, manual, initial, name) ((HANDLE)0)
static inline int WaitForMultipleObjects(int n, const HANDLE *h, bool all, int timeout)
{
   (void)n; (void)h; (void)all;
   if (timeout > 0)
      usleep((useconds_t)timeout * 1000);
   return WAIT_TIMEOUT;
}

class Platform {
public:  // types
   typedef pid_t ProcessID;

public:  // functions
   static ProcessID GetProcessID() { return getpid(); }
   static void AssertFailed(char *msg) { fprintf(stderr, "GGPO assertion failed: %s\n", msg); }
   static uint32 GetCurrentTimeMS();
   static int GetConfigInt(const char* name);
   static bool GetConfigBool(const char* name);
};

#endif
