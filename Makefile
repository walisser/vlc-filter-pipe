PREFIX = /usr/local
LD = ld
CC = cc
PKG_CONFIG = pkg-config
INSTALL = install
CFLAGS = -g -O2 -Wall -Wextra
LDFLAGS = 
LIBS = -lpthread -lm
VLC_PLUGIN_CFLAGS := $(shell $(PKG_CONFIG) --cflags vlc-plugin)
VLC_PLUGIN_LIBS := $(shell $(PKG_CONFIG) --libs vlc-plugin)

GLIB_CFLAGS := $(shell $(PKG_CONFIG) --cflags glib-2.0)
GLIB_LIBS   := $(shell $(PKG_CONFIG) --libs   glib-2.0)

libdir = $(PREFIX)/lib
plugindir = $(libdir)/vlc/plugins/video_filter

override CC += -std=gnu99
override CPPFLAGS += -DPIC -I. -Isrc
override CFLAGS += -fPIC
override LDFLAGS += -Wl,-no-undefined,-z,defs

override CPPFLAGS += -DMODULE_STRING=\"y4mpipe\"
override CFLAGS += $(VLC_PLUGIN_CFLAGS)
override LIBS += $(VLC_PLUGIN_LIBS)

override CFLAGS += $(GLIB_CFLAGS)
override LIBS += $(GLIB_LIBS)

TARGETS = libpipe_plugin.so

all: liby4mpipe_plugin.so

install: all
	mkdir -p -- $(DESTDIR)$(plugindir)
	$(INSTALL) --mode 0755 liby4mpipe_plugin.so $(DESTDIR)$(plugindir)

install-strip:
	$(MAKE) install INSTALL="$(INSTALL) -s"

uninstall:
	rm -f $(plugindir)/liby4mpipe_plugin.so

clean:
	rm -f -- liby4mpipe_plugin.so src/*.o

mostlyclean: clean

SOURCES = y4mpipe.c 

$(SOURCES:%.c=src/%.o): %: 
 
liby4mpipe_plugin.so: $(SOURCES:%.c=src/%.o)
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

.PHONY: all install install-strip uninstall clean mostlyclean
