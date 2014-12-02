#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h> 
#include <pwd.h>

#include <magic.h>

#include "vrt.h"
#include "vre.h"
#include "vas.h"
#include "cache/cache.h"
#include "cache/cache_backend.h"

#include "vcc_if.h"

/* lightweight addrinfo */
struct vss_addr {
	int			 va_family;
	int			 va_socktype;
	int			 va_protocol;
	socklen_t		 va_addrlen;
	struct sockaddr_storage	 va_addr;
};

int VSS_listen(struct vss_addr *va, int depth);
int VSS_resolve(const char *addr, const char *port, struct vss_addr ***vap);

int
init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
    printf("vmod_static: init_function %p\n", priv);
    return (0);
}

VCL_VOID
vmod_print(const struct vrt_ctx *ctx, VCL_STRING name) {
    printf("%s\n", name);
}

VCL_STRING
vmod_hello(const struct vrt_ctx *ctx, VCL_STRING name)
{
    char *p;
    unsigned u, v;

    u = WS_Reserve(ctx->ws, 0); /* Reserve some work space */
    p = ctx->ws->f;		/* Front of workspace area */
    v = snprintf(p, u, "Hello, %s", name);
    v++;
    if (v > u) {
	/* No space, reset and leave */
	WS_Release(ctx->ws, 0);
	return (NULL);
    }
    /* Update work space with what we've used */
    WS_Release(ctx->ws, v);
    return (p);
}

struct vdi_simple {
        unsigned                magic;
#define VDI_SIMPLE_MAGIC        0x476d25b7
        struct director         dir;
        struct backend          *backend;
        const struct vrt_backend *vrt;
};

/*--------------------------------------------------------------------*/

#define WS_LEN        0x400000 // 4 MiB
#define HTTP1_BUF     0x200000 // 2 MiB
#define HTTP1_MAX_HDR 20
const char* DEFAULT_ROOT = "/var/tmp";

struct vmod_static_file_system {
        unsigned                 magic;
#define VMOD_STATIC_MAGIC    0x94874A52
        const char               *root;
        struct vdi_simple        *vs;
        int                      sock;
        struct vss_addr          **vss_addr;
        char                     port[6];
        char                     sockaddr_size;
        struct sockaddr_in       sockaddr;
        pthread_t                tp;
        struct worker            *wrk;
        struct http_conn         htc;
        magic_t                  magic_cookie;
        char                     *thread_name;
        char                     *ws_name;
};

static void
prepare_answer(struct http_conn *htc, int status)
{
  printf("  static.prepare_answer status=%d\n", status);
  char *message;

  switch (status) {
  case 200: message = "OK";                 break;
  case 204: message = "No Content";         break;
  case 302: message = "Moved Temporarily";  break;
  case 400: message = "Bad Request";        break;
  case 403: message = "Forbidden";          break;
  case 404: message = "Not Found";          break;
  case 405: message = "Method Not Allowed"; break;
  default:
    status = 500;
    message = "Internal Error";
  }
  dprintf(htc->fd, "HTTP/1.1 %d %s\r\n", status, message);
  dprintf(htc->fd, "Connection: close\r\n");
}

static void
prepare_body(struct http_conn *htc)
{
  dprintf(htc->fd, "\r\n");
}

static void
handle_file_error(struct http_conn *htc, int err) {
  printf("  static.handle_file_error %d\n", err);
  int status;

  switch (err) {
  case EACCES:
    status = 403;
    break;
  case ENAMETOOLONG:
  case EFAULT:
    status = 400;
    break;
  case ENOENT:
  case ENOTDIR:
    status = 404;
    break;
  default:
    status = 500;
  }

  prepare_answer(htc, status);
  prepare_body(htc);
}

static void
add_content_type(struct vmod_static_file_system *fs, const char *path)
{
  const char *mime = magic_file(fs->magic_cookie, path);
  // XXX how to free a string from magic_file or magic_error ?
  if (mime == NULL) {
    perror(magic_error(fs->magic_cookie));
    return;
  }
  dprintf(fs->htc.fd, "Content-Type: %s\r\n", mime);
  printf("  static.add_content_type %s\n", mime);
}

static void
send_response(struct vmod_static_file_system *fs, struct stat *stat_buf, const char *path)
{
    printf("  static.send_response path=%s\n", path);
    int fd;
    off_t offset = 0;
    ssize_t remaining = stat_buf->st_size;
    ssize_t written;

    fd = open(path, O_RDONLY);

    if(fd < 0) {
	handle_file_error(&fs->htc, errno);
	return;
    }

    prepare_answer(&fs->htc, 200);
    dprintf(fs->htc.fd, "Content-Length: %lu\r\n", stat_buf->st_size);
    add_content_type(fs, path);
    prepare_body(&fs->htc);

    while (remaining > 0) {
	written = sendfile(fs->htc.fd, fd, &offset, remaining);
	if (written < 0) {
	    perror("sendfile");
	    break;
	}
	remaining -= written;
    }

    // XXX too late for a 500 response...
    close(fd);
}

static const char *
absolutize_link(struct ws* ws, const char* path, const char* link)
{
    printf("  static.absolutize_link path=%s link=%s\n", path, link);
    unsigned available, written;
    char *front, *path_copy;

    if (link[0] == '/') {
	return link;
    }

    path_copy = (char*) WS_Copy(ws, path, -1);

    if (path_copy == NULL) {
	return NULL;
    }

    available = WS_Reserve(ws, 0);
    front = ws->f;

    written = snprintf(front, available, "%s/%s", dirname(path_copy), link);
    written++; // null-terminating char

    if (written > available) {
	WS_Release(ws, 0);
	return NULL;
    }

    WS_Release(ws, written);
    return front;
}

static const char *
normalize_link(struct vmod_static_file_system *fs, const char* link)
{
    printf("  static.normalize_link %s\n", link);
    char *real_path, *location;
    unsigned root_len;

    real_path = realpath(link, NULL);

    if (real_path == NULL) {
	return NULL;
    }

    root_len = strlen(fs->root);
    if (memcmp(real_path, fs->root, root_len) || real_path[root_len] != '/') {
	return NULL;
    }

    location = (char*) WS_Copy(fs->htc.ws, &real_path[root_len], -1);
    free(real_path);

    return location;
}

static void
send_redirect(struct vmod_static_file_system *fs, const char *path)
{
    printf("  static.send_redirect %s\n", path);
    int link_buf_size;
    char link_buf[4096]; // XXX hardcoded
    const char *absolute_link;
    char const *location;

    link_buf_size = readlink(path, link_buf, 4096);

    if(link_buf_size < 0) {
	handle_file_error(&fs->htc, errno);
	return;
    }

    if (link_buf[0] != '/') {
	absolute_link = absolutize_link(fs->htc.ws, path, link_buf);
	if (absolute_link == NULL) {
	    prepare_answer(&fs->htc, 500);
	    prepare_body(&fs->htc);
	    return;
	}
    }
    else {
	absolute_link = link_buf;
    }

    location = normalize_link(fs, absolute_link);
    if (location == NULL) {
	prepare_answer(&fs->htc, 500);
	prepare_body(&fs->htc);
	return;
    }

    prepare_answer(&fs->htc, 302);
    dprintf(fs->htc.fd, "Location: %s\r\n", location);
    prepare_body(&fs->htc);
}

static void
answer_file(struct vmod_static_file_system *fs, struct stat *stat_buf, const char *path)
{
  printf("  static.answer_file %s\n", path);
    mode_t mode = stat_buf->st_mode;

    if (S_ISREG(mode)) {
	if (stat_buf->st_size) {
	    send_response(fs, stat_buf, path);
	}
	else {
	    prepare_answer(&fs->htc, 204);
	    prepare_body(&fs->htc);
	}
    }
    else if (S_ISLNK(mode)) {
	send_redirect(fs, path);
    }
    else {
	prepare_answer(&fs->htc, 404);
	prepare_body(&fs->htc);
    }
}

static void
answer_appropriate(struct vmod_static_file_system *fs)
{
  printf("  static.answer_appropriate\n");
    unsigned available;
    char *url;
    char *url_start;
    char *url_end;
    size_t url_len;
    size_t root_len;
    char *path;
    struct stat stat_buf;

    if (strncmp("GET ", fs->htc.ws->s, 4)) {
	prepare_answer(&fs->htc, 405);
	prepare_body(&fs->htc);
	return;
    }

    url_start = &fs->htc.ws->s[4];
    url_end = strchr(url_start, ' ');
    url_len = (url_end - url_start) + strlen(fs->root) + 1;
    url = WS_Alloc(fs->htc.ws, url_len);
    snprintf(url, url_len, "%s%s", fs->root, url_start);

    path = url;
    if (lstat(path, &stat_buf) < 0) {
	handle_file_error(&fs->htc, errno);
	return;
    }

    answer_file(fs, &stat_buf, path);
}

static void *
server_bgthread(struct worker *wrk, void *priv)
{
    struct vmod_static_file_system *fs;
    struct sockaddr_storage addr_s;
    socklen_t len;
    struct http_conn *htc;
    int fd;
    enum htc_status_e htc_status;

    CAST_OBJ_NOTNULL(fs, priv, VMOD_STATIC_MAGIC);
    assert(fs->sock >= 0);

    htc = &fs->htc;
    fs->wrk = wrk;
    
    WS_Init(wrk->aws, fs->ws_name, malloc(WS_LEN), WS_LEN);

    while (1) {
	do {
	    fd = accept(fs->sock, (void*)&addr_s, &len);
	} while (fd < 0 && errno == EAGAIN);

	if (fd < 0) {
	    continue;
	}

	HTTP1_Init(htc, wrk->aws, fd, NULL, HTTP1_BUF, HTTP1_MAX_HDR);

	htc_status = HTTP1_Rx(htc);
	switch (htc_status) {
	case HTTP1_OVERFLOW:
	case HTTP1_ERROR_EOF:
	case HTTP1_ALL_WHITESPACE:
	case HTTP1_NEED_MORE:
	    prepare_answer(htc, 400);
	    prepare_body(htc);
	    break;
	case HTTP1_COMPLETE:
	    answer_appropriate(fs);
	    break;
	}

	WS_Reset(wrk->aws, NULL);
	close(fd);
    }

    pthread_exit(0);

    NEEDLESS_RETURN(NULL);
}

static void
server_start(struct vmod_static_file_system *fs)
{
    printf("server_start\n");
    struct vdi_simple *vs;
    const struct vrt_backend *be;

    vs = fs->vs;
    be = vs->vrt;

    AN(VSS_resolve(be->ipv4_addr, be->port, &fs->vss_addr));
    fs->sock = VSS_listen(fs->vss_addr[0], be->max_connections);
    printf("..fs->sock=%d\n", fs->sock);
    assert(fs->sock >= 0);

    WRK_BgThread(&fs->tp, fs->thread_name, server_bgthread, fs);
}

static magic_t
load_magic_cookie()
{
    magic_t magic_cookie = magic_open(MAGIC_NONE|MAGIC_MIME);

    AN(magic_cookie);

    if (magic_load(magic_cookie, NULL) < 0) {
	perror("magic_load");
	magic_close(magic_cookie);
	magic_cookie = NULL;
    }

    return magic_cookie;
}


VCL_VOID
vmod_file_system__init(const struct vrt_ctx *ctx,
    struct vmod_static_file_system **fsp,
    const char *vcl_name, VCL_BACKEND be, const char *root)
{
    fprintf(stderr, "vmod_static__init name=%s be=%p root=%s\n", vcl_name, be, root);
    fflush(stderr);
    struct vmod_static_file_system *fs;
    struct vdi_simple *vs;

    AN(ctx);
    AN(fsp);
    AN(vcl_name);
    AZ(*fsp);

    CHECK_OBJ_NOTNULL(be, DIRECTOR_MAGIC);
    CAST_OBJ_NOTNULL(vs, be->priv, VDI_SIMPLE_MAGIC);

    ALLOC_OBJ(fs, VMOD_STATIC_MAGIC);
    AN(fs);
    *fsp = fs;
    fs->vs = vs;

    fs->thread_name = malloc(sizeof("fsthread-")    + strlen(vcl_name));
    // fs->ws_name     = malloc(sizeof("fsworkspace-") + strlen(vcl_name));
    fs->ws_name     = malloc(strlen(vcl_name));

    AN(fs->thread_name);
    AN(fs->ws_name);

    sprintf(fs->thread_name, "fsthread-%s", vcl_name);
    // sprintf(fs->ws_name,  "fsworkspace-%s", vcl_name);
    sprintf(fs->ws_name,  "%s", vcl_name);

    AN(root);
    assert(root[0] == '\0' || root[0] == '/');

    if (root[0] == '\0') {
      // Default to /var/tmp
      printf("  defaulting to %s\n", DEFAULT_ROOT);
      fs->root = DEFAULT_ROOT;
    } else {
      fs->root = root;
    }


    fs->magic_cookie = load_magic_cookie();
    AN(fs->magic_cookie);

    server_start(fs);
}

VCL_VOID
vmod_file_system__fini(struct vmod_static_file_system **fsp)
{
    printf("vmod_static__fini\n");
    struct vmod_static_file_system *fs;
    void *res;

    // XXX It seems that the destructor is not called yet.
    //     A little reminder then...
    abort();

    fs = *fsp;
    *fsp = NULL;
    CHECK_OBJ_NOTNULL(fs, VMOD_STATIC_MAGIC);

    AZ(pthread_cancel(fs->tp));
    AZ(pthread_join(fs->tp, &res));
    assert(res == PTHREAD_CANCELED);

    magic_close(fs->magic_cookie);
    free(fs->thread_name);
    free(fs->ws_name);
    free(fs->wrk->aws);
    FREE_OBJ(fs->wrk);
    FREE_OBJ(fs);
}

