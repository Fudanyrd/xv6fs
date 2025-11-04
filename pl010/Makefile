TARGET_MODULE := pl010

# If we are running by kernel building system
LINUX_DIR = /home/yrd/linux-6.12.43
ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := kmod.o
	obj-m := $(TARGET_MODULE).o

# If we running without kernel build system
else
	BUILDSYSTEM_DIR := $(LINUX_DIR)
	PWD:=$(shell pwd)

# run kernel build system to make module
all : 
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules

# run kernel build system to cleanup in current directory
clean:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean
endif

