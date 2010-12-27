// $Id: map.h,v 1.8 2004/09/25 11:39:17 MouseJstr Exp $
#ifndef _LANG_H_
#define _LANG_H_

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <netinet/in.h>
#include "mmo.h"

extern struct dbt *translate_db;

void do_init_langs ();

const char* lang_trans(const char *str, int lng);

#endif
