# About

**p2p** is a minimal peer-to-peer mesh simulator running on localhost; a prototype, not a finished product. The single `main.cpp` source file is written in ISO C++14 and uses the header-only, _non-Boost_ version of the [ASIO framework](http://www.think-async.com).

This project uses UDP network communication: reading is asynchronous while writing is synchronous. The application-level protocol is strictly binary.

Text messages are logged to stdout/console. There is no user input to speak of, except for launching and quitting, everything else is automated with (asynchronous) timers.


# Logic

* the server, acting as a service, maintains the mesh directory (registry), containing _N_ peer entries. Upon any directory change (e.g. peer addition and/or removal), the server re-broadcasts its updated directory to all active mesh peers
* a new peer announces its birth to the server by sending its peer identifier: single-char name + network port. If that new name _and_ port are unique, the server will accept the new peer by adding it to its mesh. However, if that new peer name _or_ port is already used, that new peer will be rejected from the mesh and silently ignored (the rejected peer receives _no_ notification)
* once part of a mesh, a peer sends periodic keep-alive (heartbeat) messages to the server and receives any directory updates from the server. It may also receive _vanilla_ messages from other peers in the mesh
* a peer can be terminated by simply pressing CTRL-C to kill its process. There's no formal peer disconnection protocol as the absence of keeep-alive messages will be detected by the server
* the server periodically checks each peer's last heartbeat time stamp, removes any dead ones, and, upon change, updates its master peer directory, which it then re-broadcasts to the mesh's remaining (connected) peers
* each peer broadcasts periodic ASCII _vanilla_ messages to all known (hence alive) peers in the mesh. That vanilla message payload format is "<_source_peer_name_>\_broadcast\_<_counter_>" (the counter is incremented after every sent batch)
* a peer can only send vanilla messages to other known, live peers within its mesh (hence fulfilling the test assignment)
* apart from receiving heartbeats, the server is idle I/O-wise while no peers are added or removed


# Limitations, scalability and bottlenecks

* peer names _and_ ports must be unique across the mesh
  * port 3000 is reserved for the server
* the different timer values for heartbeat, reaping & vanilla messages are hardcoded
* error handling is rudimentary:
  * fatal-level: stdout message or assert()
  * application-level: stdout message or silent ignore
* for simplicity's sake, peer names can only be a single (printable) ASCII character, so the practical per mesh peer size limit must be around 80 peers
  * if names were lengthened, the limit would be the number of available host ports (somewhere under 65535 - 1024)
* if a peer's address were increased to a full `<ip4:port>`, the limit would be much higher (under 2^24)
* peer-to-peer message size is limited by the maximum IPv4 packet size: 65507 bytes, minus 2 bytes for the application-level protocol header
* from wikipedia: "UDP has no garantee of delivery, ordering, or duplicate protection"
* there's no other hardcoded application software limit

Other than above-mentioned limitations, the system itself is fairly scalable, although:
* large vanilla message payloads would benefit from a pre-allocated memory buffer pool
* network writes could be asynchronous but would need more complex source memory allocation
* hashmap access isn't a real (terminal) bottleneck as their bucket size could be fine-tuned.


# Portability

The code should be portable to any platform with an ISO-compliant C++14 compiler, e.g. Linux, Windows, OSX and iOS. The compiled binary is small (500 kB) and mostly made of logging messages so suitable for resource-limited platforms. Memory is allocated dynamically, but could be more frugal if needed.


# Attack surface and remedies

* server/peer impersonation
  * remedy via peer authentication
  * possibly with public key cryptography to encrypt/sign all mesh packets
* UDP packet spoofing
  * depends on user-facing ISP policy ("your mileage may vary")
* Denial Of Service
  * prevention should be upstream, at firewall-level
* vanilla message forgery and std::string stack overflows
  * remedy (hardening) should use `strnlen()` in addition to string zero-termination to double-validate an ASCII message length
* UTF-8 strings with multi-byte characters
  * remedy: proper str.size over str.length() use

In general, one should consider generic payload safety vs treating it as a string-only issue


# Build instructions

```
git clone https://github.com/kluete/p2plocal.git
cd p2plocal
git submodule update --init
./build.sh
```


# Invocation

The same executable is used to invoke server or peer.

## server
```
./p2p.bin
```

## peer
```
./p2p.bin <name> <port>
```

- `name` must currently be a single (printable) character
- `port` 3000 is reserved by the server


# Example

terminal 1:
```
./p2p.bin
```

terminal 2:
```
./p2p.bin A 3001
# note doesn't send vanilla messages since only single-peer mesh
```

terminal 3:
```
./p2p.bin B 3002
# note vanilla messages flow across mesh
```

terminal 4:
```
./p2p.bin C 3003
# note more vanilla messages flow across mesh

```

terminal 3:
```
# terminate peer B
CTRL-C
(...)
# note directory update
(...)
# relaunch peer B
./p2p.bin B 3002
# note directory update, resumption of cross-messaging
```


# Suggested enhancements

* multi-host protocol, i.e. replace `127.0.0.1:port` with `ip:port`
* longer peer name or replace it altogether with peer's `ip:port`
* templated function for binary stream de/serialization
* peer stdin user input for:
  * connect/disconnect
  * vanilla broadcast on/off
* more timeouts, f.ex. for rejected peer
* unit tests with timed disconnect/sequences
* confirm number of threads spawn, i.e. that ASIO callbacks really are "stackles coroutines"
* valgrind memory proofing
* static analysis, e.g. with Synopsys Covertity Scan
* protocol fuzzing (random data UDP messages)
* kcachegrind performance profiling (?)
* manual benchmarks upon mesh scaling


# Famous last words

Enjoy, hope it's useful but, still, "caveat emptor" (_so there_).


