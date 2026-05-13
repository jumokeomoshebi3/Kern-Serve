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
echo "<html><head><title>Kern-Serve</title></head><body>It's Kern-Serve!</body></html>" > /etc/kern-serve/index.html
```

## Compilation

**Make sure to perform this as root.**

Put `kern-serve.c` and `Makefile` in the same directory.

Run `make`.

```bash
make
```

### Insert into the kernel

Then, insert the module into the kernel.

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
curl -w "%{http_code}" http://localhost
```

And you can request a fake page to see the 404 error.


```bash
curl -w "%{http_code}" http://localhost/some-fake-page
```

You can even view it from a web browser if you set up the networking on your VM properly (that's outside the scope of this README; I'll leave that as an exercise for the interprising tester).

## Unload the Module

When you're done playing with it, you can unload the module from the kernel like this.

```bash
rmmod test-00
```
