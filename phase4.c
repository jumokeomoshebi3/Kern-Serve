#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <net/sock.h>
#include <linux/kthread.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kern-Serve: Your favorite web server!!");
MODULE_AUTHOR("Jumi & Aaron");

#define KS_PORT                   80
#define MYNAME                    "Kern-Serve"
#define LOG                       MYNAME ": "
#define PRETTY_THREAD_NAME MYNAME "/listen_thread"
// Just make a big size for messages
#define MAX_REQUEST_SIZE          4096

static struct socket *ks_sock;
struct socket *conn_from_client;
static struct task_struct *listen_thread;
char* request_target;

static int handle_http_response(void) {
    /* kernel_sendmsg parameters */
    struct msghdr *message;
    struct kvec *vec;
    
    /* buff for kvec */
    char *response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 90\r\n"
        "Connection: close\r\n"
        "Server: Kern-Serve\r\n"
        "\r\n"
        "<html><head><title>Kern-Serve!</title></head><body><h1>It's Kern-Serve!</h1></body></html>";
    
    int ret;

    message = kmalloc(sizeof(*message), GFP_KERNEL);
    
    /* Set up message */
    message->msg_name = 0;
    message->msg_namelen = 0;
    message->msg_control = NULL;
    message->msg_controllen = 0;
    message->msg_flags = MSG_DONTWAIT;
    
    /* We need to set up vec similar to how we did message */
    vec = kmalloc(sizeof(*vec), GFP_KERNEL);
    
    /* Set up vec */
    vec->iov_len = strlen(response);
    vec->iov_base = response;
    
    // DON'T FORGET WE NEED TO FREE UP ALL THIS MEMORY LATER !!!
    /* https://www.kernel.org/doc/html/latest/core-api/memory-allocation.html */
    /* "Objects allocated by kmalloc can be freed by kfree or kvfree." */
    
    if (conn_from_client == NULL) {
        printk(KERN_ERR LOG "TCP send socket (conn_from_client) is NULL, cannot handle connection.\n");
        kfree(vec);
        kfree(message);
        return -1;
    }
    if (!response) {
        printk(KERN_ERR LOG "Error allocating memory for buff\n");
        kfree(vec);
        kfree(message);
        return -2;
    }
    if (!vec) {
        printk(KERN_ERR LOG "Error allocating memory for vec\n");
        kfree(vec);
        kfree(message);
        return -3;
    }
    if (!message) {
        printk(KERN_ERR LOG "Error allocating memory for message\n");
        kfree(vec);
        kfree(message);
        return -4;
    }
    
    do {
        ret = kernel_sendmsg(
            conn_from_client,
            message,
            vec,
            1,
            vec->iov_len
        );
    } while (ret == -EAGAIN || ret == -ERESTARTSYS);
    if (ret < 0) {
        printk(KERN_ERR LOG "Error sending response, (code %d)\n", ret);
        kfree(vec);
        kfree(message);
        return -5;
    }
    
    printk(KERN_INFO LOG "Response sent!\n\n");
    
    kfree(vec);
    kfree(message);
    return 0;
}

static int parse_http_request(char* request) {
    char* http_method;
    int counter = 1;
    
    /* https://stackoverflow.com/questions/2246618/can-i-use-strtok-in-a-linux-kernel-module#2246716 */
    /* https://www.delftstack.com/howto/c/strsep-in-c/ */
    /* https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Methods/GET */
    /* 
     * We only care about the first two tokens, which will give us the HTTP method
     * and the request-target
     */
    char* tokens = strsep(&request, " ");
    while (tokens != NULL && counter <= 2) {
        if (counter == 1) {
            http_method = kmalloc(strlen(tokens) + 1, GFP_KERNEL);
            strcpy(http_method, tokens);
            // printk(KERN_INFO "Method: %s\n", http_method);
        }
        else if (counter == 2) {
            request_target = kmalloc(strlen(tokens) + 1, GFP_KERNEL);
            strcpy(request_target, tokens);
            // printk(KERN_INFO "Request Target: %s\n", request_target);
        }
        tokens = strsep(&request, " ");
        counter++;
    }
    
    if (strlen(http_method) <= 0) {
        printk(KERN_ERR LOG "Could not pull HTTP method from message\n");
        kfree(http_method);
        return -1;
    }
    if (strlen(request_target) <= 0) {
        printk(KERN_ERR LOG "Could not pull request target from message\n");
        kfree(http_method);
        return -2;
    }
    
    /*
     * I'm only pulling the method out because for a simple server like this,
     * we should only accept GET requests. 
     */
     /* https://www.geeksforgeeks.org/c/strcmp-in-c/ */
    if (strcmp(http_method, "GET") != 0) {
        printk(KERN_INFO LOG "Unsupported HTTP Method %s\n", http_method);
        kfree(http_method);
        return -3;
    }
    else {
        printk(KERN_INFO LOG "Proceeding with %s request\n", http_method);
    }
    
    kfree(http_method);
    return 0;
}

static int handle_http_request(void) {
    /* kernel_recvmsg parameters */
    struct msghdr *message;
    struct kvec *vec;
    
    /* buff for kvec */
    char *buff;
    
    int ret;
    
    /* Before setting up message, we need to dynamically allocate memory for it */
    /* https://stackoverflow.com/questions/12958931/warning-x-may-be-used-uninitialized-in-this-function */
    /* We use kmalloc instead of malloc in the kernel */
    /* https://stackoverflow.com/questions/2888421/malloc-in-kernel */
    /* Why GFP_Kernel? "Most of the time GFP_KERNEL is what you need." */
    /* https://www.kernel.org/doc/html/latest/core-api/memory-allocation.html */
    message = kmalloc(sizeof(*message), GFP_KERNEL);
    
    /* Set up message */
    message->msg_name = 0;
    message->msg_namelen = 0;
    message->msg_control = NULL;
    message->msg_controllen = 0;
    message->msg_flags = MSG_DONTWAIT;
    
    /* We need to set up vec similar to how we did message */
    vec = kmalloc(sizeof(*vec), GFP_KERNEL);
    
    /* And now we need to set up buff as well */
    buff = kmalloc(MAX_REQUEST_SIZE, GFP_KERNEL);
    
    /* Set up vec */
    vec->iov_len = MAX_REQUEST_SIZE;
    vec->iov_base = buff;
    
    // DON'T FORGET WE NEED TO FREE UP ALL THIS MEMORY LATER !!!
    /* https://www.kernel.org/doc/html/latest/core-api/memory-allocation.html */
    /* "Objects allocated by kmalloc can be freed by kfree or kvfree." */
    
    if (conn_from_client == NULL) {
        printk(KERN_ERR LOG "TCP receive socket (conn_from_client) is NULL, cannot handle connection.\n");
        kfree(vec);
        kfree(buff);
        kfree(message);
        return -1;
    }
    if (!buff) {
        printk(KERN_ERR LOG "Error allocating memory for buff\n");
        kfree(vec);
        kfree(buff);
        kfree(message);
        return -2;
    }
    if (!vec) {
        printk(KERN_ERR LOG "Error allocating memory for vec\n");
        kfree(vec);
        kfree(buff);
        kfree(message);
        return -3;
    }
    if (!message) {
        printk(KERN_ERR LOG "Error allocating memory for message\n");
        kfree(vec);
        kfree(buff);
        kfree(message);
        return -4;
    }
    
    do {
        ret = kernel_recvmsg(
            conn_from_client,
            message,
            vec,
            1,
            vec->iov_len,
            MSG_DONTWAIT
        );
    } while (ret == -EAGAIN || ret == -ERESTARTSYS);
    if (ret < 0) {
        printk(KERN_ERR LOG "Error receiving message, (code %d)\n", ret);
        kfree(vec);
        kfree(buff);
        kfree(message);
        return -5;
    }
    
    printk(KERN_INFO LOG "Message recieved!\n\n");
    printk(KERN_INFO LOG "%s\n", buff);
    
    if (parse_http_request(buff) < 0) {
        printk(KERN_ERR LOG "ERROR received from parse_http_request\n");
    }
    printk(KERN_INFO "Received request target %s\n", request_target);
    
    kfree(vec);
    kfree(buff);
    kfree(message);
    return 0;
}

static int listen_loop(void *data) {
    struct sockaddr_in client_addr;
    int ret;
    
    printk(KERN_INFO LOG "Listen thread started\n");
    
    // Set thread priority
    set_user_nice(current, 5); // Lower priority (higher nice value)
    
    while (!kthread_should_stop()) {
        conn_from_client = NULL;
        
        ret = kernel_accept(ks_sock, &conn_from_client, 0);
        /*
         * I noticed -512 (ERESTARTSYS) is the error code return when the
         * function is waiting for a connection here and we rmmod. Figured we 
         * should handle this condition gracefully rather than printing an error.
         */
        if (ret == -ERESTARTSYS) {
            printk(KERN_INFO LOG "kernel_accept received signal interrupt.\n");
            continue;
        }
        else if (ret < 0) {
            printk(KERN_ERR LOG "kernel_accept error (code %d)\n", ret);
            continue;
        }
        
        /*
         * This is functionally unnecessary. Just for pretty logging.
         */
        ret = kernel_getpeername(conn_from_client, (struct sockaddr *)&client_addr);
        if (ret < 0) {
            printk(KERN_INFO LOG "connected accepted, but error displaying address (code %d).\n", ret);
        }
        else {
            printk(KERN_INFO LOG "connected accepted from %pI4\n", &client_addr.sin_addr);
        }
        
        handle_http_request();
        handle_http_response();
        
        sock_release(conn_from_client);
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
        printk(KERN_ERR LOG "sock_create_kern failed to create socket (code %d)\n", ret);
        return ret;
    }
    printk(KERN_INFO LOG "Socket created\n");
    
    // Find source for this.
    sock_set_reuseaddr(ks_sock->sk);

    /* Bind socket to port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(KS_PORT);

    ret = kernel_bind(ks_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        printk(KERN_ERR LOG "kernel_bind failed to bind socket (code %d)\n", ret);
        handle_socket_error();
        return ret;
    }
    printk(KERN_INFO LOG "Socket bound to port %d\n", KS_PORT);

   /* Listen on port */
   /* (we allow 10 pending connections) */
    ret = kernel_listen(ks_sock, 10);
    if (ret < 0) {
        printk(KERN_ERR LOG "kernel_listen failed (code %d)\n", ret);
        handle_socket_error();
        return ret;
    }
    printk(KERN_INFO LOG "Listening on port %d\n", KS_PORT);
    
    /* Now we need a thread to handle the listening logic */
    listen_thread = kthread_run(listen_loop, NULL, PRETTY_THREAD_NAME);
    if (IS_ERR(listen_thread)) {
        printk(KERN_ERR LOG "Failed to spawn listen thread (code %ld)\n", PTR_ERR(listen_thread));
        return PTR_ERR(listen_thread);
    }
    printk(KERN_INFO LOG PRETTY_THREAD_NAME " successfully created. PID %d\n", task_pid_nr(listen_thread));

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
