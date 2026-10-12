# CS 425 – P2: Reliable Data Transfer

- Name: Christopher Hastings
- Email: christopherhastings@u.boisestate.edu
- Class: CS425

A Go-Back-N file transfer over UDP, through a relay that drops, corrupts and
duplicates datagrams. One program, two modes:

```bash
make release
python3 scripts/cs425_relay.py --delay 50          # terminal 1
./build/release/myapp recv -s jdoe-1 127.0.0.1 out.bin                   # terminal 2
./build/release/myapp send -s jdoe-1 -w 16 -l 0.1 -c 0.05 127.0.0.1 in.bin  # terminal 3
cmp in.bin out.bin && echo identical
```

`scripts/cs425_relay.py` is an unmodified copy of the course relay, and
`scripts/measure.sh` reproduces the Results table below.

## Design

The protocol logic never touches a socket, a clock or a file. `src/lab.h` is
split into three layers, and each one only calls the layer beneath it.

### Layer 1 – Packets (`src/packet.c`)

Pure functions, bytes in and bytes or a struct out:

- `pkt_checksum` – the RFC 1071 Internet checksum, folding the carry after
  every word and padding an odd length with a zero byte. Run over a whole
  packet with its checksum filled in, it returns 0 exactly when the packet is
  undamaged, so verification and computation are the same function.
- `pkt_encode` – writes the 10-byte header field by field with `htonl`/`htons`
  (never a `memcpy` of a struct) and fills in the checksum.
- `pkt_decode` – validates, in this order: at least 10 bytes; the length field
  is at most 1024 and agrees with the size `recv` returned; type is DATA, ACK or
  FIN and reserved is 0; the checksum verifies. Only then is a payload byte
  copied. The I/O layer receives into a 1035-byte buffer, one byte more than
  the largest legal packet, so an oversized datagram can never pass as a full one.

### Layer 2 – Go-Back-N state machines (`src/gbn.c`)

A sender and a receiver, each a struct plus functions that feed it events. The
current time is always a parameter in milliseconds, and every event function
fills an `out` struct saying what to do next: which datagrams to send, what
payload to deliver, whether the FIN was just accepted, and when the timer is due.

- **Sender** – `base`, `next`, and a ring of `window` slots of 1034 bytes that
  holds the encoded copy of every packet from `base` to `next - 1` (packet `i`
  lives in slot `i % window`). There is one timer, stored as an absolute due
  time. `gbn_sender_start` fills the window; `gbn_sender_on_ack` slides `base`
  on any ACK with `base < seq <= next`, restarts or stops the timer and
  refills the window, and ignores everything else; `gbn_sender_on_timer`
  resends `base .. next - 1` once the due time has passed, or gives up on the
  10th timeout in a row with no progress. The FIN is just packet `ndata`, which
  may be sent only once `base == ndata`, and its ACK finishes the transfer.
  Stop-and-wait is the same code with a window of 1.
- **Receiver** – one number, `expected`, plus a single deadline that is the
  30-second idle give-up time until the FIN arrives and the end of the
  two-second linger afterwards. Every DATA or FIN is answered with
  `ACK expected`; only the one with `seq == expected` is delivered or accepted.

### Layer 3 – I/O (`src/io.c`, `src/cli.c`, `src/main.c`)

The only code that knows the outside world exists, and it stays thin:

- `io_open` resolves the relay with `getaddrinfo` (a name or an address,
  preferring IPv4 because the relay binds 127.0.0.1) and returns one UDP socket,
  `connect`ed to the relay, that is used for the whole run.
- `io_hello` sends the hello and waits one second for `OK` or `ERR <reason>`,
  five attempts in all.
- `io_run_sender` / `io_run_receiver` are the poll loops: wait on the socket with
  the time left on the state machine's timer as the poll timeout, decode
  whatever arrives with layer 1, hand it to layer 2, and carry out the `out`
  struct that comes back. Time comes from `clock_gettime(CLOCK_MONOTONIC)`.
  An ICMP "port unreachable" (`ECONNREFUSED`) is treated as a lost datagram,
  since that is all it means to the protocol.
- `cli_parse` reads the mode from `argv[1]` and hands `argv + 1` to `getopt`.
  `main` opens the file before registering, so a bad path fails fast with exit 1.

### Why the split

You cannot unit test against a relay that loses packets at random, but you can
unit test a state machine. Because layer 2 takes the time as an argument and
returns datagrams instead of sending them, the tests drive it with a fake clock
and an in-memory channel. A lost packet is a packet the test does not pass
along, and a timeout is a larger number for "now". The end-to-end tests run
both state machines against each other through a channel that copies the
relay's damage model (drop, else flip one random bit, else duplicate) with a
seeded xorshift generator, so a failing seed replays exactly. Twelve seeds at
20% loss, 20% corruption and 20% duplication in each direction check that every
byte delivered matches every byte sent. Minutes of simulated time run in
milliseconds, with no flaky timing.

Layer 3 is tested too, against a test that plays the relay on a real loopback
socket and queues its side of the conversation in advance: the hello's `OK`,
`ERR` and retries, a sender fed scripted ACKs, a receiver fed duplicates,
gaps, a corrupted packet and a repeated FIN, a relay that disappears, and
writes that fail on `/dev/full`. All four source files have 100% line coverage.
`main.c` is excluded by the Makefile. The only lines excluded by hand are the
failure branches of system and library calls.

## Results

A 1 MiB file of random bytes (1024 DATA packets plus the FIN, 1025 in all)
went through the course relay started with `--delay 50`, using the default
250 ms timeout. Each combination ran three times, timed with `time` on the
sender, and every copy matched the original under `cmp`. The runs were on
Ubuntu 24.04 under WSL2, with the relay, sender and receiver on one machine.
`scripts/measure.sh` reproduces the table.

| Window | Loss | Corrupt | Dup | Run 1 (s) | Run 2 (s) | Run 3 (s) | Mean time (s) | Throughput (KiB/s) |
|-------:|-----:|--------:|----:|----------:|----------:|----------:|--------------:|-------------------:|
| 1      | 0    | 0       | 0   | 102.98    | 103.44    | 102.47    | 102.96        | 9.9                |
| 16     | 0    | 0       | 0   | 6.74      | 6.73      | 6.73      | 6.73          | 152.1              |
| 1      | 0.05 | 0       | 0   | 131.77    | 132.58    | 129.99    | 131.44        | 7.8                |
| 16     | 0.05 | 0       | 0   | 21.23     | 20.76     | 24.79     | 22.26         | 46.0               |

The sender's own count of what it sent (mean of the three runs):

| Window | Loss | Datagrams sent | Resent | Timeouts |
|-------:|-----:|---------------:|-------:|---------:|
| 1      | 0    | 1025           | 0      | 0        |
| 16     | 0    | 1025           | 0      | 0        |
| 1      | 0.05 | 1140           | 115    | 115      |
| 16     | 0.05 | 1854           | 829    | 52       |

**Round trip time.** With a window of 1 the sender waits one full round trip
for each of its 1025 packets, so the RTT it saw was 102.96 s / 1025 =
**100.45 ms**. The relay accounts for 100 ms of that (50 ms each way). The other
~0.45 ms per round trip is processing that `--delay` does not include. Each
packet crosses the loopback stack four times (sender to relay, relay to
receiver, and back). The relay is an interpreted Python loop that wakes on
`select`, parses the datagram, rolls its random numbers and pushes onto a heap,
and its wake-ups to release delayed datagrams land a little after the due time.
Both of my processes also take a scheduler wake-up out of `poll` for every
packet. A smaller one-time cost, spread over the run, is process start-up,
reading the file, and the hello round trip, which the relay answers without delay.

**Window 16.** A window of 16 keeps 16 packets in flight per round trip, so the
ideal is 16 × 1 KiB / 100.45 ms ≈ 159 KiB/s. The measurement was 6.73 s and 152 KiB/s, a speedup of 102.96 / 6.73 =
**15.3×**: close to 16, but not quite. Two things keep it under 16:

- The FIN is effectively stop-and-wait. It may only go once every DATA packet
  is acknowledged, so the end of the transfer drains the pipe and pays one
  extra round trip that the window cannot overlap.
- Together that is 1024 / 16 = 64 round trips for the data plus one for the
  FIN: 65 × 100.45 ms ≈ 6.53 s, already 15.8× rather than 16×. That leaves
  about 0.2 s unexplained.
- The per-packet processing above is serialized. A burst of 16 datagrams queues
  behind itself in the relay's single-threaded loop instead of riding the
  100 ms delay in parallel. That accounts for the remaining ~0.2 s.

On a loopback link the bandwidth is effectively unlimited, so the window, not
the link, is the bottleneck. Throughput is window / RTT, which is why it scales
almost linearly.

**5% loss.** Loss costs window 1 an extra 28.5 s (+28%) but costs window 16 an
extra 15.5 s (+231%), and it cuts throughput from 152 to 46 KiB/s. The
difference is what a timeout costs each sender.

- **Window 1.** With 5% loss each way, about 1 − 0.95² ≈ 10% of round trips
  lose either the packet or its ACK, so each run took about 115 timeouts. Each
  one replaces a 100 ms round trip with a 250 ms wait followed by a new round
  trip. That is 115 × 0.25 s ≈ 28.8 s, which matches the measurement. The sender
  resends exactly one packet per timeout, and it was idle while waiting for a
  round trip anyway.
- **Window 16.** A lost DATA packet freezes `base`. The packets behind it
  arrive and are thrown away by the receiver, so their duplicate ACKs move
  nothing. The window fills, and the sender sits idle until the timer runs out.
  At 152 KiB/s, each 250 ms of idle time is about 40 packets of throughput lost.
  Then the timeout goes back N and resends everything from `base` to `next - 1`:
  829 resent packets over 52 timeouts, about 16 per timeout. That is the whole
  window, even though the receiver had already seen 15 of them. 45% of
  everything the window-16 sender transmitted was retransmission, against 10%
  for window 1. Those resends are themselves exposed to 5% loss, and the odds
  that at least one of 16 is dropped are 1 − 0.95¹⁶ ≈ 56%, so one loss often
  sets off another go-back.

Window 16 had fewer timeouts than window 1 (52 vs 115), because a lost ACK
costs it nothing: the next cumulative ACK covers it. But each timeout stalls a
full pipe and throws away a full window, which is the price Go-Back-N pays for
its trivial receiver. A selective-repeat receiver that buffered out-of-order
packets, or TCP's fast retransmit on duplicate ACKs, would recover most of that.

## Known Bugs or Issues

None known. Like any Go-Back-N sender, it can exit 2 after a transfer that
actually succeeded. If the receiver's ACK of the FIN is lost on every retry
during its two-second linger (eight retries at the default 250 ms timeout),
the receiver has the whole file and exits 0, but nobody is left to answer the
sender's FINs. The linger makes that unlikely; it cannot make it impossible.

## Experience

The trickiest part was deciding what layer 2 returns: the state machines cannot call `send`, `poll` or `clock_gettime`, so every event function must hand back everything the I/O layer needs – which datagrams to send, what payload to write, and when the timer is due.  Once that `out` struct was settled, the poll loop collapsed to a dozen lines and the tests could do anything the relay can.

The seeded in-memory channel paid off immediately – lost FINs, duplicated ACKs and corrupted length fields all showed up in `make check` long before the real relay was running, so the first transfer through `cs425_relay.py` came out byte-identical.
