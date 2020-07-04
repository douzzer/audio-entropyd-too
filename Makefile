VERSION=2.0.3
OPT_FLAGS=-O2 -ffast-math
WARNFLAGS=-Wall -Wno-conversion -Waggregate-return -Wstrict-prototypes -g
DEBUGFLAGS= #-DDEBUG
INCLUDES=
DEFINES+=# -DDEBUG
CFLAGS+= $(DEFINES) $(WARNFLAGS) $(DEBUGFLAGS) $(INCLUDES) $(OPT_FLAGS) -DVERSION=\"$(VERSION)\"
LFLAGS=-lm -lasound -g

TARGETS=audio-entropyd

all: $(TARGETS) 

audio-entropyd: audio-entropyd.o error.o proc.o val.o RNGTEST.o error.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LFLAGS) 

install: audio-entropyd
	cp audio-entropyd /usr/local/sbin/
	cp init.d-audio-entropyd /etc/init.d/

clean:
	rm -f *.o core $(TARGETS)

package: clean
	# source package
	rm -rf audio-entropyd-$(VERSION)
	mkdir audio-entropyd-$(VERSION)
	cp *.c *.h TODO Makefile init.d-audio-entropyd COPYING README* audio-entropyd-$(VERSION)
	tar czf audio-entropyd-$(VERSION).tgz audio-entropyd-$(VERSION)
	rm -rf audio-entropyd-$(VERSION)
