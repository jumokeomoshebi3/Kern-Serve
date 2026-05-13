# Kern-Serve

Put 'kern-serve.c' and 'Makefile'

As root, run

```bash
make
```

And then

```bash
insmod ./test-00.ko
```

In another shell, you can see the output with

```bash
sudo journalctl -f
```

or

```bash
sudo dmesg -w
```

Then, to unload the module

```bash
rmmod test-00
```


---
## Prerequisites

```bash
sudo apt install make linux-headers-generic
```
