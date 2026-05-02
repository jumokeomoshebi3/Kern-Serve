#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/in.h>
#include <net/sock.h>
#include <linux/kthread.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kern-Serve Phase 3: Spawn a Kernel thread where the listening logic will happen");

#define KS_PORT                   80
#define MYNAME                    "Kern-Serve"
#define LOG                       MYNAME ": "
#define PRETTY_THREAD_NAME MYNAME "/listen_thread"

static struct socket *ks_sock            = NULL;
static struct task_struct *listen_thread = NULL;

static int listen_loop(void *data) {
    printk(KERN_INFO LOG "Listen thread started\n");
    
    // Set thread priority
    set_user_nice(current, 5); // Lower priority (higher nice value)
    
    while (!kthread_should_stop()) {
        // Phase 4 Goes here
        
        // https://stackoverflow.com/questions/20679228/linux-kernel-schedule-function
        // We call schedule() here otherwise this loop just consumes
        // all the scheduling time. It'll probably be fine once phase 4 is implemented
        // becase waiting for a network connection should serve a similar purpose.
        // btw, user ffr's discussion on that link is really interesting
        schedule();
    }
    
    
    printk(KERN_INFO LOG "Listen thread end.\n");
    return 0;
}

static void handle_socket_error(void) {
    sock_release(ks_sock);
    ks_sock = NULL;
}

static int __init ks_init(void)
{
    struct sockaddr_in addr;
    int ret;

    printk(KERN_INFO LOG "Loading " MYNAME "\n");

    /* Create the socket */
    ret = sock_create_kern(
            &init_net,
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            &ks_sock
    );
    if (ret < 0) {
        printk(KERN_ERR LOG "sock_create_kern FAILED (%d)\n", ret);
        return ret;
    }
    printk(KERN_INFO LOG "Socket created\n");

    /* Bind to port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(KS_PORT);

    ret = kernel_bind(ks_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        printk(KERN_ERR LOG "kernel_bind failed (%d)\n", ret);
        handle_socket_error();
        return ret;
    }
    printk(KERN_INFO LOG "Socket bound to port %d\n", KS_PORT);

   /* Listen on port */
    ret = kernel_listen(ks_sock, 10);
    if (ret < 0) {
        printk(KERN_ERR LOG "kernel_listen failed (%d)\n", ret);
        handle_socket_error();
        return ret;
    }
    printk(KERN_INFO LOG "Listening on port %d\n", KS_PORT);
    
    /* Now we need a thread to handle the listening logic */
    listen_thread = kthread_run(listen_loop, NULL, PRETTY_THREAD_NAME);
    if (IS_ERR(listen_thread)) {
        printk(KERN_ERR LOG "Failed to spawn listen thread (%ld)\n", PTR_ERR(listen_thread));
        return PTR_ERR(listen_thread);
    }
    printk(KERN_INFO LOG "Thread successfully created. PID %d\n", task_pid_nr(listen_thread));

    return 0;
}

static void __exit ks_exit(void)
{
    // Gracefully terminate the thread if it's still running.
    if (listen_thread && !IS_ERR(listen_thread)) {
        kthread_stop(listen_thread);
        listen_thread = NULL;
        printk(KERN_INFO LOG PRETTY_THREAD_NAME " terminated.\n");
    }
    
    // We do the socket after the thread, since the thread
    // uses the socket.
    if (ks_sock) {
        sock_release(ks_sock);
        ks_sock = NULL;
        printk(KERN_INFO LOG "socket released\n");
    }
    
    printk(KERN_INFO LOG "unloaded.\n");
}

module_init(ks_init);
module_exit(ks_exit);
