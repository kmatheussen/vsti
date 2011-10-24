
INSTALLPATH=/usr/local
JACKINSTALLPATH=/usr

CFLAGS=-Wall -Iinclude -I/usr/local/include -g -O2
LDFLAGS=-L/usr/local/lib -lvst -lasound

all: vst.so

install: vst.so
	chmod a+rx vst.so
	cp vst.so $(JACKINSTALLPATH)/lib/jack/
	chmod a+rx vsti
	cp vsti $(INSTALLPATH)/bin/

vst.so: jackclient.c Makefile
	gcc -shared -o vst.so jackclient.c $(CFLAGS) $(LDFLAGS)

clean:
	rm -f vst.so *.o *~
