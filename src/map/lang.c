#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef LCCWIN32
#include <winsock.h>
#else
#include <netdb.h>
#endif

#include "strlib.h"
#include "core.h"
#include "timer.h"
#include "db.h"
#include "grfio.h"
#include "malloc.h"

#include "map.h"
#include "chrif.h"
#include "intif.h"
#include "nullpo.h"
#include "socket.h"

#ifdef MEMWATCH
#include "memwatch.h"
#endif

struct dbt *translate_db = NULL;
char *lang_langs[100];
int lang_num = 0;

static int langsdb_readdb (void);
static int langsdb_readlangs (void);

void do_init_langs ()
{
    translate_db = strdb_init (100);

    langsdb_readlangs ();
    langsdb_readdb ();
}

static int langsdb_readlangs (void)
{
    FILE *fp;
    lang_num = 0;
    fp = fopen_ ("langs/langs.txt", "r");
    if (fp == NULL)
    {
        printf ("can't read langs/langs.txt\n");
        return 1;
    }
    char line[100];
    char text[101];
    while (fgets (line, 99, fp))
    {
        if (sscanf(line, "%99s\n", text) < 1)
            continue;

        lang_langs[lang_num] = strdup (text);
        lang_num ++;
    }
    return 0;
}

static int langsdb_readdb (void)
{
    FILE *fp;
    char line1[1020], line2[1020];
    char text1[1020], text2[1020];
    char filename[1000];
    char **strings = NULL;
    int *lng = NULL;
    int i;
    for (i = 0; i < lang_num; i ++)
    {
        strcpy (filename, "langs/lang_");
        strcat (filename, lang_langs[i]);
        strcat (filename, ".txt");

        fp = fopen_ (filename, "r");
        if (fp == NULL)
        {
            printf ("can't read %s\n", filename);
            return 1;
        }

        while (fgets (line1, 1010, fp))
        {
            if (*line1 == 0 || !fgets (line2, 1010, fp) || *line2 == 0)
                continue;

            if (sscanf(line1, "%1000s\n", text1) < 1 || sscanf(line2, "%1000s\n", text2) < 1)
                continue;

            strings = strdb_search (translate_db, text1);
            if (!strings)
            {
                strings = aCalloc (lang_num, sizeof(int*));
                lng = aMalloc (sizeof(int));
                *lng = i;
                strings[0] = strdup (text1);
                strdb_insert (translate_db, strdup (text1), strings);
            }

            strings[i] = strdup (text2);
        }
        fclose (fp);
    }
    return 0;
}

const char* lang_trans(const char *str, int lng)
{
    if (!str)
        return 0;

    if (lng < 0 || lng >= lang_num)
        return str;

    char **strings = strdb_search (translate_db, str);
    if (!strings)
    {
        printf ("no translations for: %s\n", str);
        return str;
    }

    if (!strings[lng])
    {
        printf ("no lang string (%d) for: %s\n", lang_langs[lng], str);
        return str;
    }

    return strings[lng];
}
