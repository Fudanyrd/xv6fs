# source: <https://www.apriorit.com/dev-blog/195-simple-driver-for-linux-os>
TARGET_MODULE := xv6fs

# If we are running by kernel building system
ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := init.o
	obj-m := $(TARGET_MODULE).o

# If we running without kernel build system
else
	BUILDSYSTEM_DIR := /home/liuyu/Desktop/linux-6.17.3
	PWD:=$(shell pwd)

# run kernel build system to make module
all : 
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules

# run kernel build system to cleanup in current directory
clean:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean && rm -f mkxv6 check
endif

CXXFLAGS = -O2 -g
ALLCXXFLAGS = $(CXXFLAGS) -fno-pie

mkxv6: mkxv6.c Makefile fs.h
	@echo CCLD mkxv6 && gcc -Wall -Werror -g -O2 -o mkxv6 mkxv6.c

# check.o may be linked into kernel module. must compile in freestanding.
check.o: check.cpp Makefile check.h fs.h common.h
	@echo CXX check.o && gcc -c check.cpp -o check.o -ffreestanding \
	-nostdlib -mno-sse -fno-exceptions $(ALLCXXFLAGS) -fno-pie

check: check.o xv6check.cpp
	@echo CXX check && g++ -static -fno-pie xv6check.cpp check.o $(ALLCXXFLAGS) -o check
