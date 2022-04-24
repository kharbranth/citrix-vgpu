TARGET = vgpu

OBJS :=	device.o \
	mapcache.o \
	surface.o \
	demu.o \
	control.o

CFLAGS  = -I$(shell pwd)

ifeq ($(D), 1)
CFLAGS += -g
else
CFLAGS += -g -O1
endif

# _GNU_SOURCE for asprintf.
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_GNU_SOURCE 
CFLAGS += -Wall -Werror
CFLAGS += -pthread
CFLAGS += -DXC_WANT_COMPAT_MAP_FOREIGN_API -DXC_WANT_COMPAT_EVTCHN_API -DXC_WANT_COMPAT_GNTTAB_API -DXC_WANT_COMPAT_DEVICEMODEL_API

ifeq ($(shell uname),Linux)
LDLIBS := -lutil -lrt -ldl
endif

LDLIBS += -lxenstore -lxenctrl -ljson-c -lempserver

VPATH += :./drivers/vgpu/xen/vmioplugin-xs-6.5/environment:./drivers/vgpu/xen/vmioplugin-xs-6.5/plugins/presentation
CFLAGS += -I./sdk/vmioplugin/inc -I./drivers/vgpu/xen/vmioplugin-xs-6.5/inc

OBJS += vmiop-env.o vmiop-vga.o vmiop-presentation.o

LDFLAGS += -Wl,--dynamic-list=./drivers/vgpu/xen/vmioplugin-xs-6.5/vmiop-dynamic-list
LDFLAGS += -pthread

# Get gcc to generate the dependencies for us.
CFLAGS   += -Wp,-MD,$(@D)/.$(@F).d

SUBDIRS  = $(filter-out ./,$(dir $(OBJS) $(LIBS)))
DEPS     = .*.d

LDFLAGS += -g 

all: $(TARGET)

$(TARGET): $(LIBS) $(OBJS)
	gcc -o $@ $(LDFLAGS) $(OBJS) $(LIBS) $(LDLIBS)

%.o: %.c
	gcc -o $@ $(CFLAGS) -c $<

.PHONY: ALWAYS

clean:
	$(foreach dir,$(SUBDIRS),make -C $(dir) clean)
	rm -f $(OBJS)
	rm -f $(DEPS)
	rm -f $(TARGET)
	rm -f TAGS

.PHONY: TAGS
TAGS:
	find . -name \*.[ch] | etags -

-include $(DEPS)

print-%:
	echo $($*)
