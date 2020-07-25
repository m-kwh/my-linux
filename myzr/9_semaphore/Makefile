KERNELDIR := /home/myzr/my-work/02_source/linux-4.1.15
CURRENT_PATH := $(shell pwd)

obj-m := semaphore.o

build: kernel_modules

kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean