# Kern-Serve

## Prerequisites

Make sure you have the proper packages installed.

```bash
sudo apt -y install make linux-headers-generic
```

### The `kern-serve` Directory

**As root**, create a directory call `/etc/kern-serve`.

```bash
mkdir /etc/kern-serve
```

You can put whatever HTML, CSS, Javascript files you like in there. Kern-Serve will treat `index.html` as its default page.

Here is a command you can copy and paste for ease of testing.

```bash
echo -e '<html><head><title>Kern-Serve</title></head><body><h1>It&apos;s Kern-Serve&excl;</h1></body></html>' > /etc/kern-serve/index.html
```

## Compilation

**Make sure to perform this as root.**

Put `kern-serve.c` and `Makefile` in the same directory.

Within that directory, run `make`.

```bash
make
```

### Error! Uh oh!

**NOTE:** The Makefile seems to *require* tab characters, *not* spaces. The version we've included here should include them, but it seems like sometimes moving the file around (i.e. pulling it from Github) will replace those tabs with space.

```
obj-m += kern-serve.o
all:
        make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
        make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

If you try to run `make` and you get an error like this,

```
Makefile:3: *** missing separator (did you mean TAB instead of 8 spaces?).  Stop.
```

**Edit the make file** and replace the spaces in front of lines 3 and 5 with tabs.

```
obj-m += kern-serve.o
all:
/* TAB HERE */make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
/* TAB HERE */make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

Then run `make` again.

### Insert into the kernel

Then, (still within the directory where you made the module) insert the module into the kernel.

```bash
insmod ./kern-serve.ko
```

## Test

You can see the output of the module using either of the following two commands.

```bash
sudo journalctl -f
```

```bash
sudo dmesg -w
```

You can test the module from your local box there like this.

```bash
curl http://localhost
```

If you want to see the HTTP code, you can run it like this.

```bash
curl -w "%{http_code}" http://localhost; echo
```

And you can request a fake page to see the 404 error.


```bash
curl -w "%{http_code}" http://localhost/some-fake-page; echo
```

You can even view it from a web browser if you set up the networking on your VM properly (that's outside the scope of this README; we'll leave that as an exercise for the enterprising tester). If you do, make sure to type `http://` into your web browser, NOT `https://`. Most browser will try to force HTTPS, but Kern-Serve only accepts HTTP requests on the standard port 80.

## Unload the Module

When you're done playing with it, (from within the directory where you made the module) you can unload the module from the kernel like this.

```bash
rmmod test-00
```
