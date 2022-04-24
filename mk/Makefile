USE_BRANDING := yes
IMPORT_BRANDING := yes
include $(B_BASE)/common.mk
include $(B_BASE)/rpmbuild.mk

REPO_NAME := vgpu
REPO_LOC  := $(call git_loc,vgpu)

J_FLAG = 8
MAKE_CMD = $(MAKE) -j$(J_FLAG) -C $($REPO_LOC)

-include $(MY_OBJ_DIR)/version.inc
$(MY_OBJ_DIR)/version.inc:
	rm -f $(MY_OBJ_DIR)/version.inc
	$(version-makefile) > $@
	$(call git_cset_number,vgpu) >> $@
	echo VGPU_VERSION := \$$\(PRODUCT_VERSION\) >> $@
	echo VGPU_RELEASE := \$$\(CSET_NUMBER\) >> $@

CHANGELOG_DATE    := $(shell LANG=C date +"%a %b %d %Y")
CHANGELOG_USER    := $(COMPANY_NAME) <$(COMPANY_WEBSITE)>
CHANGELOG_HEADER  := * $(CHANGELOG_DATE) $(CHANGELOG_USER) [$(VGPU_VERSION) $(VGPU_RELEASE)]
CHANGELOG_MESSAGE := - Build vgpu.

PKG := vgpu-$(VGPU_VERSION)-$(VGPU_RELEASE).$(DOMAIN0_ARCH_OPTIMIZED).rpm
PKG_COOKIE := $(MY_XENSERVER_PACKAGES)/.cookie

RPM_BINDIR := $(RPM_RPMSDIR)/$(DOMAIN0_ARCH_OPTIMIZED)

SPKG := vgpu-$(VGPU_VERSION)-$(VGPU_RELEASE).src.rpm
SPKG_COOKIE := $(RPM_SRPMSDIR)/.cookie

.PHONY: build
build: $(PKG_COOKIE) \
	$(MY_SOURCES)/MANIFEST
	@ :

.PHONY: clean
clean:
	rm -f $(RPM_SOURCESDIR)/*
	rm -f $(RPM_SPECSDIR)/*
	rm -f $(PKG_COOKIE) $(PKG)
	rm -f $(MY_OBJ_DIR)/version.inc
	rm -f $(MY_SOURCES)/MANIFEST

SOURCES = $(RPM_SOURCESDIR)/vgpu-$(VGPU_VERSION).tar.bz2 \
	  $(RPM_SOURCESDIR)/vgpu-development.patch \
	  $(RPM_SPECSDIR)/vgpu.spec

$(RPM_SOURCESDIR)/vgpu-$(VGPU_VERSION).tar.bz2: $(RPM_SOURCESDIR)/.dirstamp $(REPO_LOC)
	set -e; set -o pipefail; \
	(cd $(REPO_LOC) && exec git archive --prefix=$(patsubst %.tar.bz2,%,$(notdir $@))/ HEAD) | bzip2 -c > $@.tmp
	mv -f $@.tmp $@

$(RPM_SOURCESDIR)/vgpu-development.patch: $(RPM_SOURCESDIR)/.dirstamp $($REPO_LOC)
	(cd $(REPO_LOC) && exec git diff) > $@

$(RPM_SPECSDIR)/vgpu.spec: vgpu.spec.in $(RPM_SPECSDIR)/.dirstamp
# Use a temporary file because sed in rhel3 doesn't understand "\n"
	echo "$(CHANGELOG_HEADER)" > changelog.tmp
	echo "$(CHANGELOG_MESSAGE)" >> changelog.tmp
	echo "" >> changelog.tmp
	sed -e s\,@REPO_VERSION@,$(REPO_VERSION),g \
	    -e s\,@VGPU_VERSION@,$(VGPU_VERSION),g\
	    -e s\,@VGPU_RELEASE@,$(VGPU_RELEASE),g\
	    -e \\,%changelog,rchangelog.tmp             \
	    < vgpu.spec.in                           \
	    > $(RPM_SPECSDIR)/vgpu.spec
	-rm changelog.tmp

$(PKG_COOKIE): $(RPM_DIRECTORIES) $(SOURCES)
	$(RPMBUILD) --target $(DOMAIN0_ARCH_OPTIMIZED) -bb $(RPM_SPECSDIR)/vgpu.spec
	mkdir -p $(MY_XENSERVER_PACKAGES)
	cp $(RPM_BINDIR)/$(PKG) $(MY_XENSERVER_PACKAGES)
	@touch $@

$(SPKG_COOKIE): $(RPM_DIRECTORIES) $(SOURCES)
	$(RPMBUILD) -bs $(RPM_SPECSDIR)/vgpu.spec

$(MY_SOURCES)/MANIFEST: $(SPKG_COOKIE)
	rm -f $@
	mkdir -p $(MY_SOURCES) 
	echo "$(REPO_NAME) $(call rpm_license,$(RPM_SRPMSDIR)/$(SPKG)) file $(RPM_SRPMSDIR)/$(SPKG)" > $@
