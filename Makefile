obj-m += snd_mcp49x1_gpio.o

KSRC ?= /root/luckfox-pico/sysdrv/source/kernel
KOBJ ?= /root/luckfox-pico/sysdrv/source/objs_kernel
ARCH ?= arm
CROSS_COMPILE ?= arm-rockchip830-linux-uclibcgnueabihf-
STRIP ?= arm-rockchip830-linux-uclibcgnueabihf-strip

all:
	$(MAKE) -C $(KSRC) O=$(KOBJ) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

strip: all
	$(STRIP) --strip-unneeded snd_mcp49x1_gpio.ko || true

clean:
	$(MAKE) -C $(KSRC) O=$(KOBJ) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f Module.symvers modules.order

.PHONY: all strip clean
