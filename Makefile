PROG=mrcalendar
SRC=mrcalendar.c
CC=gcc
CFLAGS=`pkg-config --cflags gtk4 gdk-pixbuf-2.0`
LDFLAGS=`pkg-config --libs gtk4 gdk-pixbuf-2.0` -lX11

all: $(PROG)

$(PROG): $(SRC)
	$(CC) -c $(CFLAGS) -g -std=gnu99 $(SRC)
	$(CC) mrcalendar.o $(LDFLAGS) -o $(PROG)

install: $(PROG)
	install -Dm755 $(PROG) /usr/bin/$(PROG)
	install -Dm644 mrcalendar.desktop /usr/share/applications/mrcalendar.desktop

uninstall:
	rm -f /usr/bin/$(PROG)
	rm -f /usr/share/applications/mrcalendar.desktop

clean:
	rm -f *.o $(PROG)

.PHONY: all install uninstall clean
