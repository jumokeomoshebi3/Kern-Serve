#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/string.h>
#include <linux/fs.h> 
#include <linux/slab.h> 
#include <net/sock.h>
#include <linux/kthread.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kern-Serve: Your favorite web server!!");
MODULE_AUTHOR("Jumi & Aaron");

#define KS_PORT         80
#define MYNAME          "Kern-Serve"
#define LOG             MYNAME ": "
#define PRETTY_THREAD_NAME MYNAME "/listen_thread"
#define MAX_REQUEST_SIZE   4096
#define MAX_FILE_SIZE      65536   /* 64KB cap on files we'll serve */
#define SERVE_DIR          "/etc/kern-serve"

static struct socket     *ks_sock;
struct socket            *conn_from_client;
static struct task_struct *listen_thread;
char *request_target;

/* function prototypes */
static int  handle_http_response(void);
static int  parse_http_request(char *request);
static int  handle_http_request(void);
static int  listen_loop(void *data);
static void close_socket(void);
static char *read_file(const char *path, loff_t *file_size_out);

/*  read_file */
static char *read_file(const char *path, loff_t *file_size_out)
{
    struct file *f;
    char        *buf;
    loff_t       pos = 0;
    ssize_t      bytes_read;

    *file_size_out = 0;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f)) {
        printk(KERN_INFO LOG "filp_open failed for %s (code %ld)\n",
               path, PTR_ERR(f));
        return NULL;
    }
    buf = kmalloc(MAX_FILE_SIZE, GFP_KERNEL);
    if (!buf) {
        printk(KERN_ERR LOG "kmalloc failed for file buffer\n");
        filp_close(f, NULL);
        return NULL;
    }
    bytes_read = kernel_read(f, buf, MAX_FILE_SIZE - 1, &pos);
    filp_close(f, NULL);

    if (bytes_read < 0) {
        printk(KERN_ERR LOG "kernel_read failed for %s (code %zd)\n",
               path, bytes_read);
        kfree(buf);
        return NULL;
    }

    buf[bytes_read] = '\0';   /* null-terminate so it's safe to strlen */
    *file_size_out  = bytes_read;

    printk(KERN_INFO LOG "Read %zd bytes from %s\n", bytes_read, path);
    return buf;
}

/* Modified handle_http_response, finds either a 200 OK with file contents or 400 */
static int handle_http_response(void)
{
    struct msghdr *message;
    struct kvec   *vec;
    char          *header_buf;
    char          *file_buf   = NULL;
    char           filepath[512];
    loff_t         file_size  = 0;
    int            ret;


    /*build the file path from request_target*/
    if (!request_target || strcmp(request_target, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", SERVE_DIR);
    } else {
        if (strstr(request_target, "..")) {
            printk(KERN_WARN LOG "Rejected suspicious request_target: %s\n",
                   request_target);
            request_target = "/index.html";
        }
        snprintf(filepath, sizeof(filepath), "%s%s", SERVE_DIR, request_target);
    }

    printk(KERN_INFO LOG "Looking for file: %s\n", filepath);

    /* try to read the file */
    file_buf = read_file(filepath, &file_size);

    /* allocate header buffer */
    header_buf = kmalloc(256, GFP_KERNEL);
    if (!header_buf) {
        printk(KERN_ERR LOG "kmalloc failed for header_buf\n");
        if (file_buf) kfree(file_buf);
        return -1;
    }

    if (file_buf) { /* if file is found build a 200 page , content-length is set dynamically from file size */
        snprintf(header_buf, 256,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html\r\n"
                 "Content-Length: %lld\r\n"
                 "Connection: close\r\n"
                 "Server: Kern-Serve\r\n"
                 "\r\n",
                 file_size);
    } else {
        /* if the file is not found, build a 404 page */
        const char *body_404 =
            "<html><head><title>404 Not Found</title></head>"
            "<body><h1>404 Not Found</h1>"
            "<p>Kern-Serve could not find that file.</p>"
            "</body></html>";

        snprintf(header_buf, 256,
                 "HTTP/1.1 404 Not Found\r\n"
                 "Content-Type: text/html\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "Server: Kern-Serve\r\n"
                 "\r\n",
                 strlen(body_404));
        /* replace file_buf with the 404 body so the logic works */
        file_buf  = kmalloc(strlen(body_404) + 1, GFP_KERNEL);
        if (!file_buf) {
            printk(KERN_ERR LOG "kmalloc failed for 404 body\n");
            kfree(header_buf);
            return -2;
        }
        strcpy(file_buf, body_404);
        file_size = strlen(body_404);
    }


    /* Allocating the message and vec*/
    message = kmalloc(sizeof(*message), GFP_KERNEL);
    if (!message) {
        printk(KERN_ERR LOG "Error allocating memory for message\n");
        kfree(header_buf);
        kfree(file_buf);
        return -3;
    }
    message->msg_name     = 0;
    message->msg_namelen  = 0;
    message->msg_control  = NULL;
    message->msg_controllen = 0;
    message->msg_flags    = MSG_DONTWAIT;

    vec = kmalloc(sizeof(*vec), GFP_KERNEL);
    if (!vec) {
        printk(KERN_ERR LOG "Error allocating memory for vec\n");
        kfree(header_buf);
        kfree(file_buf);
        kfree(message);
        return -4;
    }

    if (conn_from_client == NULL) {
        printk(KERN_ERR LOG "conn_from_client is NULL, cannot send response\n");
        kfree(header_buf);
        kfree(file_buf);
        kfree(vec);
        kfree(message);
        return -5;
    }

    /* sending the header first */
    vec->iov_base = header_buf;
    vec->iov_len  = strlen(header_buf);

    do {
        ret = kernel_sendmsg(conn_from_client, message, vec, 1, vec->iov_len);
    } while (ret == -EAGAIN || ret == -ERESTARTSYS);

    if (ret < 0) {
        printk(KERN_ERR LOG "Error sending header (code %d)\n", ret);
        kfree(header_buf);
        kfree(file_buf);
        kfree(vec);
        kfree(message);
        return -6;
    }

    /* send the body */
    vec->iov_base = file_buf;
    vec->iov_len  = file_size;

    do {
        ret = kernel_sendmsg(conn_from_client, message, vec, 1, vec->iov_len);
    } while (ret == -EAGAIN || ret == -ERESTARTSYS);

    if (ret < 0) {
        printk(KERN_ERR LOG "Error sending body (code %d)\n", ret);
    } else {
        printk(KERN_INFO LOG "Response sent! (%lld bytes)\n", file_size);
    }

    kfree(header_buf);
    kfree(file_buf);
    kfree(vec);
    kfree(message);
    return 0;
}


/* copy and pasting phase 4.5 */
static void close_socket(void) {
    if (ks_sock) {
        sock_release(ks_sock);
        ks_sock = NULL;
    }
}

static int parse_http_request(char *request) {
    char *http_method = NULL;
    int   counter     = 1;
    char *tokens      = strsep(&request, " ");

    while (tokens != NULL && counter <= 2) {
        if (counter == 1) {
            http_method = kmalloc(strlen(tokens) + 1, GFP_KERNEL);
            strcpy(http_method, tokens);
        } else if (counter == 2) {
            request_target = kmalloc(strlen(tokens) + 1, GFP_KERNEL);
            strcpy(request_target, tokens);
        }
        tokens = strsep(&request, " ");
        counter++;
    }

    if (http_method && strlen(http_method) <= 0) {
        printk(KERN_ERR LOG "Could not pull HTTP method from message.\n");
        kfree(http_method);
        return -1;
    }
    if (strlen(request_target) <= 0) {
        printk(KERN_ERR LOG "Could not pull request target from message.\n");
        kfree(http_method);
        return -2;
    }
    if (strcmp(http_method, "GET") != 0) {
        printk(KERN_INFO LOG "Unsupported HTTP Method %s\n", http_method);
        kfree(http_method);
        return -3;
    } else {
        printk(KERN_INFO LOG "Proceeding with %s request\n", http_method);
    }

    kfree(http_method);
    return 0;
}

static int handle_http_request(void) {
    struct msghdr *message;
    struct kvec   *vec;
    char          *buff;
    int            ret;

    message = kmalloc(sizeof(*message), GFP_KERNEL);
    if (!message) { printk(KERN_ERR LOG "kmalloc failed for message\n"); return -1; }
    message->msg_name     = 0;
    message->msg_namelen  = 0;
    message->msg_control  = NULL;
    message->msg_controllen = 0;
    message->msg_flags    = MSG_DONTWAIT;

    vec = kmalloc(sizeof(*vec), GFP_KERNEL);
    if (!vec) { printk(KERN_ERR LOG "kmalloc failed for vec\n"); kfree(message); return -2; }

    buff = kmalloc(MAX_REQUEST_SIZE, GFP_KERNEL);
    if (!buff) { printk(KERN_ERR LOG "kmalloc failed for buff\n"); kfree(vec); kfree(message); return -3; }

    vec->iov_len  = MAX_REQUEST_SIZE;
    vec->iov_base = buff;

    if (conn_from_client == NULL) {
        printk(KERN_ERR LOG "conn_from_client is NULL\n");
        kfree(buff); kfree(vec); kfree(message);
        return -4;
    }

    do {
        ret = kernel_recvmsg(conn_from_client, message, vec, 1, vec->iov_len, MSG_DONTWAIT);
    } while (ret == -EAGAIN || ret == -ERESTARTSYS);

    if (ret < 0) {
        printk(KERN_ERR LOG "Error receiving message (code %d)\n", ret);
        kfree(buff); kfree(vec); kfree(message);
        return -5;
    }

    printk(KERN_INFO LOG "Message received!\n");
    printk(KERN_INFO LOG "\n%s\n", buff);

    if (parse_http_request(buff) < 0)
        printk(KERN_ERR LOG "ERROR from parse_http_request\n");

    printk(KERN_INFO "Received request target %s\n", request_target);

    kfree(buff); kfree(vec); kfree(message);
    return 0;
}

static int listen_loop(void *data) {
    struct sockaddr_in client_addr;
    int ret;

    printk(KERN_INFO LOG "Listen thread started\n");
    set_user_nice(current, 5);

    while (!kthread_should_stop()) {
        conn_from_client = NULL;
        ret = kernel_accept(ks_sock, &conn_from_client, 0);

        if (ret == -ERESTARTSYS) {
            printk(KERN_INFO LOG "kernel_accept received signal interrupt.\n");
            continue;
        } else if (ret < 0) {
            printk(KERN_ERR LOG "kernel_accept error (code %d)\n", ret);
            continue;
        }

        ret = kernel_getpeername(conn_from_client, (struct sockaddr *)&client_addr);
        if (ret < 0)
            printk(KERN_INFO LOG "connection accepted, but error displaying address (code %d).\n", ret);
        else
            printk(KERN_INFO LOG "connection accepted from %pI4\n", &client_addr.sin_addr);

        handle_http_request();
        handle_http_response();
        sock_release(conn_from_client);
    }

    printk(KERN_INFO LOG "Listen thread end.\n");
    return 0;
}

static int __init ks_init(void) {
    struct sockaddr_in addr;
    int ret;

    printk(KERN_INFO LOG "Loading " MYNAME "\n");

    ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &ks_sock);
    if (ret < 0) { printk(KERN_ERR LOG "sock_create_kern failed (code %d)\n", ret); return ret; }

    printk(KERN_INFO LOG "Socket created\n");
    sock_set_reuseaddr(ks_sock->sk);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(KS_PORT);

    ret = kernel_bind(ks_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) { printk(KERN_ERR LOG "kernel_bind failed (code %d)\n", ret); close_socket(); return ret; }
    printk(KERN_INFO LOG "Socket bound to port %d\n", KS_PORT);

    ret = kernel_listen(ks_sock, 10);
    if (ret < 0) { printk(KERN_ERR LOG "kernel_listen failed (code %d)\n", ret); close_socket(); return ret; }
    printk(KERN_INFO LOG "Listening on port %d\n", KS_PORT);

    listen_thread = kthread_run(listen_loop, NULL, PRETTY_THREAD_NAME);
    if (IS_ERR(listen_thread)) {
        printk(KERN_ERR LOG "Failed to spawn listen thread (code %ld)\n", PTR_ERR(listen_thread));
        close_socket();
        return PTR_ERR(listen_thread);
    }
    printk(KERN_INFO LOG PRETTY_THREAD_NAME " created. PID %d\n", task_pid_nr(listen_thread));
    return 0;
}

static void __exit ks_exit(void) {
    if (listen_thread && !IS_ERR(listen_thread)) {
        kthread_stop(listen_thread);
        listen_thread = NULL;
        printk(KERN_INFO LOG PRETTY_THREAD_NAME " terminated.\n");
    }
    if (ks_sock) {
        sock_release(ks_sock);
        ks_sock = NULL;
        printk(KERN_INFO LOG "ks_sock released\n");
    }
    if (conn_from_client) {
        sock_release(conn_from_client);
        conn_from_client = NULL;
        printk(KERN_INFO LOG "conn_from_client released\n");
    }
    kfree(request_target);
    printk(KERN_INFO LOG "unloaded.\n");
}

module_init(ks_init);
module_exit(ks_exit);


/* 
*Jumi's comments/decisions 
* read_file() as a separate function allows handle_http_response to stay readbale 
* read_file can be reused 
*
* flip_open opens a file descriptor in the kernel's VFS layer 
*  kernel_read reads from it and left_tpos variable tracts its position in the file
*
*I set the max file size to be 64KB so we dont have to worry about files that are too large for the kernel memory
* I sent the header and body separately so the body can be dynamic 
* I used %lld with file_size so the header is always separate no matter what file is served
* 
*/