obj-m += test-00.o
obj-m += phase 2.o
obj-m += phase 5.o
all:
        make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
        make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
