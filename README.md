# Instructions (so far)

Put these two files in the same directory.

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




---
## Prerequisites

```bash
sudo apt install make linux-headers-generic
```
