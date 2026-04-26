#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/in.h>
#include <net/sock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kern-Serve Phase 2: create, bind, and listen on a kernel socket");

#define KS_PORT 8080

static struct socket *ks_sock = NULL;

static int __init ks_init(void)
{
    struct sockaddr_in addr;
    int ret;

    printk(KERN_INFO "Kern-Serve: loading phase 2\n");

    /* creating the socket */
    ret = sock_create_kern(
            &init_net,
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            &ks_sock
    );
    if (ret < 0) {
        printk(KERN_ERR "Kern-Serve: sock_create_kern failed (%d)\n", ret);
        return ret;
    }
    printk(KERN_INFO "Kern-Serve: socket created\n");

    /* bind to port 8080 */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(KS_PORT);

    ret = kernel_bind(ks_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        printk(KERN_ERR "Kern-Serve: kernel_bind failed (%d)\n", ret);
        sock_release(ks_sock);
        ks_sock = NULL;
        return ret;
    }
    printk(KERN_INFO "Kern-Serve: socket bound to port %d\n", KS_PORT);

   
    ret = kernel_listen(ks_sock, 10);
    if (ret < 0) {
        printk(KERN_ERR "Kern-Serve: kernel_listen failed (%d)\n", ret);
        sock_release(ks_sock);
        ks_sock = NULL;
        return ret;
    }
    printk(KERN_INFO "Kern-Serve: listening on port %d — phase 2 complete\n", KS_PORT);

    return 0;
}

static void __exit ks_exit(void)
{
    if (ks_sock) {
        sock_release(ks_sock);
        ks_sock = NULL;
        printk(KERN_INFO "Kern-Serve: socket released\n");
    }
    printk(KERN_INFO "Kern-Serve: unloaded\n");
}

module_init(ks_init);
module_exit(ks_exit);