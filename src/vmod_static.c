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
#include "vrt.h"
#include "vre.h"
#include "vas.h"
#include "cache/cache.h"
#include "cache/cache_backend.h"

#include "vcc_if.h"

#include <Judy.h>
#include "cJSON.c"

#define DB_MIMETYPE "/etc/vmod-static/mimetypes.json"
Pvoid_t MIMETYPE_CACHE = (Pvoid_t) NULL;

static void init_mimetypes() {

    // Create simple db based on this => http://www.stdicon.com/mimetypes
    Word_t Bytes;
    JSLFA(Bytes, MIMETYPE_CACHE);

    int error = 0;
    FILE* file = fopen(DB_MIMETYPE, "r");
    if (file != NULL) {
	fseek(file, 0L, SEEK_END);
	uint sz = ftell(file);
	fseek(file, 0L, SEEK_SET);
	char* buf = malloc(sz+1);
	buf[sz] = 0;
	size_t sz_read = fread(buf, 1, sz, file);
	if (sz_read != sz) {
	    error++;
	    printf("WARN: mimetype db read failure\n");
	}
	cJSON *items = cJSON_Parse(buf);
	if (items != NULL) {
	    uint i;
	    uint item_len = cJSON_GetArraySize(items);
	    for (i=0; i < item_len; i++) {
		cJSON *item = cJSON_GetArrayItem(items, i);
		if (item->child != NULL) {
		    cJSON *child = item->child;
		    char* key = child->string;
		    char* val = child->valuestring;
		    if (key != NULL && val != NULL) {
			PWord_t PV;
			JSLI(PV, MIMETYPE_CACHE, key);
			*PV = (long)val;
		    }
		}
	    }
	} else {
	    error++;
	    printf("ERROR: Invalid mimetypes db: %s\n", DB_MIMETYPE);
	}
	free(buf);
	fclose(file);
    } else {
	fprintf(stderr, "WARN: Unable to find mimetypes: %s", DB_MIMETYPE);
    }
    // Clear cache if any error whatsoever
    if (error > 0) {
	Word_t Bytes;
	JSLFA(Bytes, MIMETYPE_CACHE);
    }
}

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

#if !defined(VRT_MINOR_VERSION) || !defined(VRT_MAJOR_VERSION) || \
    (VRT_MAJOR_VERSION < 2 || VRT_MINOR_VERSION < 2)
static struct http *
VRT_selecthttp(const struct vrt_ctx *ctx, enum gethdr_e where)
{
    struct http *hp;
    switch (where) {
    case HDR_REQ:
	hp = ctx->http_req;
	break;
    case HDR_BEREQ:
	hp = ctx->http_bereq;
	break;
    case HDR_BERESP:
	hp = ctx->http_beresp;
	break;
    case HDR_RESP:
	hp = ctx->http_resp;
	break;
#if !defined(VRT_MAJOR_VERSION) || (VRT_MAJOR_VERSION < 2)
    case HDR_OBJ:
	hp = ctx->http_obj;
	break;
#endif
    default:
	WRONG("VRT_selecthttp 'where' invalid");
    }
    return (hp);
}
#endif

/*
 * Returns true if the *hdr header is the one pointed to by *hh.
 *
 * FIXME: duplication from varnishd.
 */
static int
header_http_IsHdr(const txt *hh, const char *hdr)
{
    unsigned l;

    Tcheck(*hh);
    AN(hdr);
    l = hdr[0];
    assert(l == strlen(hdr + 1));
    assert(hdr[l] == ':');
    hdr++;
    int result = !strncasecmp(hdr, hh->b, l);
    // printf("header_http_IsHdr hdr=%s hh->b=%s len=%u => %u\n", hdr, hh->b, l, result);
    return result;
}

/*
 * Return true if the hp->hd[u] header matches *hdr
 */
static int
header_http_match(const struct vrt_ctx *ctx, const struct http *hp, unsigned u, const char *hdr)
{
    const char *start;
    unsigned l;
    assert(hdr);
    assert(hp);
    Tcheck(hp->hd[u]);
    if (hp->hd[u].b == NULL)
	return 0;
    l = hdr[0];
    if (!header_http_IsHdr(&hp->hd[u], hdr))
	return 0;
    return 1;
}

static void 
header_http_append(const struct vrt_ctx *ctx, struct http *hp, VCL_HEADER hdr, const char *fmt, ...)
{
    va_list ap;
    const char *b;
    va_start(ap, fmt);
    b = VRT_String(hp->ws, hdr->what + 1, fmt, ap);
    if (b == NULL)
	VSLb(ctx->vsl, SLT_LostHeader, "vmod_header: %s", hdr->what + 1);
    else
	http_SetHeader(hp, b);
    va_end(ap);
}

static char *
str_replace ( const char *string, const char *substr, const char *replacement, char* result ) {
    // printf("str_replace %s sub=%s rep=%s\n", string, substr, replacement);
    char *tok = NULL;
    char *oldstr = NULL;
    if ( substr == NULL || replacement == NULL ) return strdup (string);
    char *newstr = strdup(string);
    while ( (tok = strstr ( newstr, substr ))){
	oldstr = newstr;
	newstr = malloc ( strlen ( oldstr ) - strlen ( substr ) + strlen ( replacement ) + 1 );
	/*failed to alloc mem, free old string and return NULL */
	if ( newstr == NULL ){
	    free (oldstr);
	    return NULL;
	}
	memcpy ( newstr, oldstr, tok - oldstr );
	memcpy ( newstr + (tok - oldstr), replacement, strlen ( replacement ) );
	memcpy ( newstr + (tok - oldstr) + strlen( replacement ), tok + strlen ( substr ), strlen ( oldstr ) - strlen ( substr ) - ( tok - oldstr ) );
	memset ( newstr + strlen ( oldstr ) - strlen ( substr ) + strlen ( replacement ) , 0, 1 );
	free (oldstr);
    }
    // Allow caller to provide final buffer and not have to cleanup. Could segfault etc if buffer not big enough. Caller responsibilty.
    if (result != NULL) {
	strcpy(result, newstr);
	free(newstr);
	return result;
    }
    return newstr;
}

static void
header_http_cphdr(const struct vrt_ctx *ctx, struct http *hp, const char *hdr, VCL_HEADER dst, const char* substr, const char* replacement)
{
    if (hp == NULL) return;

    if (dst == NULL) {
    }

    unsigned u;
    const char *p = NULL;
    for (u = HTTP_HDR_FIRST; u < hp->nhd; u++) {
	if (!header_http_match(ctx, hp, u, hdr)) {
	    continue;
	}
	p = hp->hd[u].b + hdr[0];
	while (*p == ' ' || *p == '\t')
	    p++;

	if (substr != NULL) {
	    char* new_cookie = str_replace(p, substr, replacement, NULL);
	    header_http_append(ctx, hp, dst, new_cookie, vrt_magic_string_end);
	    free(new_cookie);
	} else {
	    header_http_append(ctx, hp, dst, p, vrt_magic_string_end);
	}
    }
}

VCL_VOID  __match_proto__()
    vmod_copy(const struct vrt_ctx *ctx, VCL_HEADER src, VCL_HEADER dst)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    struct http *hp = VRT_selecthttp(ctx, src->where);
    if (hp == NULL) return;
    header_http_cphdr(ctx, hp, src->what, dst, NULL, NULL);
}

VCL_VOID  __match_proto__()
    vmod_copy_and_replace(const struct vrt_ctx *ctx, VCL_HEADER src, VCL_HEADER dst, VCL_STRING substr, VCL_STRING replacement)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    struct http *hp = VRT_selecthttp(ctx, src->where);
    if (hp == NULL) return;
    header_http_cphdr(ctx, hp, src->what, dst, substr, replacement);
}

VCL_VOID  __match_proto__()
    vmod_replace(const struct vrt_ctx *ctx, VCL_HEADER src, VCL_STRING substr, VCL_STRING replacement)
{
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    struct http *hp = VRT_selecthttp(ctx, src->where);
    if (hp == NULL) return;
    // 1 - Create temporary header.
    struct gethdr_s temp_header = { src->where, "\011xxx-temp:"};
    // 2 - Copy original header performing substr replacements (no REGEX!)
    header_http_cphdr(ctx, hp, src->what, &temp_header, substr, replacement);
    // 3 - Unset all copies of original header
    VRT_SetHdr(ctx, src, vrt_magic_string_unset);
    // 4 - Copy temp header(s) to original(s)
    header_http_cphdr(ctx, hp, temp_header.what, src, NULL, NULL);
    // 5 - Remove temporary header(s)
    VRT_SetHdr(ctx, &temp_header, vrt_magic_string_unset);
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
    char                     *thread_name;
    char                     *ws_name;
};

static void
prepare_answer(struct http_conn *htc, int status)
{
    // printf("  static.prepare_answer status=%d\n", status);
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
    //printf("  static.handle_file_error %d\n", err);
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
    case ENOTDIR: {
	status = 404;
	break;
    }
    default:
	status = 500;
    }

    prepare_answer(htc, status);
    prepare_body(htc);
}


static void
add_content_type(struct vmod_static_file_system *fs, const char *path)
{
    if (fs != NULL) {
	// No libmagic. Done by determining apparent file extension and using that as key
	char* mimetype = NULL;
	char* key = strrchr(path,'.');
	if (key != NULL) {
	    PWord_t PV;
	    JSLG(PV, MIMETYPE_CACHE, key);
	    if (PV != NULL) {
		mimetype = (char*)*PV;
	    }
	}
	if (mimetype==NULL) mimetype = "application/octet-stream";
	// printf("  static.add_content_type %s => %s\n", path, mimetime);
	dprintf(fs->htc.fd, "Content-Type: %s\r\n", mimetype);
    }
}

static void
send_response(struct vmod_static_file_system *fs, struct stat *stat_buf, const char *path)
{
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
    add_content_type(fs, path +strlen(fs->root));
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
    else {
	prepare_answer(&fs->htc, 404);
	prepare_body(&fs->htc);
    }
}

static void
answer_appropriate(struct vmod_static_file_system *fs)
{
    //printf("  static.answer_appropriate\n");
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

    if (strstr(url, "/..") != NULL) {
	prepare_answer(&fs->htc, 400);
	prepare_body(&fs->htc);
	return;
    }

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
    struct vdi_simple *vs;
    const struct vrt_backend *be;
    vs = fs->vs;
    be = vs->vrt;
    AN(VSS_resolve(be->ipv4_addr, be->port, &fs->vss_addr));
    fs->sock = VSS_listen(fs->vss_addr[0], be->max_connections);
    assert(fs->sock >= 0);
    WRK_BgThread(&fs->tp, fs->thread_name, server_bgthread, fs);
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

    init_mimetypes();

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
    fs->ws_name     = malloc(strlen(vcl_name));

    AN(fs->thread_name);
    AN(fs->ws_name);

    sprintf(fs->thread_name, "fsthread-%s", vcl_name);
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
    free(fs->thread_name);
    free(fs->ws_name);
    free(fs->wrk->aws);
    FREE_OBJ(fs->wrk);
    FREE_OBJ(fs);
}
