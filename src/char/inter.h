// $Id: inter.h,v 1.1.1.1 2004/09/10 17:26:51 MagicalTux Exp $
#ifndef _INTER_H_
#define _INTER_H_

int  inter_init (const char *file);
int  inter_save ();
int  inter_parse_frommap (int fd);
int  inter_mapif_init (int fd);

int  inter_check_length (int fd, int length);

int  inter_log (char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));

#define inter_cfgName "conf/inter_athena.conf"

extern int party_share_level;
extern int db_count;
extern int db_skip_count;
extern char inter_log_filename[1024];
extern int min_hair_style;
extern int min_hair_color;
extern int max_hair_style;
extern int max_hair_color;

#endif
