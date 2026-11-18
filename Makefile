#
# Makefile
#
# See the README file for copyright information and how to reach the author.
#

EPGD_SRC ?= ../..

include $(EPGD_SRC)/Make.config

PLUGIN = sngtv

SOFILE = libepgd-sngtv.so
OBJS = sngtv.o

CFLAGS += -I$(EPGD_SRC) -Wno-long-long

all: $(SOFILE)

$(SOFILE): $(OBJS)
	$(CC) $(CFLAGS) -shared $(OBJS) $(LIBS) -lcurl -ljpeg -o $@

install: $(SOFILE) install-config install-scripts
	install -D $(SOFILE) $(_PLGDEST)/

install-scripts:
	install --mode=755 -D ./scripts/sngtv-channels.py $(_BINDEST)/sngtv-channels

clean:
	@-rm -f $(OBJS) core* *~ *.so
	rm -f ./configs/*~

# never overwrite existing (maybe customized) config files

install-config:
	if ! test -d $(CONFDEST); then \
	   mkdir -p $(CONFDEST); \
	   chmod a+rx $(CONFDEST); \
	fi
	for i in ./configs/sngtv*.xsl ./configs/sngtv*.xml; do \
	   if ! test -f "$(CONFDEST)/`basename $$i`"; then \
	      install --mode=644 -D "$$i" $(CONFDEST)/; \
	   fi; \
	done

sngtv.o: sngtv.c sngtv.h jpegscale.h
