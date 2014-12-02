vcl 4.0;

import static;

backend default { .host = "127.0.0.1"; .port = "8080";}

C{
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct ConfigBody {
  long len;
  char* raw;
};

typedef int (req_body_iter_f)(struct req *, void *priv, void *ptr, size_t);
extern int HTTP1_IterateReqBody(struct req *req, req_body_iter_f *func, void *priv);

int req_body_dump(struct req *req, void *priv, void *ptr, size_t len)
{
  if (len > 0) {
     struct ConfigBody *body = (struct ConfigBody*)priv;
     memcpy(body->raw+body->len,ptr,len);
     body->len += len;
  }
  return 0;
}

const struct gethdr_s hdr_content_length = {HDR_REQ,"\017Content-Length:"};

}C

sub vcl_init {
  static.print("VCL_INIT");
  new fs = static.file_system(default, "");
}

sub vcl_recv {
  C{printf("VCL_RECV %s %s\n", VRT_r_req_method(ctx), VRT_r_req_url(ctx));}C
  if (req.http.host == "static") {
    C{printf("..passing through to static backend\n");}C
  } else {
    return(synth(700));
  }
}

sub vcl_synth {
    set resp.status = 200;
    set resp.reason = "OK";
C{printf("VCL_SYNTH\n");}C
  if (req.method == "PUT" || req.method == "POST") {
    C{
      // Get the body of the PUT/POST.
      const char* content_len = VRT_GetHdr(ctx, &hdr_content_length);
      struct ConfigBody *body = malloc(sizeof(struct ConfigBody));
      long bodysize = atol(content_len);
      body->len = 0;
      body->raw = (char*)malloc(bodysize+1);
      HTTP1_IterateReqBody(ctx->req, req_body_dump, body);
      body->raw[bodysize] = 0;
      VRT_synth_page(ctx, body->raw, vrt_magic_string_end);
      free(body->raw);
      free(body);
    }C
} else {
    synthetic("ACK");
}
    return(deliver);
}

sub vcl_hit {
    static.print("VCL_HIT");
}

sub vcl_miss {
    static.print("VCL_MISS");
}

sub vcl_deliver {
    C{printf("VCL_DELIVER\n");}C
    set resp.http.hello = static.hello("World");
}
