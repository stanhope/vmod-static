vmod-static
===========

static file serving backend (Varnish V4.0.2 or greater only)

This vmod is based on work of Lasse (https://github.com/varnish/libvmod-example/) and Dridi (https://github.com/Dridi/libvmod-fsdirector).

It's a work in progress. The initial version works based on Dridi's work showing how to spinup a dynamic backend on the same host/process space as Varnish.
The end goal for V4.1 is a backend that doesn't spinup a background socket server yet delivers the same functionality and demonstrates how to properly
implement backend health probes, etc.

* No directory listing support. 
* No default file to server (e.g. index.html). 
* All requests must be explicit or a 404 will be returned.
* Lots of intentional debugging, run in the foreground to undestand how request processing occurs.

To test this simple vmod, edit the `default.vcl` and specify the root of the local file system for the backend to serve files from.

If the vcl file is not modified, the root will default to "/var/tmp" when "" is provided to the `static.file_system` object is created in `vcl_init` sub-routine.

Dependencies
------------

* Varnish 4.0.2 or greater

Ubuntu 14.04 Setup Steps
------------------------

It's my primary dev environment, so here's what I do when running, for example, on a new 14.04 EC2 AMI instance:

```
sudo su
apt-get update
apt-get install apt-transport-https git emacs
curl https://repo.varnish-cache.org/debian/GPG-key.txt | apt-key add -
echo "deb https://repo.varnish-cache.org/debian/ precise varnish-4.0" >> /etc/apt/sources.list.d/varnish-cache.list
apt-get update
apt-get install varnish
apt-get install libvarnishapi-dev
src/exit
```

At this point, varnish should be running on localhost:6081 and the non-existent backend port :8080. Testing the server should
result in the following 503 error:

```
curl -v localhost/
* Connected to localhost (127.0.0.1) port 80 (#0)
> GET / HTTP/1.1
> User-Agent: curl/7.35.0
> Host: localhost
> Accept: */*
> 
< HTTP/1.1 503 Backend fetch failed
< Date: Tue, 02 Dec 2014 05:12:09 GMT
* Server Varnish is not blacklisted
< Server: Varnish
< Content-Type: text/html; charset=utf-8
< Retry-After: 5
< X-Varnish: 32773
< Age: 0
< Via: 1.1 varnish-v4
< Content-Length: 282
< Connection: keep-alive
< 
```

> If a 503 response doesn't happen, it's because there is already a server that's bound port 8080. 
> Terminate this server. Or, modify the `default.vcl` and change the following config section:

```
backend default { .host = "127.0.0.1"; .port = "8080";}
```

> Change the .port to another one that is not in use.


Stop Varnish:

```
sudo service varnish stop
```


Building VMOD from Source
-------------------------

```
sudo apt-get install automake libtool pkg-config libpcre3-dev python-docutils ncurses-dev libedit-dev make
git clone https://github.com/stanhope/vmod-static.git
cd vmod-static
autogen.sh
configure
make
sudo make install
```


Restart Varnish
---------------

Since the sample VCL shows an inline C trick, you must enable inline C when launching Varnish:

Start a version running in the foreground:


```
sudo varnishd -F -a *:80 -p vcc_allow_inline_c=true -f default.vcl
child (25666) Started
Child (25666) said Child starts
Child (25666) said vmod_static: init_function 0x7fbf6b2bb710
Child (25666) said VCL_INIT
Child (25666) said vmod_static__init name=fs be=0x7fbf74c2b1f8 root=
Child (25666) said   defaulting to /var/tmp
Child (25666) said server_start
Child (25666) said ..fs->sock=12
```

Simple Testing
--------------

Create sample file to deliver via server:

```
curl -v localhost/
*   Trying 127.0.0.1...
* Connected to localhost (127.0.0.1) port 80 (#0)
> GET / HTTP/1.1
> User-Agent: curl/7.35.0
> Host: localhost
> Accept: */*
> 
< HTTP/1.1 200 OK
< Date: Tue, 02 Dec 2014 03:10:25 GMT
* Server Varnish is not blacklisted
< Server: Varnish
< X-Varnish: 4
< Content-Length: 3
< Connection: keep-alive
< Accept-Ranges: bytes
< 
* Connection #0 to host localhost left intact
ACK
```

The *ACK* is the hard-coded response from vcl_synth subroutine. This method will always be executed for *ANY* GET request sent to this server, *UNLESS* a 
host header is provided to cause the static file serving backend to be used instead. The VCL file defines to backends (default and static). 

To show that we can serve files, let's create a few samples to exercise the backend.

```
echo "test" > /var/tmp/test
```

```
curl -v -H"host:static" localhost/test
*   Trying 127.0.0.1...
* Connected to localhost (127.0.0.1) port 80 (#0)
> GET /test HTTP/1.1
> User-Agent: curl/7.35.0
> Accept: */*
> host:static
> 
< HTTP/1.1 200 OK
< Content-Type: text/plain; charset=us-ascii
< Date: Tue, 02 Dec 2014 03:14:02 GMT
< X-Varnish: 32770
< Age: 0
< Via: 1.1 varnish-v4
< hello: Hello, World
< Content-Length: 5
< Connection: keep-alive
< Accept-Ranges: bytes
< 
test
```

Repeat the request:

```
curl -v -H"host:static" localhost/test
* Hostname was NOT found in DNS cache
*   Trying 127.0.0.1...
* Connected to localhost (127.0.0.1) port 80 (#0)
> GET /test HTTP/1.1
> User-Agent: curl/7.35.0
> Accept: */*
> host:static
> 
< HTTP/1.1 200 OK
< Content-Type: text/plain; charset=us-ascii
< Date: Tue, 02 Dec 2014 03:14:02 GMT
< X-Varnish: 6 32771
< Age: 53
< Via: 1.1 varnish-v4
< hello: Hello, World
< Content-Length: 5
< Connection: keep-alive
< Accept-Ranges: bytes
< 
test
```

Note the above `Age: 53` response header. The second curl command was executed 53 seconds after the initial one that brought the file '/var/tmp/test' into cache. 

For the two tests, The foreground debugging of the varnish process should show the following:

```
Child (26219) said VCL_RECV GET /test
Child (26219) said ..passing through to static backend
Child (26219) said VCL_MISS
Child (26219) said   static.answer_appropriate
Child (26219) said   static.answer_file /var/tmp/test
Child (26219) said   static.send_response path=/var/tmp/test
Child (26219) said   static.prepare_answer status=200
Child (26219) said   static.add_content_type text/plain; charset=us-ascii
Child (26219) said VCL_DELIVER
Child (26219) said VCL_RECV GET /test
Child (26219) said ..passing through to static backend
Child (26219) said VCL_HIT
Child (26219) said VCL_DELIVER
```

> Note that for the second request we see that the file was delivered from cache (VCL_HIT) and therefore caused no backend debugging info to be displayed.
> The default TTL is 120 seconds. Every 120 seconds the object will be refreshed from the backend.

