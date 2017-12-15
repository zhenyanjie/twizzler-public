ifndef PROJECT
$(error PROJECT is not set. Please choose one of ($(shell ls --format=commas projects)), or create a new one in projects/)
endif

.DEFAULT_GOAL=all

.newproj:
	mkdir projects/${PROJECT}
	cp projects/.config.mk.template projects/${PROJECT}/config.mk

ifneq ("$(shell ls -d projects/$(PROJECT))","projects/$(PROJECT)")
$(error Project $(PROJECT) does not exist. Perhaps you wish to create it?)
endif

CONFIGFILE=projects/$(PROJECT)/config.mk
BUILDDIR=projects/$(PROJECT)/build

include $(CONFIGFILE)
export PATH := ${TOOLCHAIN_PATH}/bin:$(PATH)
DEFINES=$(addprefix -D,$(shell sed -e 's/=y/=1/g' -e 's/=n/=0/g' -e 's/\#.*$$//' -e '/^$$/d' -e 's/+=/=/g' $(CONFIGFILE)))

ARCH=$(CONFIG_ARCH)
MACHINE=$(CONFIG_MACHINE)

INCLUDES=-Iinclude -Imachine/$(MACHINE)/include -Iarch/$(ARCH)/include
CFLAGS=-ffreestanding -Wall -Wextra -std=gnu11 -include stdbool.h -include stddef.h -include stdint.h $(INCLUDES) -include printk.h $(DEFINES) -include system.h -fno-omit-frame-pointer -g -Wno-error=unused-variable
ASFLAGS=$(INCLUDES) $(DEFINES)

ifeq ($(CONFIG_WERROR),y)
CFLAGS+=-Werror
endif

ifeq ($(CONFIG_UBSAN),y)
CFLAGS+=-fsanitize=undefined -fstack-check -fstack-protector-all
endif

CFLAGS+=-O$(CONFIG_OPTIMIZE)

C_SOURCES=
ASM_SOURCES=

ifeq ($(CONFIG_INSTRUMENT),y)
CFLAGS+=-finstrument-functions '-finstrument-functions-exclude-file-list=lib/vsprintk.c,core/panic.c,core/instrument.c,core/ksymbol.c'
C_SOURCES+=core/instrument.c
endif

OBJECTS=$(addprefix $(BUILDDIR)/,$(ASM_SOURCES:.S=.o) $(C_SOURCES:.c=.o))

all: $(BUILDDIR)/kernel

include arch/$(ARCH)/include.mk
include machine/$(MACHINE)/include.mk
include lib/include.mk
include core/include.mk
FEATURE_FLAGS=$(addprefix -D,$(addsuffix =1,$(ARCH_KERNEL_SUPPORT)))

CFLAGS+=$(FEATURE_FLAGS)
ASFLAGS+=$(FEATURE_FLAGS)

CRTBEGIN=$(shell $(TOOLCHAIN_PATH)/bin/$(TOOLCHAIN_PREFIX)gcc -print-file-name=crtbegin.o)
CRTEND=$(shell $(TOOLCHAIN_PATH)/bin/$(TOOLCHAIN_PREFIX)gcc -print-file-name=crtend.o)
ifeq ($(CRTI),)
CRTI=$(shell $(TOOLCHAIN_PATH)/bin/$(TOOLCHAIN_PREFIX)gcc -print-file-name=crti.o)
endif
ifeq ($(CRTN),)
CRTN=$(shell $(TOOLCHAIN_PATH)/bin/$(TOOLCHAIN_PREFIX)gcc -print-file-name=crtn.o)
endif

-include $(addprefix $(BUILDDIR)/,$(C_SOURCES:.c=.d) $(ASM_SOURCES:.S=.d))

test: $(BUILDDIR)/kernel# userspace
	$(QEMU) $(QEMU_FLAGS) -serial stdio

export TOOLCHAIN_PREFIX
export BUILDDIR

#userspace: tools/fotgen
#	$(MAKE) -C us all

#tools/fotgen: tools/fotgen.c
#	$(CC) -Wall -Wextra -std=gnu11 -Og -g tools/fotgen.c -o tools/fotgen

$(BUILDDIR)/kernel: $(BUILDDIR)/link.ld $(OBJECTS) $(BUILDDIR)/symbols.o
	@mkdir -p $(BUILDDIR)
	@echo "[LD]  $@"
	@$(TOOLCHAIN_PREFIX)gcc -ffreestanding -nostdlib $(CRTI) $(CRTBEGIN) $(OBJECTS) $(BUILDDIR)/symbols.o $(CRTEND) $(CRTN) -o $(BUILDDIR)/kernel -T $(BUILDDIR)/link.ld -lgcc -Wl,--export-dynamic

$(BUILDDIR)/symbols.o: $(BUILDDIR)/symbols.c
	@echo "[CC]  $@"
	@$(TOOLCHAIN_PREFIX)gcc $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/symbols.c: $(BUILDDIR)/kernel.sym.c
	@echo "[GEN] $@"
	@echo '#include <ksymbol.h>' > $(BUILDDIR)/symbols.c
	@echo '__attribute__((section(".ksyms"))) const struct ksymbol kernel_symbol_table[] = {' >> $(BUILDDIR)/symbols.c
	@cat $(BUILDDIR)/kernel.sym.c >> $(BUILDDIR)/symbols.c
	@echo "};" >> $(BUILDDIR)/symbols.c
	@echo >> $(BUILDDIR)/symbols.c
	@echo '__attribute__((section(".ksyms"))) const size_t kernel_symbol_table_length = ' $$(wc -l < $(BUILDDIR)/kernel.sym.c) ';' >> $(BUILDDIR)/symbols.c

$(BUILDDIR)/kernel.sym.c: $(BUILDDIR)/kernel.stage1
	@echo "[GEN] $@"
	@$(TOOLCHAIN_PREFIX)objdump -t $(BUILDDIR)/kernel.stage1 | grep '^.* [lg]' | awk '{print $$1 " " $$(NF-1) " " $$NF}' | grep -v '.hidden' | sed -rn 's|([0-9a-f]+) ([0-9a-f]+) ([a-zA-Z0-9_/\.]+)|{.value=0x\1, .size=0x\2, .name="\3"},|p' > $(BUILDDIR)/kernel.sym.c

$(BUILDDIR)/kernel.stage1: $(BUILDDIR)/link.ld $(OBJECTS)
	@echo "[LD]  $@"
	@mkdir -p $(BUILDDIR)
	@$(TOOLCHAIN_PREFIX)gcc -ffreestanding -nostdlib $(CRTI) $(CRTBEGIN) $(OBJECTS) $(CRTEND) $(CRTN) -o $(BUILDDIR)/kernel.stage1 -T $(BUILDDIR)/link.ld -lgcc -Wl,--export-dynamic

$(BUILDDIR)/%.o : %.S $(CONFIGFILE)
	@echo "[AS]  $@"
	@mkdir -p $(@D)
	@$(TOOLCHAIN_PREFIX)gcc $(ASFLAGS) -c $< -o $@ -MD -MF $(BUILDDIR)/$*.d

$(BUILDDIR)/%.o : %.c $(CONFIGFILE)
	@echo "[CC]  $@"
	@mkdir -p $(@D)
	@$(TOOLCHAIN_PREFIX)gcc $(CFLAGS) -c $< -o $@ -MD -MF $(BUILDDIR)/$*.d

clean:
	-rm -rf $(BUILDDIR)

od: $(BUILDDIR)/kernel
	$(TOOLCHAIN_PREFIX)objdump -dS $(BUILDDIR)/kernel

re: $(BUILDDIR)/kernel
	$(TOOLCHAIN_PREFIX)readelf -a $(BUILDDIR)/kernel

$(BUILDDIR)/hd.img: $(USRPROGS)
	@-sudo umount $(BUILDDIR)/mnt
	truncate -s 1GB $(BUILDDIR)/hd.img
	mke2fs -F $(BUILDDIR)/hd.img
	mkdir -p $(BUILDDIR)/mnt
	sudo mount $(BUILDDIR)/hd.img $(BUILDDIR)/mnt
	sudo cp $(BUILDDIR)/kernel $(BUILDDIR)/mnt/
	sudo umount $(BUILDDIR)/mnt


.PHONY: od re clean all test newproj userspace

