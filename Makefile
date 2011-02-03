# $Id$
include make.defs

all clean: src/common/Makefile src/login/Makefile src/char/Makefile src/map/Makefile
	cd src ; cd common ; $(MAKE) $(MKDEF) $@ ; cd ..
	cd src ; cd login ; $(MAKE) $(MKDEF) $@ ; cd ..
	cd src ; cd char ; $(MAKE) $(MKDEF) $@ ; cd ..
	cd src ; cd map ; $(MAKE) $(MKDEF) $@ ; cd ..
