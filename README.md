DoFP+ ABR Algorithm README
=============================================

Description
-----------

This is a fork of [LSQUIC](https://github.com/litespeedtech/lsquic) where we implemented DoFP+ ABR algorithm in QUIC
and HTTP/3.


Requirements
------------

To build LSQUIC, you need CMake, zlib, Gurobi and BoringSSL.  The example program
uses libevent to provide the event loop.

Building BoringSSL
------------------

BoringSSL is not packaged; you have to build it yourself.  The process is
straightforward.  You will need `go` installed.

1. Clone BoringSSL by issuing the following command:

```
git clone https://boringssl.googlesource.com/boringssl
cd boringssl
```

You may need to install pre-requisites like zlib and libevent.

2. Use specific BoringSSL version

```
git checkout a2278d4d2cabe73f6663e3299ea7808edfa306b9
```

3. Compile the library

```
cmake . &&  make
```

Remember where BoringSSL sources are:
```
BORINGSSL=$PWD
```

If you want to turn on optimizations, do

```
cmake -DCMAKE_BUILD_TYPE=Release . && make
```

If you want to build as a library, (necessary to build lsquic itself
as as shared library) do:

```
cmake -DBUILD_SHARED_LIBS=1 . && make
```

Building LSQUIC Library
-----------------------

LSQUIC's `http_client`, `http_server`, and the tests link BoringSSL
libraries statically.  Following previous section, you can build LSQUIC
as follows:

1. Get the source code

```
git clone https://github.com/cd-athena/dofp.git
cd lsquic
git checkout my_updates
git submodule init
git submodule update
```

2. Compile the library

Statically:


```
# $BORINGSSL is the top-level BoringSSL directory from the previous step
cmake -DBORINGSSL_DIR=$BORINGSSL .
make
```

As a dynamic library:

```
cmake -DLSQUIC_SHARED_LIB=1 -DBORINGSSL_DIR=$BORINGSSL .
make
```


3. Run experiments

```
./bin/http_client_dofp_python -H www.optimized-abr.com -s 192.168.153.139:123456 -p tos1_h264/107/segment_1.m4s -M GET -K -r 250 -w 1 -J 0
```

