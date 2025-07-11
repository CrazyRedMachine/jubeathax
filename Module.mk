cflags          += -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -Ipkcs11 -Wno-attributes

avsvers_32      := 1700 1508
avsvers_64      := 1700 1509

imps            += avs avs-ea3

include util/Module.mk
include libdisasm/Module.mk
include minhook/Module.mk
include jubeathax/Module.mk
include io_plugins/Module.mk

#
# Distribution build rules
#

zipdir          := $(BUILDDIR)/zip
jubeathax_version := $(shell grep "define PROGRAM_VERSION" jubeathax/dllmain.cc | cut -d'"' -f2)

plugin_list := $(addsuffix .dll,$(addprefix build/bin/indep-32/,$(plugindlls)))

$(BUILDDIR)/jubeathax_v$(jubeathax_version).zip: \
	build/bin/avs2_1700-32/jubeathax.dll \
	$(plugin_list)	
	@echo ... $@
	@mkdir -p $(zipdir)
	@cp -a -p build/bin/avs2_1700-32/jubeathax.dll $(zipdir)
	@cp -r -a -p build/bin/indep-32/*.dll $(zipdir)
	@cp -r -a -p dist/jubeathax/* $(zipdir)
	@cd $(zipdir) \
	&& zip -r ../jubeathax_v$(jubeathax_version).zip ./*

zipdir          := $(BUILDDIR)/zip

all: $(BUILDDIR)/jubeathax_v$(jubeathax_version).zip
