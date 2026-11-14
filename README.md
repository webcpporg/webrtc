# webrtc

webrtc is a header-only library of WebRTC peer connections and data channels
for C++20, one of the libraries of [webcpp](https://github.com/webcpporg/webcpp).

Its types and operations carry the names and the shapes of the W3C's WebRTC
API, `RTCPeerConnection` and `RTCDataChannel`, written in Asio's model: every
operation takes a completion token and runs its handler on the executor its
object was made with, every event is a wait the program starts again, and
every failure is a value. It runs natively over libdatachannel, through its C
API, with exceptions on and off; the browser implementation, over the page's
own `RTCPeerConnection`, comes next. Media is out of scope.

<!-- include::example/webrtc_quick_start.cpp[tag=make,indent=0] -->
```cpp
asio::io_context io;
webrtc::runtime rt;  // libdatachannel's threads, made before any connection

// Host candidates on the loopback only, so that the example needs no network.
webrtc::configuration config;
config.bind_address = "127.0.0.1";

auto a = webrtc::peer_connection::make(rt, io.get_executor(), config).value();
auto b = webrtc::peer_connection::make(rt, io.get_executor(), config).value();
```

One header, `<webcpp/webrtc.hpp>`, includes the whole library, whose names
are in the namespace `webcpp::webrtc`. A b2 project uses it through
`/webcpp/webrtc//webrtc`, which natively brings the superproject's pinned
libdatachannel 0.24.6 and its OpenSSL, built from their sources, and needs
`threading=multi`.

## Building and testing

webrtc is developed inside the webcpp superproject, as a Boost library is
developed inside Boost:

    git clone --recursive https://github.com/webcpporg/webcpp
    cd webcpp
    b2 threading=multi libs/webrtc/test libs/webrtc/example

builds the tests and the examples natively, runs every example and compares
its output with the `.expected` file beside it; `exception-handling=off`
does the same without exceptions. `b2 threading=multi
libs/webrtc/bench//results` measures what reading a message costs.

## Documentation

webrtc's page, with its API reference, is published at
<https://webcpporg.github.io/libs/webrtc/>; `b2 threading=multi
libs/webrtc/doc` builds it into `doc/html/index.html`.

## License

Distributed under the [Boost Software License, Version 1.0](LICENSE_1_0.txt).
Natively webrtc uses libdatachannel unmodified, under the Mozilla Public
License 2.0, with its nested dependencies under their own licences.
