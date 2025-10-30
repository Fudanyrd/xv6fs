# source: <https://www.apriorit.com/dev-blog/195-simple-driver-for-linux-os>
TARGET_MODULE := xv6fs

# If we are running by kernel building system
LINUX_DIR = /home/yrd/linux-6.17.5
ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := init.o
	$(TARGET_MODULE)-objs += check.o
	$(TARGET_MODULE)-objs += fs.o
	obj-m := $(TARGET_MODULE).o

# If we running without kernel build system
else
	BUILDSYSTEM_DIR := $(LINUX_DIR)
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
CXXFLAGS += -fno-rtti -fno-exceptions
CXXFLAGS += -include /usr/include/errno.h

CXX = $(CROSS_COMPILE)g++
SCRIPTDEP = Makefile configure fixdep

# build native target.
%.n.o: %.cpp
	@echo ' CXX [N] ' $@ && $(CXX) $(CXXFLAGS) -c -o $@ $<

mkxv6: mkxv6.c Makefile fs.h
	@echo CCLD mkxv6 && gcc -Wall -Werror -g -O2 -o mkxv6 mkxv6.c

.check.o.cmd:
	@echo ' GEN     ' .check.o.cmd && echo > .check.o.cmd

check: check.n.o xv6check.cpp fs.n.o
	@echo ' CXX     ' check && $(CXX) xv6check.cpp fs.n.o check.n.o $(ALLCXXFLAGS) -o check

check.o: check.cpp $(SCRIPTDEP) init.o
	@echo ' CXX [M] ' check.o && ./configure check && bash -ex .check.o.sh \
	&& ./fixdep check.o .check.o.cmd .check.o.d

fs.o: fs.cpp $(SCRIPTDEP) init.o
	@echo ' CXX [M] ' fs.o && ./configure fs \
	'-include $(LINUX_DIR)/include/uapi/asm-generic/errno.h' \
	&& bash -ex .fs.o.sh \
	&& ./fixdep fs.o .fs.o.cmd .fs.o.d

