CC = i686-w64-mingw32-gcc -O2 -Wall

all: dmgplay

dmgplay: dmgplay.c uzlib-2.9.5/lib/libtinf.a
	$(CC) -o $@ $< -Iuzlib-2.9.5/src -Luzlib-2.9.5/lib -ltinf

uzlib-2.9.5/lib/libtinf.a:
	$(MAKE) CC="$(CC)" -C uzlib-2.9.5/src

strip: dmgplay
	strip dmgplay

clean:
	$(MAKE) -C uzlib-2.9.5/src clean
	$(RM) dmgplay
