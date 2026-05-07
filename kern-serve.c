#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/string.h>
#include <net/sock.h>
#include <linux/kthread.h>
#include <linux/fs.h> 
#include <linux/slab.h> 
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kern-Serve: Your favorite web server!!");
MODULE_AUTHOR("Jumi & Aaron");

#define KS_PORT                   80
#define MYNAME                    "Kern-Serve"
#define LOG                       MYNAME ": "
#define PRETTY_THREAD_NAME        MYNAME "/listen_thread"
#define SERVE_DIR                 "/etc/kern-serve"

/* Just make a big size for messages */
#define MAX_REQUEST_SIZE          4096
/* 64KB cap on files we'll serve */
#define MAX_FILE_SIZE             65536   

static struct socket *ks_sock;
static struct socket *conn_from_client;
static struct task_struct *listen_thread;
static char* request_target;
static char* http_method = NULL;

/*
 * We put our function prototypes up top here just 
 * because, honestly, I prefer it that way.
 */
 static int determine_response(char *request_target);
 
static int handle_http_response(char *response);

static int parse_http_request(char* request);

static int handle_http_request(void);

static int listen_loop(void *data);

static void close_socket(void);

static int __init ks_init(void) {
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
    
    sock_set_reuseaddr(ks_sock->sk);

    /* Bind socket to port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(KS_PORT);

    ret = kernel_bind(ks_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        printk(KERN_ERR LOG "kernel_bind failed to bind socket (code %d)\n", ret);
        close_socket();
        return ret;
    }
    printk(KERN_INFO LOG "Socket bound to port %d\n", KS_PORT);

   /* Listen on port */
   /* (we allow 10 pending connections) */
    ret = kernel_listen(ks_sock, 10);
    if (ret < 0) {
        printk(KERN_ERR LOG "kernel_listen failed (code %d)\n", ret);
        close_socket();
        return ret;
    }
    printk(KERN_INFO LOG "Listening on port %d\n", KS_PORT);
    
    /* Now we need a thread to handle the listening logic */
    listen_thread = kthread_run(listen_loop, NULL, PRETTY_THREAD_NAME);
    if (IS_ERR(listen_thread)) {
        printk(KERN_ERR LOG "Failed to spawn listen thread (code %ld)\n", PTR_ERR(listen_thread));
        close_socket();
        return PTR_ERR(listen_thread);
    }
    printk(KERN_INFO LOG PRETTY_THREAD_NAME " successfully created. PID %d\n", task_pid_nr(listen_thread));

    return 0;
}

static void __exit ks_exit(void) {
    // Gracefully terminate the thread if it's still running.
    if (listen_thread && !IS_ERR(listen_thread)) {
        kthread_stop(listen_thread);
        listen_thread = NULL;
        printk(KERN_INFO LOG PRETTY_THREAD_NAME " terminated.\n");
    }
    
    // We do the sockets after the thread, since the thread
    // uses the sockets.
    if (ks_sock) {
        sock_release(ks_sock);
        ks_sock = NULL;
        printk(KERN_INFO LOG "ks_sock socket released\n");
    }
    
    
    
    
    printk(KERN_INFO LOG "unloaded.\n");
}

module_init(ks_init);
module_exit(ks_exit);

static void close_socket(void) {
    if (ks_sock) {
        sock_release(ks_sock);
        ks_sock = NULL;
    }
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
         * I noticed -512 (ERESTARTSYS) is the error code returned when the
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
        
        sock_release(conn_from_client);
        conn_from_client = NULL;
    }
    
    
    printk(KERN_INFO LOG "Listen thread end.\n");
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
    if (!message) {
        printk(KERN_ERR LOG "Error allocating memory for message\n");
        kfree(message);
        return -1;
    }
    
    /* Set up message */
    message->msg_name = 0;
    message->msg_namelen = 0;
    message->msg_control = NULL;
    message->msg_controllen = 0;
    message->msg_flags = MSG_DONTWAIT;
    
    /* We need to allocate for vec similar to how we did message */
    vec = kmalloc(sizeof(*vec), GFP_KERNEL);
    if (!vec) {
        printk(KERN_ERR LOG "Error allocating memory for vec\n");
        kfree(vec);
        kfree(message);
        return -2;
    }
    
    /* And now we need to allocate buff as well */
    buff = kmalloc(MAX_REQUEST_SIZE, GFP_KERNEL);
    if (!buff) {
        printk(KERN_ERR LOG "Error allocating memory for buff\n");
        kfree(vec);
        kfree(buff);
        kfree(message);
        return -3;
    }
    
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
        return -4;
    }
    
    /*
     * Now that we have all those pesky variables set up, we 
     * can finally receive a message
     */
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
    
    printk(KERN_INFO LOG "Message recieved!\n");
    printk(KERN_INFO LOG "\n%s\n", buff);
    
    ret = parse_http_request(buff);
    if (ret < 0) {
        printk(KERN_ERR LOG "ERROR received from parse_http_request\n");
    }
    
    kfree(vec);
    kfree(buff);
    kfree(message);
    return 0;
}

static int parse_http_request(char* request) {
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
            if (!http_method) {
                printk(KERN_ERR LOG "Failure allocation memory for http_method\n.");
                return -1;
            }
            strcpy(http_method, tokens);
            printk(KERN_INFO "Method: %s\n", http_method);
        }
        else if (counter == 2) {
            request_target = kmalloc(strlen(tokens) + 1, GFP_KERNEL);
            if (!request_target) {
                kfree(http_method);
                printk(KERN_ERR LOG "Failure allocation memory for request_target\n.");
                return -1;
            }
            strcpy(request_target, tokens);
            printk(KERN_INFO "Request Target: %s\n", request_target);
        }
        tokens = strsep(&request, " ");
        counter++;
    }
    
    // We shouldn't encounter this, but string parsing gets weird.
    if (!http_method || strlen(http_method) <= 0) {
        printk(KERN_ERR LOG "Could not pull HTTP method from message.\n");
        kfree(http_method);
        kfree(request_target);
        return -1;
    }
    if (!request_target || strlen(request_target) <= 0) {
        printk(KERN_ERR LOG "Could not pull request target from message.\n");
        kfree(http_method);
        kfree(request_target);
        return -2;
    }
    
    /*
     * I'm only pulling the method out because for a simple server like this,
     * we should only accept GET requests. 
     */
     /* https://www.geeksforgeeks.org/c/strcmp-in-c/ */
    if (strcmp(http_method, "GET") != 0) {
        printk(KERN_INFO LOG "Unsupported HTTP Method %s\n", http_method);
        handle_http_response("HTTP/1.1 501 Not Implemented\r\n");
        kfree(http_method);
        kfree(request_target);
        return -3;
    }
    
    printk(KERN_INFO LOG "Proceeding with %s request\n", http_method);
    
    determine_response(request_target);

    kfree(request_target);
    kfree(http_method);
    return 0;
}

static int handle_http_response(char *response) {
    struct msghdr *message;
    struct kvec *vec;
    
    if (!response) {
        printk(KERN_ERR LOG "No text for me to send as a response.\n");
        return -5;
    }
    
    int content_length = strlen(response);
    
    printk(KERN_INFO LOG "Content-Length %d\n", content_length);
    printk(KERN_INFO LOG "Content %s\n", response);
    
    int ret;

    message = kmalloc(sizeof(*message), GFP_KERNEL);
    if (!message) {
        printk(KERN_ERR LOG "Error allocating memory for message\n");
        kfree(message);
        return -1;
    }
    
    message->msg_name = 0;
    message->msg_namelen = 0;
    message->msg_control = NULL;
    message->msg_controllen = 0;
    message->msg_flags = MSG_DONTWAIT;
    
    vec = kmalloc(sizeof(*vec), GFP_KERNEL);
    if (!vec) {
        printk(KERN_ERR LOG "Error allocating memory for vec\n");
        kfree(vec);
        kfree(message);
        return -2;
    }
    
    vec->iov_len = strlen(response);
    vec->iov_base = response;
    
    if (conn_from_client == NULL) {
        printk(KERN_ERR LOG "TCP send socket (conn_from_client) is NULL, cannot handle connection.\n");
        kfree(vec);
        kfree(message);
        return -3;
    }
    
    ret = kernel_sendmsg(
        conn_from_client,
        message,
        vec,
        1,
        vec->iov_len
    );
    if (ret < 0) {
        printk(KERN_ERR LOG "Error sending response, (code %d)\n", ret);
        kfree(vec);
        kfree(message);
        return -4;
    }
    
    printk(KERN_INFO LOG "Response sent!\n\n");
    
    kfree(vec);
    kfree(message);
    return 0;
}

static int determine_response(char *request_target) {
    char           filepath[512];
    char          *file_buffer;
    loff_t         pos  = 0;
    struct file   *file       = NULL;
    ssize_t        bytes_read;
    
    /*build the file path from request_target*/
    if (!request_target || strcmp(request_target, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", SERVE_DIR);
    }
    else {
        if (strstr(request_target, "..")) {
            printk(KERN_WARNING LOG "Rejected suspicious request_target: %s\n", request_target);
            request_target = "/index.html";
        }
        snprintf(filepath, sizeof(filepath), "%s%s", SERVE_DIR, request_target);
    }

    printk(KERN_INFO LOG "Looking for file: %s\n", filepath);
    
    /* try to read the file */
    /* https://stackoverflow.com/questions/1184274/read-write-files-within-a-linux-kernel-module */
    /* filp: https://elixir.bootlin.com/linux/v6.12.74/source/fs/open.c#L1365 */
    /* https://www.man7.org/linux/man-pages/man2/open.2.html */
    /* For mode, though I did intuit 0600 https://www.man7.org/linux/man-pages/man3/mode_t.3type.html */
    file = filp_open(filepath, O_RDONLY, 0600);
    if (IS_ERR(file)) {
        if (PTR_ERR(file) == -2) {
            printk(KERN_ERR LOG "File not found, return 404 (code %ld)\n", PTR_ERR(file));
            handle_http_response("HTTP/1.1 404 Not Found\r\n");
            return 404;
        }
        else {
            printk(KERN_ERR LOG "File error (code %ld)\n", PTR_ERR(file));
            handle_http_response("HTTP/1.1 500 Internal Server Error\r\n");
            return 500;
        }
    }
    
    // We get a compiler warning if we try to do:
    // file_buffer = char[4096];
    // "the frame size of 4624 bytes is larger than 2048 bytes"
    // I think that means because we're trying to put all that space on the stack
    // So we kmalloc() the space instead.
    file_buffer = kmalloc(MAX_FILE_SIZE, GFP_KERNEL);
    if (!file_buffer) {
        kfree(file_buffer);
        printk(KERN_ERR LOG "Failed to allocate space for file_buffer\n");
        handle_http_response("HTTP/1.1 500 Internal Server Error\r\n");
        filp_close(file, NULL);
        return 500;
    }
    
    /* https://www.baeldung.com/linux/kernel-module-read-write-files */
    bytes_read = kernel_read(file, file_buffer, MAX_FILE_SIZE, &pos);
    if (bytes_read < 0) {
        printk(KERN_ERR LOG "Failed to read file (code %ld)\n", bytes_read);
        filp_close(file, NULL);
        kfree(file_buffer);
        handle_http_response("HTTP/1.1 500 Internal Server Error\r\n");
        return 500;
    }
    
    // Null-terminate the buffer to safely print it
    if (bytes_read >= MAX_FILE_SIZE) {
        bytes_read = MAX_FILE_SIZE - 1;
    }
    file_buffer[bytes_read] = '\0';
    
    pr_info("Read %ld bytes:\n%s\n", bytes_read, file_buffer);
    
    // Now we want to get an HTTP GET request into a reponse
    char* response;
    
    // We need to allocate space for it. How big is it? bytes_read
    // + the size of the header. I counted, and it's 102 bytes.
    // But of course that varies based on the Content-Legnth
    // 124 bytes accounts for a length of up to 22 (decimal) digits. I think
    // that's sufficient.
    size_t response_size = bytes_read + 124;
    
    response = kmalloc(response_size, GFP_KERNEL);
    if (!response) {
        kfree(response);
        kfree(file_buffer);
        printk(KERN_ERR LOG "Failed to allocate memory for response\n");
        filp_close(file, NULL);
        handle_http_response("HTTP/1.1 500 Internal Server Error\r\n");
        return 500;
    }
    
    snprintf(
        response,
        response_size,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Server: Kern-Serve\r\n" 
        "\r\n"
        "%s\r\n",
        bytes_read,
        file_buffer
    );
        
    handle_http_response(response);
    
    kfree(response);
    kfree(file_buffer);
    filp_close(file, NULL);
    return 0;
    
}
