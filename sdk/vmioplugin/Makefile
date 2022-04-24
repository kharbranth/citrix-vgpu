SUBDIRS=
DOCSLIST=vmioplugin vmiop-vga

default:
	for i in $(SUBDIRS) ; do ( cd $$i && $(MAKE) $(MAKEFLAGS) ) || exit 1; done

docs:	FORCE
	for i in $(DOCSLIST) ; do doxygen docs/$$i.cfg && \
		(cd docs/$$i-latex; $(MAKE) $(MAKEFLAGS) > make.log) && \
		cp -f docs/$$i-latex/refman.pdf docs/$$i.pdf || exit 1; done

clean:
	for i in $(DOCSLIST) ; do \
		rm -rf docs/$$i-html docs/$$i-latex docs/$$i.pdf ; done
	for i in $(SUBDIRS) ; do ( cd $$i && $(MAKE) $(MAKEFLAGS) clean ) || exit 1; done

FORCE:
