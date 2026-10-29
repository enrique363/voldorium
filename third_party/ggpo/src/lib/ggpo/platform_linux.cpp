/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include <time.h>
#include <strings.h>
#include "platform_linux.h"

uint32 Platform::GetCurrentTimeMS() {
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint32)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

int
Platform::GetConfigInt(const char* name)
{
   const char *v = getenv(name);
   return v ? atoi(v) : 0;
}

bool Platform::GetConfigBool(const char* name)
{
   const char *v = getenv(name);
   return v && (atoi(v) != 0 || strcasecmp(v, "true") == 0);
}
