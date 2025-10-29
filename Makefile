# source: <https://www.apriorit.com/dev-blog/195-simple-driver-for-linux-os>
TARGET_MODULE := xv6fs

# If we are running by kernel building system
ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := init.o
	$(TARGET_MODULE)-objs += check.o
	$(TARGET_MODULE)-objs += fs.o
	obj-m := $(TARGET_MODULE).o

# If we running without kernel build system
else
	BUILDSYSTEM_DIR := /home/yrd/linux-6.17.5
	PWD:=$(shell pwd)

# run kernel build system to make module
all : 
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules
	$(MAKE) -C $(PWD) check

# run kernel build system to cleanup in current directory
clean:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean && rm -f mkxv6 check
endif

CXXFLAGS = -O2 -g -Wall -Werror
ALLCXXFLAGS = $(CXXFLAGS) -fno-pie
# required when used in kernel.
CXXKFLAGS = -ffreestanding -nostdlib -mno-sse -fno-exceptions -fno-rtti
CXXKFLAGS += -mno-mmx -mno-sse2 -mno-3dnow -mno-avx
#  module: overflow in relocation type 10 val ffffffffc05fde4b
#  module: `xv6fs' likely not compiled with -mcmodel=kernel [😭]
CXXKFLAGS += -mcmodel=kernel

CXX = $(CROSS_COMPILE)g++

mkxv6: mkxv6.c Makefile fs.h
	@echo CCLD mkxv6 && gcc -Wall -Werror -g -O2 -o mkxv6 mkxv6.c

.check.o.cmd:
	@echo ' GEN     ' .check.o.cmd && echo > .check.o.cmd

include .fs.o.d
include .check.o.d

# check.o may be linked into kernel module. must compile in freestanding.
check.o .check.o.d: Makefile
	@echo ' CXX [M] ' check.o && $(CXX) -c check.cpp -o check.o $(CXXKFLAGS) $(ALLCXXFLAGS) \
	-MD -MF .check.o.d

check: check.o xv6check.cpp fs.o
	@echo ' CXX     ' check && $(CXX) -static -fno-pie xv6check.cpp fs.o check.o $(ALLCXXFLAGS) -o check


fs.o .fs.o.d: Makefile
	@echo ' CXX [M] ' fs.o && $(CXX) -c fs.cpp -o fs.o -MD -MF .fs.o.d \
	$(CXXKFLAGS) $(ALLCXXFLAGS) -include /usr/include/errno.h
