include ../../make.defs

all: char-server
txt: char-server

COMMON_OBJ = ../common/core.o ../common/socket.o ../common/timer.o ../common/db.o ../common/lock.o ../common/malloc.o ../common/showmsg.o ../common/strlib.o ../common/mt_rand.o
COMMON_H = ../common/core.h ../common/socket.h ../common/timer.h ../common/mmo.h ../common/db.h ../common/lock.h ../common/timer.h ../common/malloc.h ../common/showmsg.h ../common/strlib.h ../common/mt_rand.h
char-server: char.o inter.o int_party.o int_guild.o int_storage.o $(COMMON_OBJ)
	$(CC) ${CHARPROFILE} $(CFLAGS) -o ../../$@ $^

char.o: char.c char.h inter.h $(COMMON_H) ../common/version.h
inter.o: inter.c inter.h int_party.h int_guild.h int_storage.h char.h $(COMMON_H)
int_party.o: int_party.c int_party.h inter.h char.h $(COMMON_H)
int_guild.o: int_guild.c int_guild.h int_storage.h inter.h char.h $(COMMON_H)
int_storage.o: int_storage.c int_storage.h int_guild.h inter.h char.h $(COMMON_H)

clean:
	rm -f *.o ../../char-server
