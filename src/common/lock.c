
#include <unistd.h>
#include <stdio.h>
#include "lock.h"
#include "socket.h"

// 書き込みファイルの保護処理
// （書き込みが終わるまで、旧ファイルを保管しておく）

// 新しいファイルの書き込み開始
FILE *lock_fopen (const char *filename, int *info, int *cnt)
{
    char newfile[512];
    FILE *fp;
    int  no = getpid ();

    // 安全なファイル名を得る（手抜き）
    do
    {
        sprintf (newfile, "%s_%d.tmp", filename, no++);
    }
    while ((fp = fopen_ (newfile, "r")) && fclose_ (fp));
    *info = --no;
    return fopen_ (newfile, "w");
}

// 旧ファイルを削除＆新ファイルをリネーム
int lock_fclose (FILE * fp, const char *filename, int *info, int *cnt)
{
    int  ret = 0;
    char newfile[512];
    if (fp != NULL)
    {
        ret = fclose_ (fp);
        if (cnt && (*cnt)%2)
        {
            char file2[512];
            sprintf (newfile, "%s_%d.tmp", filename, *info);
            sprintf (file2, "%s_%d", filename, (int)((*cnt)/2));
            remove (file2);
            rename (newfile, file2);
            return ret;
        }
        else
        {
            sprintf (newfile, "%s_%d.tmp", filename, *info);
            remove (filename);
            rename (newfile, filename);
            return ret;
        }
    }
    else
    {
        return 1;
    }
}
