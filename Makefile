# mapt - two panel APT package manager
#
# Targets:
#   make          build ./mapt
#   make test     build and run unit tests
#   make install  install binary + man page into $(PREFIX)
#   make clean    remove build artefacts

APP      := mapt
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man/man1

CC       ?= cc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wpedantic
OPT      ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE -Isrc
CFLAGS   += $(CSTD) $(WARN) $(OPT)

# pkg-config is optional; fall back to plain -lncursesw.
NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
NCURSES_LIBS   := $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)
CFLAGS   += $(NCURSES_CFLAGS)
LDLIBS   += $(NCURSES_LIBS)

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

all: $(APP)

$(APP): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/test_vercmp: tests/test_vercmp.c src/vercmp.c src/vercmp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_vercmp.c src/vercmp.c

tests/test_vercmp_dpkg: tests/test_vercmp_dpkg.c src/vercmp.c src/vercmp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_vercmp_dpkg.c src/vercmp.c

tests/test_shadow_leak: tests/test_shadow_leak.c src/ui.c src/util.c src/ui.h src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_shadow_leak.c src/ui.c src/util.c $(LDFLAGS) $(LDLIBS)

test: tests/test_vercmp tests/test_vercmp_dpkg tests/test_shadow_leak
	./tests/test_vercmp
	./tests/test_vercmp_dpkg
	@if [ -t 0 ]; then ./tests/test_shadow_leak; \
	 else script -qec ./tests/test_shadow_leak /dev/null | tr -d '\r'; fi

install: $(APP)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 0755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)
	install -m 0644 doc/mapt.1 $(DESTDIR)$(MANDIR)/mapt.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(APP) $(DESTDIR)$(MANDIR)/mapt.1

clean:
	rm -f $(APP) $(OBJS) $(DEPS) tests/test_vercmp tests/test_vercmp_dpkg

-include $(DEPS)

.PHONY: all test install uninstall clean
