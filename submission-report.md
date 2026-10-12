# Submission Report

- Submission generated at 10/12/2026 at 03:53:50

- Machine info: Linux runnervmmprz5 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

## Note to Students

Please read this report carefully before submission.
Ensure that all sections are complete and accurate.
Look for any errors in the build or test outputs.
If you find any issues, correct them before submitting.
Post any questions on the class discussion board for help.


---

## README

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

---


## Build Output

This section was generated by running `make all` in the project root directory.

```bash
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/io.c -o build/debug/io.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/packet.c -o build/debug/packet.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/cli.c -o build/debug/cli.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/gbn.c -o build/debug/gbn.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug/main.c.o
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address build/debug/io.c.o build/debug/packet.c.o build/debug/cli.c.o build/debug/gbn.c.o build/debug/main.c.o -o build/debug/myapp_d -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/io.c -o build/release/io.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/packet.c -o build/release/packet.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/cli.c -o build/release/cli.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/gbn.c -o build/release/gbn.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/main.c -o build/release/main.c.o
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion build/release/io.c.o build/release/packet.c.o build/release/cli.c.o build/release/gbn.c.o build/release/main.c.o -o build/release/myapp 
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/io.c -o build/tests/io.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/packet.c -o build/tests/packet.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/cli.c -o build/tests/cli.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/gbn.c -o build/tests/gbn.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/main.c -o build/tests/main.c.o
mkdir -p build/tests/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/lab-test.c -o build/tests/lab-test.c.o
mkdir -p build/tests/harness/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/harness/unity.c -o build/tests/harness/unity.c.o
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage build/tests/io.c.o build/tests/packet.c.o build/tests/cli.c.o build/tests/gbn.c.o build/tests/main.c.o build/tests/lab-test.c.o build/tests/harness/unity.c.o -o build/tests/myapp_t -fprofile-arcs -ftest-coverage
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/io.c -o build/debug-test/io.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/packet.c -o build/debug-test/packet.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/cli.c -o build/debug-test/cli.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/gbn.c -o build/debug-test/gbn.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug-test/main.c.o
mkdir -p build/debug-test/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/lab-test.c -o build/debug-test/lab-test.c.o
mkdir -p build/debug-test/harness/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/harness/unity.c -o build/debug-test/harness/unity.c.o
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address build/debug-test/io.c.o build/debug-test/packet.c.o build/debug-test/cli.c.o build/debug-test/gbn.c.o build/debug-test/main.c.o build/debug-test/lab-test.c.o build/debug-test/harness/unity.c.o -o build/debug-test/myapp_td -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
Builds completed. You can run the application with: ./build/release/myapp
You can run the debug build with: ./build/debug/myapp_d
You can run the test build with: ./build/tests/myapp_t
You can run the debug-test build with: ./build/debug-test/myapp_td
```

---

## Coverage Report

This section was generated by running `make report` in the project root directory.

```bash
error: cannot resolve 127.0.0.1 port not-a-port: Servname not supported for ai_socktype
error: /tmp/cs425-p2-test-GpledR is larger than 16 MiB
error: cannot open /tmp/cs425-p2-test-GpledR: No such file or directory
sent 1500 bytes in 3 packets: 3 datagrams, 0 resent, 0 timeouts
giving up: 10 timeouts in a row with no progress
error: invalid sender arguments
giving up: 10 timeouts in a row with no progress
received 12 bytes
giving up: nothing valid arrived for 30 ms
giving up: nothing valid arrived for 50 ms
write: No space left on device
close: No space left on device
error: expected send or recv
error: mode must be send or recv: 'bogus'
recv: invalid option -- 'w'
error: -s <session> is required
error: session must be 1 to 32 of a-z, 0-9 and -: 'Bad_Name'
error: window must be 1 to 64: '0'
error: window must be 1 to 64: '65'
error: window must be 1 to 64: '4x'
error: window must be 1 to 64: ''
error: window must be 1 to 64: '99999999999999999999'
error: timeout must be 1 to 600000 ms: '0'
error: loss must be 0 to 0.5: '0.6'
error: corrupt must be 0 to 0.5: 'x'
error: dup must be 0 to 0.5: '-1'
error: port must be 1 to 65535: '0'
error: port must be 1 to 65535: '70000'
error: expected <relay> and <file>
error: expected <relay> and <file>
send: option requires an argument -- 's'
tests/lab-test.c:1299:test_checksum_rfc1071_example:PASS
tests/lab-test.c:1300:test_checksum_odd_length:PASS
tests/lab-test.c:1301:test_checksum_folds_carry:PASS
tests/lab-test.c:1302:test_checksum_of_good_packet_is_zero:PASS
tests/lab-test.c:1303:test_encode_hi_example:PASS
tests/lab-test.c:1304:test_encode_ack_example:PASS
tests/lab-test.c:1305:test_encode_network_byte_order:PASS
tests/lab-test.c:1306:test_encode_rejects_bad_arguments:PASS
tests/lab-test.c:1307:test_decode_round_trip:PASS
tests/lab-test.c:1308:test_decode_rejects_every_single_bit_flip:PASS
tests/lab-test.c:1309:test_decode_rejects_too_short:PASS
tests/lab-test.c:1310:test_decode_rejects_length_mismatch:PASS
tests/lab-test.c:1311:test_decode_rejects_unknown_type_and_reserved:PASS
tests/lab-test.c:1313:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:1314:test_sender_empty_file_is_a_single_fin:PASS
tests/lab-test.c:1315:test_sender_cuts_2500_bytes_like_example_1:PASS
tests/lab-test.c:1316:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:1317:test_sender_stops_when_window_is_full:PASS
tests/lab-test.c:1318:test_sender_cumulative_ack_slides_several:PASS
tests/lab-test.c:1319:test_sender_lost_acks_cost_nothing_example_3:PASS
tests/lab-test.c:1320:test_sender_ignores_duplicate_and_bogus_acks:PASS
tests/lab-test.c:1321:test_sender_timeout_resends_whole_window:PASS
tests/lab-test.c:1322:test_sender_gives_up_after_10_timeouts:PASS
tests/lab-test.c:1323:test_sender_progress_resets_timeout_count:PASS
tests/lab-test.c:1324:test_sender_retransmits_fin_until_acked:PASS
tests/lab-test.c:1325:test_sender_window_1_is_stop_and_wait:PASS
tests/lab-test.c:1327:test_receiver_delivers_in_order:PASS
tests/lab-test.c:1328:test_receiver_reacks_duplicate:PASS
tests/lab-test.c:1329:test_receiver_discards_packet_beyond_gap:PASS
tests/lab-test.c:1330:test_receiver_fin_linger_and_repeated_fin:PASS
tests/lab-test.c:1331:test_receiver_empty_file:PASS
tests/lab-test.c:1332:test_receiver_fin_out_of_order_is_reacked:PASS
tests/lab-test.c:1333:test_receiver_ignores_acks:PASS
tests/lab-test.c:1334:test_receiver_gives_up_when_idle:PASS
tests/lab-test.c:1336:test_transfer_clean_channel:PASS
tests/lab-test.c:1337:test_transfer_lossy_20_percent_seeded:PASS
tests/lab-test.c:1338:test_transfer_lossy_stop_and_wait_and_big_window:PASS
tests/lab-test.c:1339:test_transfer_lossy_empty_and_exact_multiple:PASS
tests/lab-test.c:1341:test_io_now_ms_is_monotonic:PASS
tests/lab-test.c:1342:test_io_open_rejects_bad_port:PASS
tests/lab-test.c:1343:test_io_hello_ok_by_name:PASS
tests/lab-test.c:1344:test_io_hello_err_reports_reason:PASS
tests/lab-test.c:1345:test_io_hello_ignores_noise:PASS
tests/lab-test.c:1346:test_io_hello_retries_on_silence:PASS
tests/lab-test.c:1347:test_io_hello_with_no_relay_running:PASS
tests/lab-test.c:1348:test_io_read_file:PASS
tests/lab-test.c:1349:test_io_run_sender_clean:PASS
tests/lab-test.c:1350:test_io_run_sender_gives_up:PASS
tests/lab-test.c:1351:test_io_run_receiver_writes_in_order:PASS
tests/lab-test.c:1352:test_io_run_receiver_gives_up_when_idle:PASS
tests/lab-test.c:1353:test_io_run_receiver_write_failures:PASS
tests/lab-test.c:1354:test_cli_usage:PASS
tests/lab-test.c:1355:test_cli_valid_session:PASS
tests/lab-test.c:1356:test_cli_parse_prob:PASS
tests/lab-test.c:1357:test_cli_parse_send_defaults:PASS
tests/lab-test.c:1358:test_cli_parse_send_all_options:PASS
tests/lab-test.c:1359:test_cli_parse_recv:PASS
tests/lab-test.c:1360:test_cli_parse_errors:PASS
tests/lab-test.c:1361:test_cli_hello:PASS

-----------------------
59 Tests 0 Failures 0 Ignored 
OK
./build/tests/myapp_t
error: cannot resolve 127.0.0.1 port not-a-port: Servname not supported for ai_socktype
error: /tmp/cs425-p2-test-OUgO5d is larger than 16 MiB
error: cannot open /tmp/cs425-p2-test-OUgO5d: No such file or directory
sent 1500 bytes in 3 packets: 3 datagrams, 0 resent, 0 timeouts
giving up: 10 timeouts in a row with no progress
error: invalid sender arguments
giving up: 10 timeouts in a row with no progress
received 12 bytes
giving up: nothing valid arrived for 30 ms
giving up: nothing valid arrived for 50 ms
write: No space left on device
close: No space left on device
error: expected send or recv
error: mode must be send or recv: 'bogus'
recv: invalid option -- 'w'
error: -s <session> is required
error: session must be 1 to 32 of a-z, 0-9 and -: 'Bad_Name'
error: window must be 1 to 64: '0'
error: window must be 1 to 64: '65'
error: window must be 1 to 64: '4x'
error: window must be 1 to 64: ''
error: window must be 1 to 64: '99999999999999999999'
error: timeout must be 1 to 600000 ms: '0'
error: loss must be 0 to 0.5: '0.6'
error: corrupt must be 0 to 0.5: 'x'
error: dup must be 0 to 0.5: '-1'
error: port must be 1 to 65535: '0'
error: port must be 1 to 65535: '70000'
error: expected <relay> and <file>
error: expected <relay> and <file>
send: option requires an argument -- 's'
tests/lab-test.c:1299:test_checksum_rfc1071_example:PASS
tests/lab-test.c:1300:test_checksum_odd_length:PASS
tests/lab-test.c:1301:test_checksum_folds_carry:PASS
tests/lab-test.c:1302:test_checksum_of_good_packet_is_zero:PASS
tests/lab-test.c:1303:test_encode_hi_example:PASS
tests/lab-test.c:1304:test_encode_ack_example:PASS
tests/lab-test.c:1305:test_encode_network_byte_order:PASS
tests/lab-test.c:1306:test_encode_rejects_bad_arguments:PASS
tests/lab-test.c:1307:test_decode_round_trip:PASS
tests/lab-test.c:1308:test_decode_rejects_every_single_bit_flip:PASS
tests/lab-test.c:1309:test_decode_rejects_too_short:PASS
tests/lab-test.c:1310:test_decode_rejects_length_mismatch:PASS
tests/lab-test.c:1311:test_decode_rejects_unknown_type_and_reserved:PASS
tests/lab-test.c:1313:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:1314:test_sender_empty_file_is_a_single_fin:PASS
tests/lab-test.c:1315:test_sender_cuts_2500_bytes_like_example_1:PASS
tests/lab-test.c:1316:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:1317:test_sender_stops_when_window_is_full:PASS
tests/lab-test.c:1318:test_sender_cumulative_ack_slides_several:PASS
tests/lab-test.c:1319:test_sender_lost_acks_cost_nothing_example_3:PASS
tests/lab-test.c:1320:test_sender_ignores_duplicate_and_bogus_acks:PASS
tests/lab-test.c:1321:test_sender_timeout_resends_whole_window:PASS
tests/lab-test.c:1322:test_sender_gives_up_after_10_timeouts:PASS
tests/lab-test.c:1323:test_sender_progress_resets_timeout_count:PASS
tests/lab-test.c:1324:test_sender_retransmits_fin_until_acked:PASS
tests/lab-test.c:1325:test_sender_window_1_is_stop_and_wait:PASS
tests/lab-test.c:1327:test_receiver_delivers_in_order:PASS
tests/lab-test.c:1328:test_receiver_reacks_duplicate:PASS
tests/lab-test.c:1329:test_receiver_discards_packet_beyond_gap:PASS
tests/lab-test.c:1330:test_receiver_fin_linger_and_repeated_fin:PASS
tests/lab-test.c:1331:test_receiver_empty_file:PASS
tests/lab-test.c:1332:test_receiver_fin_out_of_order_is_reacked:PASS
tests/lab-test.c:1333:test_receiver_ignores_acks:PASS
tests/lab-test.c:1334:test_receiver_gives_up_when_idle:PASS
tests/lab-test.c:1336:test_transfer_clean_channel:PASS
tests/lab-test.c:1337:test_transfer_lossy_20_percent_seeded:PASS
tests/lab-test.c:1338:test_transfer_lossy_stop_and_wait_and_big_window:PASS
tests/lab-test.c:1339:test_transfer_lossy_empty_and_exact_multiple:PASS
tests/lab-test.c:1341:test_io_now_ms_is_monotonic:PASS
tests/lab-test.c:1342:test_io_open_rejects_bad_port:PASS
tests/lab-test.c:1343:test_io_hello_ok_by_name:PASS
tests/lab-test.c:1344:test_io_hello_err_reports_reason:PASS
tests/lab-test.c:1345:test_io_hello_ignores_noise:PASS
tests/lab-test.c:1346:test_io_hello_retries_on_silence:PASS
tests/lab-test.c:1347:test_io_hello_with_no_relay_running:PASS
tests/lab-test.c:1348:test_io_read_file:PASS
tests/lab-test.c:1349:test_io_run_sender_clean:PASS
tests/lab-test.c:1350:test_io_run_sender_gives_up:PASS
tests/lab-test.c:1351:test_io_run_receiver_writes_in_order:PASS
tests/lab-test.c:1352:test_io_run_receiver_gives_up_when_idle:PASS
tests/lab-test.c:1353:test_io_run_receiver_write_failures:PASS
tests/lab-test.c:1354:test_cli_usage:PASS
tests/lab-test.c:1355:test_cli_valid_session:PASS
tests/lab-test.c:1356:test_cli_parse_prob:PASS
tests/lab-test.c:1357:test_cli_parse_send_defaults:PASS
tests/lab-test.c:1358:test_cli_parse_send_all_options:PASS
tests/lab-test.c:1359:test_cli_parse_recv:PASS
tests/lab-test.c:1360:test_cli_parse_errors:PASS
tests/lab-test.c:1361:test_cli_hello:PASS

-----------------------
59 Tests 0 Failures 0 Ignored 
OK
mkdir -p ./build/report/html
mkdir -p ./build/report/txt
gcovr -r . --html --html-details --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$' -o ./build/report/html/coverage_report.html
(INFO) Reading coverage data...

(INFO) Writing coverage report...

gcovr -r . --txt                 --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$'
(INFO) Reading coverage data...

(INFO) Writing coverage report...

------------------------------------------------------------------------------
                           GCC Code Coverage Report
Directory: .
------------------------------------------------------------------------------
File                                       Lines     Exec  Cover   Missing
------------------------------------------------------------------------------
src/cli.c                                     98       98   100%
src/gbn.c                                    115      115   100%
src/io.c                                     123      123   100%
src/packet.c                                  38       38   100%
------------------------------------------------------------------------------
TOTAL                                        374      374   100%
------------------------------------------------------------------------------
```

---

## Address Sanitizer Report

This section was generated by running `make leak-test` in the project root directory.

```bash
error: cannot resolve 127.0.0.1 port not-a-port: Servname not supported for ai_socktype
error: /tmp/cs425-p2-test-Wprqlx is larger than 16 MiB
error: cannot open /tmp/cs425-p2-test-Wprqlx: No such file or directory
sent 1500 bytes in 3 packets: 3 datagrams, 0 resent, 0 timeouts
giving up: 10 timeouts in a row with no progress
error: invalid sender arguments
giving up: 10 timeouts in a row with no progress
received 12 bytes
giving up: nothing valid arrived for 30 ms
giving up: nothing valid arrived for 50 ms
write: No space left on device
close: No space left on device
error: expected send or recv
error: mode must be send or recv: 'bogus'
recv: invalid option -- 'w'
error: -s <session> is required
error: session must be 1 to 32 of a-z, 0-9 and -: 'Bad_Name'
error: window must be 1 to 64: '0'
error: window must be 1 to 64: '65'
error: window must be 1 to 64: '4x'
error: window must be 1 to 64: ''
error: window must be 1 to 64: '99999999999999999999'
error: timeout must be 1 to 600000 ms: '0'
error: loss must be 0 to 0.5: '0.6'
error: corrupt must be 0 to 0.5: 'x'
error: dup must be 0 to 0.5: '-1'
error: port must be 1 to 65535: '0'
error: port must be 1 to 65535: '70000'
error: expected <relay> and <file>
error: expected <relay> and <file>
send: option requires an argument -- 's'
tests/lab-test.c:1299:test_checksum_rfc1071_example:PASS
tests/lab-test.c:1300:test_checksum_odd_length:PASS
tests/lab-test.c:1301:test_checksum_folds_carry:PASS
tests/lab-test.c:1302:test_checksum_of_good_packet_is_zero:PASS
tests/lab-test.c:1303:test_encode_hi_example:PASS
tests/lab-test.c:1304:test_encode_ack_example:PASS
tests/lab-test.c:1305:test_encode_network_byte_order:PASS
tests/lab-test.c:1306:test_encode_rejects_bad_arguments:PASS
tests/lab-test.c:1307:test_decode_round_trip:PASS
tests/lab-test.c:1308:test_decode_rejects_every_single_bit_flip:PASS
tests/lab-test.c:1309:test_decode_rejects_too_short:PASS
tests/lab-test.c:1310:test_decode_rejects_length_mismatch:PASS
tests/lab-test.c:1311:test_decode_rejects_unknown_type_and_reserved:PASS
tests/lab-test.c:1313:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:1314:test_sender_empty_file_is_a_single_fin:PASS
tests/lab-test.c:1315:test_sender_cuts_2500_bytes_like_example_1:PASS
tests/lab-test.c:1316:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:1317:test_sender_stops_when_window_is_full:PASS
tests/lab-test.c:1318:test_sender_cumulative_ack_slides_several:PASS
tests/lab-test.c:1319:test_sender_lost_acks_cost_nothing_example_3:PASS
tests/lab-test.c:1320:test_sender_ignores_duplicate_and_bogus_acks:PASS
tests/lab-test.c:1321:test_sender_timeout_resends_whole_window:PASS
tests/lab-test.c:1322:test_sender_gives_up_after_10_timeouts:PASS
tests/lab-test.c:1323:test_sender_progress_resets_timeout_count:PASS
tests/lab-test.c:1324:test_sender_retransmits_fin_until_acked:PASS
tests/lab-test.c:1325:test_sender_window_1_is_stop_and_wait:PASS
tests/lab-test.c:1327:test_receiver_delivers_in_order:PASS
tests/lab-test.c:1328:test_receiver_reacks_duplicate:PASS
tests/lab-test.c:1329:test_receiver_discards_packet_beyond_gap:PASS
tests/lab-test.c:1330:test_receiver_fin_linger_and_repeated_fin:PASS
tests/lab-test.c:1331:test_receiver_empty_file:PASS
tests/lab-test.c:1332:test_receiver_fin_out_of_order_is_reacked:PASS
tests/lab-test.c:1333:test_receiver_ignores_acks:PASS
tests/lab-test.c:1334:test_receiver_gives_up_when_idle:PASS
tests/lab-test.c:1336:test_transfer_clean_channel:PASS
tests/lab-test.c:1337:test_transfer_lossy_20_percent_seeded:PASS
tests/lab-test.c:1338:test_transfer_lossy_stop_and_wait_and_big_window:PASS
tests/lab-test.c:1339:test_transfer_lossy_empty_and_exact_multiple:PASS
tests/lab-test.c:1341:test_io_now_ms_is_monotonic:PASS
tests/lab-test.c:1342:test_io_open_rejects_bad_port:PASS
tests/lab-test.c:1343:test_io_hello_ok_by_name:PASS
tests/lab-test.c:1344:test_io_hello_err_reports_reason:PASS
tests/lab-test.c:1345:test_io_hello_ignores_noise:PASS
tests/lab-test.c:1346:test_io_hello_retries_on_silence:PASS
tests/lab-test.c:1347:test_io_hello_with_no_relay_running:PASS
tests/lab-test.c:1348:test_io_read_file:PASS
tests/lab-test.c:1349:test_io_run_sender_clean:PASS
tests/lab-test.c:1350:test_io_run_sender_gives_up:PASS
tests/lab-test.c:1351:test_io_run_receiver_writes_in_order:PASS
tests/lab-test.c:1352:test_io_run_receiver_gives_up_when_idle:PASS
tests/lab-test.c:1353:test_io_run_receiver_write_failures:PASS
tests/lab-test.c:1354:test_cli_usage:PASS
tests/lab-test.c:1355:test_cli_valid_session:PASS
tests/lab-test.c:1356:test_cli_parse_prob:PASS
tests/lab-test.c:1357:test_cli_parse_send_defaults:PASS
tests/lab-test.c:1358:test_cli_parse_send_all_options:PASS
tests/lab-test.c:1359:test_cli_parse_recv:PASS
tests/lab-test.c:1360:test_cli_parse_errors:PASS
tests/lab-test.c:1361:test_cli_hello:PASS

-----------------------
59 Tests 0 Failures 0 Ignored 
OK
```

---

## Src Files
### cli.c

```c

#include "lab.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TIMEOUT_MS 600000L

void cli_usage(FILE *f)
{
    fputs("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
          "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
          "       myapp recv -s <session> [-p port] <relay> <file>\n"
          "\n"
          "  -s <session>     session name shared by the sender and the receiver\n"
          "  -w <window>      Go-Back-N window size in packets, 1 to 64 (default: 8)\n"
          "  -T <timeout-ms>  retransmission timeout in milliseconds (default: 250)\n"
          "  -l <loss>        probability the relay drops a packet (default: 0)\n"
          "  -c <corrupt>     probability the relay flips a bit (default: 0)\n"
          "  -d <dup>         probability the relay duplicates a packet (default: 0)\n"
          "  -p <port>        relay port (default: 4250)\n"
          "  <relay>          host name or address of the relay\n"
          "  <file>           file to send, or file to write what is received\n",
          f);
}

int cli_valid_session(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

int cli_parse_prob(const char *s, double *out)
{
    if (!s || !*s) return -1;
    char *end;
    errno    = 0;
    double v = strtod(s, &end);
    /* The negated test also rejects NaN. */
    if (errno != 0 || *end != '\0' || !(v >= 0.0 && v <= 0.5)) return -1;
    *out = v;
    return 0;
}

static int parse_range(const char *s, long lo, long hi, long *out)
{
    char *end;
    errno  = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static int bad(const char *what, const char *arg)
{
    fprintf(stderr, "error: %s: '%s'\n", what, arg);
    return -1;
}

int cli_parse(int argc, char *argv[], options_t *o)
{
    memset(o, 0, sizeof *o);
    o->port       = "4250";
    o->window     = 8;
    o->timeout_ms = 250;

    if (argc < 2) {
        fprintf(stderr, "error: expected send or recv\n");
        return -1;
    }
    const char *opts;
    if (strcmp(argv[1], "send") == 0) {
        o->mode = MODE_SEND;
        opts    = "s:w:T:l:c:d:p:";
    } else if (strcmp(argv[1], "recv") == 0) {
        o->mode = MODE_RECV;
        opts    = "s:p:";
    } else {
        return bad("mode must be send or recv", argv[1]);
    }

    /* getopt sees the mode word as its program name. */
    int    ac = argc - 1;
    char **av = argv + 1;
    long   v;
    int    c;
#ifdef __GLIBC__
    optind = 0; /* full reset, so this can be called more than once */
#else
    optind = 1;
#endif
    while ((c = getopt(ac, av, opts)) != -1) {
        switch (c) {
        case 's':
            o->session = optarg;
            break;
        case 'w':
            if (parse_range(optarg, 1, GBN_MAX_WINDOW, &v)) return bad("window must be 1 to 64", optarg);
            o->window = (uint32_t)v;
            break;
        case 'T':
            if (parse_range(optarg, 1, MAX_TIMEOUT_MS, &v)) return bad("timeout must be 1 to 600000 ms", optarg);
            o->timeout_ms = v;
            break;
        case 'l':
            if (cli_parse_prob(optarg, &o->loss)) return bad("loss must be 0 to 0.5", optarg);
            break;
        case 'c':
            if (cli_parse_prob(optarg, &o->corrupt)) return bad("corrupt must be 0 to 0.5", optarg);
            break;
        case 'd':
            if (cli_parse_prob(optarg, &o->dup)) return bad("dup must be 0 to 0.5", optarg);
            break;
        case 'p':
            if (parse_range(optarg, 1, 65535, &v)) return bad("port must be 1 to 65535", optarg);
            o->port = optarg;
            break;
        default:
            return -1; /* getopt has already said what was wrong */
        }
    }

    if (!o->session) {
        fprintf(stderr, "error: -s <session> is required\n");
        return -1;
    }
    if (!cli_valid_session(o->session))
        return bad("session must be 1 to 32 of a-z, 0-9 and -", o->session);
    if (ac - optind != 2) {
        fprintf(stderr, "error: expected <relay> and <file>\n");
        return -1;
    }
    o->relay = av[optind];
    o->file  = av[optind + 1];
    return 0;
}

/* A rate as a plain decimal with no exponent and no trailing zeros: 0.1, 0.05, 0. */
static void fmt_rate(double v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%.6f", v);
    char *p = buf + strlen(buf) - 1;
    while (*p == '0') *p-- = '\0';
    if (*p == '.') *p = '\0';
}

int cli_hello(const options_t *o, char *buf, size_t cap)
{
    int n;
    if (o->mode == MODE_RECV) {
        n = snprintf(buf, cap, "HELLO %s recv", o->session);
    } else {
        char l[32], c[32], d[32];
        fmt_rate(o->loss, l, sizeof l);
        fmt_rate(o->corrupt, c, sizeof c);
        fmt_rate(o->dup, d, sizeof d);
        n = snprintf(buf, cap, "HELLO %s send %s %s %s", o->session, l, c, d);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

```

### gbn.c

```c

#include "lab.h"

#include <stdlib.h>
#include <string.h>

/* ── Sender ──────────────────────────────────────────────────────────── */

static uint8_t *slot_of(gbn_sender_t *s, uint32_t seq)
{
    return s->slots + (size_t)(seq % s->window) * PKT_MAX_LEN;
}

/* Put the stored copy of packet seq on the send list. */
static void queue_slot(gbn_sender_t *s, uint32_t seq, gbn_sender_out_t *out)
{
    out->send[out->nsend].buf = slot_of(s, seq);
    out->send[out->nsend].len = s->slot_len[seq % s->window];
    out->nsend++;
    s->sent++;
}

static void out_begin(gbn_sender_out_t *out)
{
    out->nsend = 0;
}

static void out_end(const gbn_sender_t *s, gbn_sender_out_t *out)
{
    out->timer_due = s->timer_due;
    out->status    = s->status;
}

/*
 * Send while next < base + window and there is something left to send.  The
 * FIN is packet ndata, and it goes only once every DATA packet is ACKed.
 */
static void fill_window(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out)
{
    while (s->next < s->base + s->window) {
        uint8_t *slot = slot_of(s, s->next);
        size_t   n;

        if (s->next < s->ndata) {
            size_t off   = (size_t)s->next * PKT_MAX_PAYLOAD;
            size_t chunk = s->len - off;
            if (chunk > PKT_MAX_PAYLOAD) chunk = PKT_MAX_PAYLOAD;
            n = pkt_encode(slot, PKT_DATA, s->next, s->data + off, chunk);
        } else if (s->next == s->ndata && s->base == s->ndata) {
            n = pkt_encode(slot, PKT_FIN, s->next, NULL, 0);
        } else {
            break;
        }
        s->slot_len[s->next % s->window] = n;
        queue_slot(s, s->next, out);
        s->next++;
    }
    if (s->timer_due < 0 && s->base < s->next) s->timer_due = now + s->timeout_ms;
}

int gbn_sender_init(gbn_sender_t *s, const uint8_t *data, size_t len,
                    uint32_t window, int64_t timeout_ms)
{
    if (!s) return -1;
    memset(s, 0, sizeof *s);
    if (window < 1 || window > GBN_MAX_WINDOW || timeout_ms <= 0) return -1;
    if (len > GBN_MAX_FILE || (!data && len > 0)) return -1;

    s->slots = malloc((size_t)window * PKT_MAX_LEN);
    if (!s->slots) return -1; /* GCOVR_EXCL_LINE */

    s->data       = data;
    s->len        = len;
    s->ndata      = (uint32_t)((len + PKT_MAX_PAYLOAD - 1u) / PKT_MAX_PAYLOAD);
    s->window     = window;
    s->timeout_ms = timeout_ms;
    s->timer_due  = -1;
    s->status     = GBN_RUNNING;
    return 0;
}

void gbn_sender_destroy(gbn_sender_t *s)
{
    if (!s) return;
    free(s->slots);
    s->slots = NULL;
}

void gbn_sender_start(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out)
{
    out_begin(out);
    fill_window(s, now, out);
    out_end(s, out);
}

void gbn_sender_on_ack(gbn_sender_t *s, const pkt_t *ack, int64_t now,
                       gbn_sender_out_t *out)
{
    out_begin(out);
    /* seq <= base is a duplicate; seq > next acknowledges something never sent. */
    if (s->status == GBN_RUNNING && ack && ack->type == PKT_ACK &&
        ack->seq > s->base && ack->seq <= s->next) {
        s->base     = ack->seq; /* cumulative: may slide several packets at once */
        s->timeouts = 0;
        if (s->base == s->ndata + 1u) {
            s->status    = GBN_DONE; /* that was the ACK of the FIN */
            s->timer_due = -1;
        } else {
            s->timer_due = s->base < s->next ? now + s->timeout_ms : -1;
            fill_window(s, now, out);
        }
    }
    out_end(s, out);
}

void gbn_sender_on_timer(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out)
{
    out_begin(out);
    if (s->status == GBN_RUNNING && s->timer_due >= 0 && now >= s->timer_due) {
        s->timeouts++;
        s->total_timeouts++;
        if (s->timeouts >= GBN_MAX_TIMEOUTS) {
            s->status    = GBN_FAILED;
            s->timer_due = -1;
        } else {
            /* Go back N: everything still in flight goes again. */
            for (uint32_t seq = s->base; seq < s->next; seq++) {
                queue_slot(s, seq, out);
                s->resent++;
            }
            s->timer_due = now + s->timeout_ms;
        }
    }
    out_end(s, out);
}

/* ── Receiver ────────────────────────────────────────────────────────── */

static void recv_out_end(const gbn_receiver_t *r, gbn_receiver_out_t *out)
{
    out->timer_due = r->status == GBN_RUNNING ? r->deadline : -1;
    out->status    = r->status;
}

void gbn_receiver_init(gbn_receiver_t *r, int64_t now, int64_t linger_ms,
                       int64_t idle_ms)
{
    memset(r, 0, sizeof *r);
    r->linger_ms = linger_ms;
    r->idle_ms   = idle_ms;
    r->deadline  = now + idle_ms;
    r->status    = GBN_RUNNING;
}

void gbn_receiver_on_packet(gbn_receiver_t *r, const pkt_t *p, int64_t now,
                            gbn_receiver_out_t *out)
{
    memset(out, 0, sizeof *out);
    if (r->status != GBN_RUNNING || !p || p->type == PKT_ACK) {
        recv_out_end(r, out);
        return;
    }

    if (!r->finished) {
        r->deadline = now + r->idle_ms;
        if (p->seq == r->expected) {
            if (p->type == PKT_DATA) {
                out->deliver     = p->payload;
                out->deliver_len = p->len;
            } else {
                r->finished = 1;
                r->deadline = now + r->linger_ms;
                out->fin    = 1;
            }
            r->expected++;
        }
    }

    /* In order or not, the answer is always "next I want is expected". */
    out->ack.len = pkt_encode(r->ackbuf, PKT_ACK, r->expected, NULL, 0);
    out->ack.buf = r->ackbuf;
    recv_out_end(r, out);
}

void gbn_receiver_on_timer(gbn_receiver_t *r, int64_t now,
                           gbn_receiver_out_t *out)
{
    memset(out, 0, sizeof *out);
    if (r->status == GBN_RUNNING && now >= r->deadline)
        r->status = r->finished ? GBN_DONE : GBN_FAILED;
    recv_out_end(r, out);
}

```

### io.c

```c

#include "lab.h"

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t io_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int io_open(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "error: cannot resolve %s port %s: %s\n", host, port,
                gai_strerror(rc));
        return -1;
    }

    /* The relay listens on IPv4 only, so prefer an IPv4 address if there is one. */
    struct addrinfo *ai = res;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            ai = p;
            break;
        }
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) { /* GCOVR_EXCL_START */
        perror("socket");
        if (fd >= 0) close(fd);
        fd = -1;
    } /* GCOVR_EXCL_STOP */
    freeaddrinfo(res);
    return fd;
}

/*
 * Errors that mean "this datagram is gone" rather than "the socket is broken".
 * ECONNREFUSED is an ICMP port unreachable about an earlier datagram: nobody
 * is listening at the relay's port right now.  UDP promises nothing, so the
 * protocol treats all of these exactly like a loss.
 */
static int transient(int e)
{
    return e == ECONNREFUSED || e == EINTR || e == EAGAIN || e == ENOBUFS;
}

/* Wait until fd is readable or the clock reaches due (-1: stopped, wait 1 s).
 * Returns 1 if readable, 0 if the time ran out, -1 if poll failed. */
static int wait_until(int fd, int64_t due)
{
    int64_t left = due < 0 ? 1000 : due - io_now_ms();
    if (left < 0) left = 0;

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int n = poll(&pfd, 1, (int)left);
    if (n < 0) { /* GCOVR_EXCL_START */
        if (errno == EINTR) return 0;
        perror("poll");
        return -1;
    } /* GCOVR_EXCL_STOP */
    return n > 0;
}

/* Receive one datagram.  Returns 1 with *p filled if it is a valid packet,
 * 0 if it was damaged or nothing usable arrived, -1 if the socket failed. */
static int recv_packet(int fd, pkt_t *p)
{
    /* One spare byte, so an oversized datagram can never pass as a full one. */
    uint8_t buf[PKT_MAX_LEN + 1u];
    ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n < 0) {
        if (transient(errno)) return 0;
        perror("recv"); /* GCOVR_EXCL_LINE */
        return -1;      /* GCOVR_EXCL_LINE */
    }
    return pkt_decode(buf, (size_t)n, p) == 0;
}

static int send_datagrams(int fd, const gbn_datagram_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (send(fd, d[i].buf, d[i].len, 0) < 0 && !transient(errno)) {
            perror("send"); /* GCOVR_EXCL_LINE */
            return -1;      /* GCOVR_EXCL_LINE */
        }
    }
    return 0;
}

int io_hello(int fd, const char *msg, int attempts, int wait_ms,
             char *reason, size_t cap)
{
    size_t mlen = strlen(msg);

    for (int a = 0; a < attempts; a++) {
        if (send(fd, msg, mlen, 0) < 0 && !transient(errno)) {
            perror("send"); /* GCOVR_EXCL_LINE */
            return -1;      /* GCOVR_EXCL_LINE */
        }
        int64_t due = io_now_ms() + wait_ms;
        while (io_now_ms() < due) {
            int ready = wait_until(fd, due);
            if (ready < 0) return -1; /* GCOVR_EXCL_LINE */
            if (ready == 0) continue;

            char    buf[256];
            ssize_t n = recv(fd, buf, sizeof buf - 1u, MSG_DONTWAIT);
            if (n < 0) {
                if (transient(errno)) continue;
                perror("recv"); /* GCOVR_EXCL_LINE */
                return -1;      /* GCOVR_EXCL_LINE */
            }
            buf[n] = '\0';
            if (strcmp(buf, "OK") == 0) return 0;
            if (strncmp(buf, "ERR", 3) == 0) {
                if (reason) snprintf(reason, cap, "%s", buf[3] == ' ' ? buf + 4 : buf + 3);
                return 1;
            }
            /* Anything else is not an answer to our hello: keep waiting. */
        }
    }
    return -1;
}

int io_read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0) { /* GCOVR_EXCL_START */
        fprintf(stderr, "error: cannot read %s\n", path);
        fclose(f);
        return -1;
    } /* GCOVR_EXCL_STOP */
    if ((unsigned long)size > GBN_MAX_FILE) {
        fprintf(stderr, "error: %s is larger than 16 MiB\n", path);
        fclose(f);
        return -1;
    }
    rewind(f);

    size_t   n   = (size_t)size;
    uint8_t *buf = malloc(n > 0 ? n : 1u);
    if (!buf || fread(buf, 1, n, f) != n) { /* GCOVR_EXCL_START */
        fprintf(stderr, "error: cannot read %s\n", path);
        free(buf);
        fclose(f);
        return -1;
    } /* GCOVR_EXCL_STOP */
    fclose(f);
    *data = buf;
    *len  = n;
    return 0;
}

int io_run_sender(int fd, const uint8_t *data, size_t len, uint32_t window,
                  int64_t timeout_ms)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    pkt_t            pkt;
    int              rc = 2;

    if (gbn_sender_init(&s, data, len, window, timeout_ms) != 0) {
        fprintf(stderr, "error: invalid sender arguments\n");
        return 2;
    }

    gbn_sender_start(&s, io_now_ms(), &out);
    if (send_datagrams(fd, out.send, out.nsend) != 0) goto done; /* GCOVR_EXCL_LINE */

    while (out.status == GBN_RUNNING) {
        int ready = wait_until(fd, out.timer_due);
        if (ready < 0) goto done; /* GCOVR_EXCL_LINE */
        if (ready > 0) {
            int got = recv_packet(fd, &pkt);
            if (got < 0) goto done; /* GCOVR_EXCL_LINE */
            if (got > 0) {
                gbn_sender_on_ack(&s, &pkt, io_now_ms(), &out);
                if (send_datagrams(fd, out.send, out.nsend) != 0) goto done; /* GCOVR_EXCL_LINE */
            }
        }
        gbn_sender_on_timer(&s, io_now_ms(), &out);
        if (send_datagrams(fd, out.send, out.nsend) != 0) goto done; /* GCOVR_EXCL_LINE */
    }

    if (out.status == GBN_DONE) {
        fprintf(stderr,
                "sent %zu bytes in %" PRIu32 " packets: %" PRIu64 " datagrams, "
                "%" PRIu64 " resent, %" PRIu64 " timeouts\n",
                len, s.ndata + 1u, s.sent, s.resent, s.total_timeouts);
        rc = 0;
    } else {
        fprintf(stderr, "giving up: %u timeouts in a row with no progress\n",
                GBN_MAX_TIMEOUTS);
    }
done:
    gbn_sender_destroy(&s);
    return rc;
}

int io_run_receiver(int fd, FILE *out_file, int64_t linger_ms, int64_t idle_ms)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    pkt_t              pkt;
    FILE              *f     = out_file;
    size_t             total = 0;
    int                rc    = 2;

    gbn_receiver_init(&r, io_now_ms(), linger_ms, idle_ms);
    gbn_receiver_on_timer(&r, io_now_ms(), &out);

    while (out.status == GBN_RUNNING) {
        int ready = wait_until(fd, out.timer_due);
        if (ready < 0) goto done; /* GCOVR_EXCL_LINE */
        if (ready > 0) {
            int got = recv_packet(fd, &pkt);
            if (got < 0) goto done; /* GCOVR_EXCL_LINE */
            if (got > 0) {
                gbn_receiver_on_packet(&r, &pkt, io_now_ms(), &out);
                if (out.deliver) {
                    if (fwrite(out.deliver, 1, out.deliver_len, f) != out.deliver_len) {
                        perror("write");
                        goto done;
                    }
                    total += out.deliver_len;
                }
                if (out.fin) {
                    int bad = fclose(f) != 0;
                    f = NULL;
                    if (bad) {
                        perror("close");
                        goto done;
                    }
                }
                if (send_datagrams(fd, &out.ack, out.ack.len > 0) != 0) goto done; /* GCOVR_EXCL_LINE */
            }
        }
        gbn_receiver_on_timer(&r, io_now_ms(), &out);
    }

    if (out.status == GBN_DONE) {
        fprintf(stderr, "received %zu bytes\n", total);
        rc = 0;
    } else {
        fprintf(stderr, "giving up: nothing valid arrived for %" PRId64 " ms\n",
                idle_ms);
    }
done:
    if (f) fclose(f);
    return rc;
}

```

### lab.h

```c

#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ── Layer 1 : Packets ───────────────────────────────────────────────────
 *
 * Pure functions: bytes in, bytes or a struct out.  No I/O.
 *
 *   offset  size  field
 *   0       1     type: 0 DATA, 1 ACK, 2 FIN
 *   1       1     reserved, always 0
 *   2       2     checksum (RFC 1071)
 *   4       4     seq
 *   8       2     length of the payload in bytes
 *   10      len   payload, at most 1024 bytes
 *
 * Multi-byte fields are in network byte order.
 */

#define PKT_HDR_LEN     10u
#define PKT_MAX_PAYLOAD 1024u
#define PKT_MAX_LEN     (PKT_HDR_LEN + PKT_MAX_PAYLOAD)

enum { PKT_DATA = 0, PKT_ACK = 1, PKT_FIN = 2 };

/** A decoded, validated packet.  The payload is a copy, not a pointer. */
typedef struct {
    uint8_t  type;
    uint32_t seq;
    uint16_t len;
    uint8_t  payload[PKT_MAX_PAYLOAD];
} pkt_t;

/**
 * RFC 1071 Internet checksum of buf[0..len-1]: the one's complement of the
 * one's complement sum of the bytes taken as big-endian 16-bit words.  An odd
 * length is padded with one zero byte for the calculation only.
 *
 * Run over a whole packet with its checksum field filled in, the result is 0
 * exactly when the packet is undamaged (the words sum to 0xffff).
 */
uint16_t pkt_checksum(const uint8_t *buf, size_t len);

/**
 * Encode one packet into buf, which must hold PKT_HDR_LEN + len bytes, and
 * fill in its checksum.  payload may be NULL when len is 0.
 *
 * Returns the number of bytes written, or 0 if buf is NULL, type is not
 * DATA/ACK/FIN, len exceeds PKT_MAX_PAYLOAD, or payload is NULL with len > 0.
 */
size_t pkt_encode(uint8_t *buf, uint8_t type, uint32_t seq,
                  const uint8_t *payload, size_t len);

/**
 * Decode and validate the datagram buf[0..len-1] into *out.  The datagram is
 * accepted only if it is at least PKT_HDR_LEN bytes, PKT_HDR_LEN + length
 * equals len, length is at most PKT_MAX_PAYLOAD, type is DATA/ACK/FIN,
 * reserved is 0, and the checksum verifies.
 *
 * Returns 0 if the packet is valid, -1 if it must be discarded.  The length
 * field is checked against len before a single payload byte is copied.
 */
int pkt_decode(const uint8_t *buf, size_t len, pkt_t *out);

/* ── Layer 2 : Go-Back-N state machines ──────────────────────────────────
 *
 * A sender and a receiver, each a struct plus the functions that feed it
 * events.  The current time is always a parameter in milliseconds, and every
 * event function fills an "out" struct that says what to do next: which
 * datagrams to send, what payload to deliver, and when the timer is due.
 * Nothing in this layer touches a socket, a clock, or a file.
 */

#define GBN_MAX_WINDOW   64u
#define GBN_MAX_TIMEOUTS 10u                  /* in a row, with no progress */
#define GBN_MAX_FILE     (16u * 1024u * 1024u)
#define GBN_LINGER_MS    2000
#define GBN_IDLE_MS      30000

typedef enum { GBN_RUNNING = 0, GBN_DONE = 1, GBN_FAILED = 2 } gbn_status_t;

/** One encoded datagram to put on the wire.  buf points into the owner. */
typedef struct {
    const uint8_t *buf;
    size_t         len;
} gbn_datagram_t;

/** What the sender wants done after an event. */
typedef struct {
    gbn_datagram_t send[GBN_MAX_WINDOW]; /* send these, in order */
    size_t         nsend;
    int64_t        timer_due;            /* absolute ms, or -1 if stopped */
    gbn_status_t   status;
} gbn_sender_out_t;

typedef struct {
    const uint8_t *data;      /* the whole file, owned by the caller */
    size_t         len;
    uint32_t       ndata;     /* number of DATA packets; the FIN is seq ndata */
    uint32_t       window;
    int64_t        timeout_ms;
    uint32_t       base;      /* oldest unacknowledged packet */
    uint32_t       next;      /* next packet never sent */
    uint8_t       *slots;     /* window * PKT_MAX_LEN bytes: copies of base..next-1 */
    size_t         slot_len[GBN_MAX_WINDOW];
    int64_t        timer_due; /* -1 when the timer is stopped */
    uint32_t       timeouts;  /* consecutive timeouts without progress */
    gbn_status_t   status;
    /* Counters for reporting only. */
    uint64_t       sent;         /* every datagram handed out, resends included */
    uint64_t       resent;       /* datagrams sent again after a timeout */
    uint64_t       total_timeouts;
} gbn_sender_t;

/**
 * Prepare a sender for len bytes at data (NULL is allowed when len is 0).
 * Allocates the retransmission buffer; release it with gbn_sender_destroy.
 * Returns 0, or -1 if window is not 1..GBN_MAX_WINDOW, timeout_ms is not
 * positive, len exceeds GBN_MAX_FILE, or the allocation fails.
 */
int gbn_sender_init(gbn_sender_t *s, const uint8_t *data, size_t len,
                    uint32_t window, int64_t timeout_ms);

/** Free the retransmission buffer.  Safe on NULL and to call twice. */
void gbn_sender_destroy(gbn_sender_t *s);

/** Fill the window for the first time.  Call once, right after init. */
void gbn_sender_start(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out);

/**
 * A valid packet arrived.  Anything other than an ACK with
 * base < seq <= next is ignored.  A new cumulative ACK slides base, resets
 * the timeout count, restarts or stops the timer, and refills the window;
 * the ACK of the FIN finishes the transfer.
 */
void gbn_sender_on_ack(gbn_sender_t *s, const pkt_t *ack, int64_t now,
                       gbn_sender_out_t *out);

/**
 * Time has passed.  If the timer is running and now >= its due time, resend
 * every packet from base to next - 1 and restart the timer, or give up
 * (GBN_FAILED) on the GBN_MAX_TIMEOUTS-th timeout in a row.  Otherwise
 * nothing happens, so it is safe to call after every wakeup.
 */
void gbn_sender_on_timer(gbn_sender_t *s, int64_t now, gbn_sender_out_t *out);

/** What the receiver wants done after an event. */
typedef struct {
    gbn_datagram_t ack;         /* send this ACK if ack.len > 0 */
    const uint8_t *deliver;     /* write these bytes to the file if non-NULL */
    size_t         deliver_len;
    int            fin;         /* 1 when the FIN was just accepted: close the file */
    int64_t        timer_due;   /* absolute ms, or -1 when finished */
    gbn_status_t   status;
} gbn_receiver_out_t;

typedef struct {
    uint32_t     expected;      /* next packet wanted */
    int          finished;      /* FIN accepted, now lingering */
    int64_t      deadline;      /* idle give-up time, or end of the linger */
    int64_t      linger_ms;
    int64_t      idle_ms;
    uint8_t      ackbuf[PKT_HDR_LEN];
    gbn_status_t status;
} gbn_receiver_t;

/**
 * Prepare a receiver that starts waiting at now.  It finishes linger_ms after
 * accepting the FIN, and gives up after idle_ms with no valid DATA or FIN.
 * The program uses GBN_LINGER_MS and GBN_IDLE_MS.
 */
void gbn_receiver_init(gbn_receiver_t *r, int64_t now, int64_t linger_ms,
                       int64_t idle_ms);

/**
 * A valid packet arrived.  DATA with seq == expected is delivered; a FIN with
 * seq == expected is accepted and starts the linger.  Every DATA or FIN is
 * answered with ACK expected, so duplicates, packets beyond a gap and repeated
 * FINs are re-ACKed and otherwise discarded.  ACKs are ignored.  out->deliver
 * points into *p, so write it out before reusing p.
 */
void gbn_receiver_on_packet(gbn_receiver_t *r, const pkt_t *p, int64_t now,
                            gbn_receiver_out_t *out);

/**
 * Time has passed.  At the deadline a lingering receiver is GBN_DONE and one
 * still waiting for data is GBN_FAILED.  Safe to call after every wakeup.
 */
void gbn_receiver_on_timer(gbn_receiver_t *r, int64_t now,
                           gbn_receiver_out_t *out);

/* ── Layer 3 : I/O ───────────────────────────────────────────────────────
 *
 * The socket, the relay hello, the poll loop, the clock, the file, and the
 * command line.  The only layer that knows any of them exist.
 */

/** Milliseconds from CLOCK_MONOTONIC. */
int64_t io_now_ms(void);

/**
 * Resolve host:port with getaddrinfo, preferring IPv4, and return a UDP socket
 * connected to it, or -1.  Connecting means send/recv talk only to the relay.
 */
int io_open(const char *host, const char *port);

/**
 * Register with the relay: send msg, then wait up to wait_ms for a reply,
 * sending again on silence, for at most attempts tries in all.
 * Returns 0 on "OK", 1 on "ERR <reason>" (reason copied to reason[0..cap-1]
 * if reason is non-NULL), or -1 if no reply came or the socket failed.
 */
int io_hello(int fd, const char *msg, int attempts, int wait_ms,
             char *reason, size_t cap);

/**
 * Read the whole of path into a malloc'd buffer the caller frees.
 * Returns 0, or -1 (message on stderr) if it cannot be read or is larger
 * than GBN_MAX_FILE.
 */
int io_read_file(const char *path, uint8_t **data, size_t *len);

/**
 * Send data[0..len-1] with Go-Back-N over the connected socket fd.
 * Returns 0 once the FIN is acknowledged, or 2 if the sender gives up,
 * the arguments are invalid, or the socket fails.
 */
int io_run_sender(int fd, const uint8_t *data, size_t len, uint32_t window,
                  int64_t timeout_ms);

/**
 * Receive a file with Go-Back-N over the connected socket fd, writing it to
 * out.  Takes ownership of out and always closes it.  Returns 0 after the
 * FIN is accepted and the linger ends, or 2 if nothing valid arrives for
 * idle_ms, the file cannot be written, or the socket fails.
 */
int io_run_receiver(int fd, FILE *out, int64_t linger_ms, int64_t idle_ms);

typedef enum { MODE_SEND = 1, MODE_RECV = 2 } app_mode_t;

typedef struct {
    app_mode_t  mode;
    const char *session;
    const char *relay;
    const char *file;
    const char *port;
    uint32_t    window;
    int64_t     timeout_ms;
    double      loss;
    double      corrupt;
    double      dup;
} options_t;

/** Print the usage message to f. */
void cli_usage(FILE *f);

/** Return 1 if s is a valid session name: 1 to 32 of a-z, 0-9 and '-'. */
int cli_valid_session(const char *s);

/** Parse a probability between 0 and 0.5 into *out.  Returns 0 or -1. */
int cli_parse_prob(const char *s, double *out);

/**
 * Parse "myapp send|recv [options] <relay> <file>" into *o, filling defaults.
 * Returns 0, or -1 with a message on stderr if the command line is wrong.
 */
int cli_parse(int argc, char *argv[], options_t *o);

/**
 * Write the relay hello for o into buf:
 *   "HELLO <session> recv"  or  "HELLO <session> send <loss> <corrupt> <dup>"
 * with the rates as plain decimals.  Returns 0, or -1 if it does not fit.
 */
int cli_hello(const options_t *o, char *buf, size_t cap);

#endif /* LAB_H */

```

### main.c

```c

#include "lab.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

#define HELLO_ATTEMPTS 5
#define HELLO_WAIT_MS  1000

int main(int argc, char *argv[])
{
    /* No arguments at all: usage and exit 0 (this is what make leak runs). */
    if (argc == 1) {
        cli_usage(stdout);
        return 0;
    }

    options_t o;
    char      hello[128];
    if (cli_parse(argc, argv, &o) != 0 || cli_hello(&o, hello, sizeof hello) != 0) {
        cli_usage(stderr);
        return 1;
    }

    /* Open the file before registering, so a bad path fails fast. */
    uint8_t *data = NULL;
    size_t   len  = 0;
    FILE    *out  = NULL;
    if (o.mode == MODE_SEND) {
        if (io_read_file(o.file, &data, &len) != 0) return 1;
    } else {
        out = fopen(o.file, "wb");
        if (!out) {
            fprintf(stderr, "error: cannot create %s: %s\n", o.file, strerror(errno));
            return 1;
        }
    }

    int rc = 2;
    int fd = io_open(o.relay, o.port);
    if (fd >= 0) {
        char reason[256];
        int  h = io_hello(fd, hello, HELLO_ATTEMPTS, HELLO_WAIT_MS, reason, sizeof reason);
        if (h == 0 && o.mode == MODE_SEND) {
            rc = io_run_sender(fd, data, len, o.window, o.timeout_ms);
        } else if (h == 0) {
            rc  = io_run_receiver(fd, out, GBN_LINGER_MS, GBN_IDLE_MS);
            out = NULL; /* io_run_receiver closed it */
        } else if (h == 1) {
            fprintf(stderr, "relay refused: %s\n", reason);
        } else {
            fprintf(stderr, "error: no reply from the relay at %s port %s\n",
                    o.relay, o.port);
        }
        close(fd);
    }

    if (out) fclose(out);
    free(data);
    return rc;
}

```

### packet.c

```c

#include "lab.h"

#include <arpa/inet.h>
#include <string.h>

uint16_t pkt_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    size_t   i;

    for (i = 0; i + 1u < len; i += 2u) {
        sum += ((uint32_t)buf[i] << 8) | buf[i + 1u];
        sum = (sum & 0xffffu) + (sum >> 16); /* fold the carry back in */
    }
    if (len & 1u) {
        sum += (uint32_t)buf[len - 1u] << 8; /* pad with one zero byte */
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

size_t pkt_encode(uint8_t *buf, uint8_t type, uint32_t seq,
                  const uint8_t *payload, size_t len)
{
    if (!buf || type > PKT_FIN || len > PKT_MAX_PAYLOAD) return 0;
    if (!payload && len > 0) return 0;

    uint32_t nseq = htonl(seq);
    uint16_t nlen = htons((uint16_t)len);

    buf[0] = type;
    buf[1] = 0;
    buf[2] = 0; /* checksum is computed with this field zero */
    buf[3] = 0;
    memcpy(buf + 4, &nseq, sizeof nseq);
    memcpy(buf + 8, &nlen, sizeof nlen);
    if (len > 0) memcpy(buf + PKT_HDR_LEN, payload, len);

    size_t   total = PKT_HDR_LEN + len;
    uint16_t ncsum = htons(pkt_checksum(buf, total));
    memcpy(buf + 2, &ncsum, sizeof ncsum);
    return total;
}

int pkt_decode(const uint8_t *buf, size_t len, pkt_t *out)
{
    if (!buf || !out || len < PKT_HDR_LEN) return -1;

    uint16_t nlen;
    memcpy(&nlen, buf + 8, sizeof nlen);
    uint16_t plen = ntohs(nlen);

    /* Trust nothing: the length field must agree with what recv returned. */
    if (plen > PKT_MAX_PAYLOAD || PKT_HDR_LEN + plen != len) return -1;
    if (buf[0] > PKT_FIN || buf[1] != 0) return -1;
    if (pkt_checksum(buf, len) != 0) return -1;

    uint32_t nseq;
    memcpy(&nseq, buf + 4, sizeof nseq);

    out->type = buf[0];
    out->seq  = ntohl(nseq);
    out->len  = plen;
    if (plen > 0) memcpy(out->payload, buf + PKT_HDR_LEN, plen);
    return 0;
}

```

## Tests Files
### lab-test.c

```c

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "harness/unity.h"
#include "../src/lab.h"

void setUp(void) {}
void tearDown(void) {}

/* ═══════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* xorshift64: the same seed gives the same run on every machine. */
static uint64_t rng_next(uint64_t *st)
{
    uint64_t x = *st;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *st = x;
}

static double rng_unit(uint64_t *st)
{
    return (double)(rng_next(st) >> 11) / 9007199254740992.0; /* [0, 1) */
}

static void fill_random(uint8_t *buf, size_t n, uint64_t seed)
{
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rng_next(&seed);
}

/* A raw datagram with any header values at all, and a correct checksum. */
static size_t raw_packet(uint8_t *buf, uint8_t type, uint8_t reserved,
                         uint32_t seq, uint16_t lenfield,
                         const uint8_t *payload, size_t plen)
{
    buf[0] = type;
    buf[1] = reserved;
    buf[2] = 0;
    buf[3] = 0;
    buf[4] = (uint8_t)(seq >> 24);
    buf[5] = (uint8_t)(seq >> 16);
    buf[6] = (uint8_t)(seq >> 8);
    buf[7] = (uint8_t)seq;
    buf[8] = (uint8_t)(lenfield >> 8);
    buf[9] = (uint8_t)lenfield;
    if (plen > 0) memcpy(buf + PKT_HDR_LEN, payload, plen);
    size_t   total = PKT_HDR_LEN + plen;
    uint16_t c     = pkt_checksum(buf, total);
    buf[2] = (uint8_t)(c >> 8);
    buf[3] = (uint8_t)c;
    return total;
}

static pkt_t make_pkt(uint8_t type, uint32_t seq, const char *payload)
{
    pkt_t p;
    memset(&p, 0, sizeof p);
    p.type = type;
    p.seq  = seq;
    if (payload) {
        p.len = (uint16_t)strlen(payload);
        memcpy(p.payload, payload, p.len);
    }
    return p;
}

/* Decode datagram i of a sender's output and check its type and seq. */
static void assert_sent(const gbn_sender_out_t *out, size_t i, uint8_t type,
                        uint32_t seq)
{
    pkt_t p;
    TEST_ASSERT_TRUE(i < out->nsend);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(out->send[i].buf, out->send[i].len, &p));
    TEST_ASSERT_EQUAL_UINT8(type, p.type);
    TEST_ASSERT_EQUAL_UINT32(seq, p.seq);
}

static void assert_ack(const gbn_receiver_out_t *out, uint32_t seq)
{
    pkt_t p;
    TEST_ASSERT_EQUAL_size_t(PKT_HDR_LEN, out->ack.len);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(out->ack.buf, out->ack.len, &p));
    TEST_ASSERT_EQUAL_UINT8(PKT_ACK, p.type);
    TEST_ASSERT_EQUAL_UINT32(seq, p.seq);
    TEST_ASSERT_EQUAL_UINT16(0, p.len);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 1: packets
 * ═══════════════════════════════════════════════════════════════════ */

static const uint8_t HI_PACKET[] = {
    0x00, 0x00, 0x96, 0x91, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x48, 0x69, 0x21,
};

void test_checksum_rfc1071_example(void)
{
    const uint8_t b[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    /* sum 0xddf2, complement 0x220d */
    TEST_ASSERT_EQUAL_HEX16(0x220d, pkt_checksum(b, sizeof b));
}

void test_checksum_odd_length(void)
{
    const uint8_t one[]   = { 0x01 };
    const uint8_t three[] = { 0xab, 0xcd, 0xef };
    TEST_ASSERT_EQUAL_HEX16(0xfeff, pkt_checksum(one, 1));     /* ~0x0100 */
    TEST_ASSERT_EQUAL_HEX16(0x6531, pkt_checksum(three, 3));   /* ~(0xabcd + 0xef00 folded) */
    TEST_ASSERT_EQUAL_HEX16(0xffff, pkt_checksum(one, 0));     /* nothing at all */
}

void test_checksum_folds_carry(void)
{
    const uint8_t b[] = { 0xff, 0xff, 0xff, 0xff };
    TEST_ASSERT_EQUAL_HEX16(0x0000, pkt_checksum(b, sizeof b));
}

void test_checksum_of_good_packet_is_zero(void)
{
    /* With the checksum in place an undamaged packet sums to 0xffff. */
    TEST_ASSERT_EQUAL_HEX16(0x0000, pkt_checksum(HI_PACKET, sizeof HI_PACKET));
}

void test_encode_hi_example(void)
{
    uint8_t buf[PKT_MAX_LEN];
    size_t  n = pkt_encode(buf, PKT_DATA, 2, (const uint8_t *)"Hi!", 3);
    TEST_ASSERT_EQUAL_size_t(13, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(HI_PACKET, buf, 13);
}

void test_encode_ack_example(void)
{
    const uint8_t want[] = { 0x01, 0x00, 0xfe, 0xfc, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00 };
    uint8_t       buf[PKT_MAX_LEN];
    TEST_ASSERT_EQUAL_size_t(10, pkt_encode(buf, PKT_ACK, 3, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, 10);
}

void test_encode_network_byte_order(void)
{
    uint8_t buf[PKT_MAX_LEN];
    uint8_t payload[PKT_MAX_PAYLOAD];
    memset(payload, 0x5a, sizeof payload);
    TEST_ASSERT_EQUAL_size_t(PKT_MAX_LEN,
                             pkt_encode(buf, PKT_DATA, 0x01020304u, payload, PKT_MAX_PAYLOAD));
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[8]); /* 1024 = 0x0400 */
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[9]);
    TEST_ASSERT_EQUAL_HEX16(0, pkt_checksum(buf, PKT_MAX_LEN));
}

void test_encode_rejects_bad_arguments(void)
{
    uint8_t buf[PKT_MAX_LEN + 1];
    uint8_t payload[PKT_MAX_PAYLOAD + 1] = { 0 };
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(NULL, PKT_DATA, 0, payload, 1));
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(buf, 3, 0, payload, 1));
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(buf, PKT_DATA, 0, payload, PKT_MAX_PAYLOAD + 1));
    TEST_ASSERT_EQUAL_size_t(0, pkt_encode(buf, PKT_DATA, 0, NULL, 1));
}

void test_decode_round_trip(void)
{
    pkt_t p;
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(HI_PACKET, sizeof HI_PACKET, &p));
    TEST_ASSERT_EQUAL_UINT8(PKT_DATA, p.type);
    TEST_ASSERT_EQUAL_UINT32(2, p.seq);
    TEST_ASSERT_EQUAL_UINT16(3, p.len);
    TEST_ASSERT_EQUAL_MEMORY("Hi!", p.payload, 3);

    uint8_t buf[PKT_MAX_LEN];
    size_t  n = pkt_encode(buf, PKT_FIN, 77, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(buf, n, &p));
    TEST_ASSERT_EQUAL_UINT8(PKT_FIN, p.type);
    TEST_ASSERT_EQUAL_UINT32(77, p.seq);
    TEST_ASSERT_EQUAL_UINT16(0, p.len);
}

void test_decode_rejects_every_single_bit_flip(void)
{
    uint8_t buf[sizeof HI_PACKET];
    pkt_t   p;
    for (size_t bit = 0; bit < sizeof HI_PACKET * 8u; bit++) {
        memcpy(buf, HI_PACKET, sizeof buf);
        buf[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, pkt_decode(buf, sizeof buf, &p), "flipped bit accepted");
    }
}

void test_decode_rejects_too_short(void)
{
    pkt_t p;
    for (size_t n = 0; n < PKT_HDR_LEN; n++)
        TEST_ASSERT_EQUAL_INT(-1, pkt_decode(HI_PACKET, n, &p));
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(NULL, 13, &p));
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(HI_PACKET, 13, NULL));
}

void test_decode_rejects_length_mismatch(void)
{
    uint8_t buf[PKT_MAX_LEN + 8];
    pkt_t   p;
    memcpy(buf, HI_PACKET, sizeof HI_PACKET);
    buf[sizeof HI_PACKET] = 0;
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, 12, &p)); /* says 3, has 2 */
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, 14, &p)); /* says 3, has 4 */

    /* Checksum is right, but the length field disagrees with the size. */
    size_t n = raw_packet(buf, PKT_DATA, 0, 1, 5, (const uint8_t *)"abc", 3);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));

    /* Consistent, checksummed, and one byte too long. */
    uint8_t big[PKT_MAX_PAYLOAD + 1];
    memset(big, 7, sizeof big);
    n = raw_packet(buf, PKT_DATA, 0, 1, PKT_MAX_PAYLOAD + 1, big, sizeof big);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
}

void test_decode_rejects_unknown_type_and_reserved(void)
{
    uint8_t buf[PKT_MAX_LEN];
    pkt_t   p;
    size_t  n = raw_packet(buf, 3, 0, 0, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
    n = raw_packet(buf, 0xff, 0, 0, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
    n = raw_packet(buf, PKT_ACK, 1, 0, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, pkt_decode(buf, n, &p));
    n = raw_packet(buf, PKT_ACK, 0, 0, 0, NULL, 0); /* the control: this one is fine */
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(buf, n, &p));
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 2: sender
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t FILE_BUF[64 * 1024 + 123];

void test_sender_init_rejects_bad_arguments(void)
{
    gbn_sender_t s;
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(NULL, FILE_BUF, 10, 4, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, 10, 0, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, 10, GBN_MAX_WINDOW + 1, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, 10, 4, 0));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, NULL, 10, 4, 250));
    TEST_ASSERT_EQUAL_INT(-1, gbn_sender_init(&s, FILE_BUF, GBN_MAX_FILE + 1u, 4, 250));
    gbn_sender_destroy(&s); /* nothing was allocated; still safe */
    gbn_sender_destroy(NULL);
}

void test_sender_empty_file_is_a_single_fin(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, NULL, 0, 4, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 0);
    TEST_ASSERT_EQUAL_INT64(250, out.timer_due);

    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, 40, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    gbn_sender_destroy(&s);
    gbn_sender_destroy(&s); /* twice is fine */
}

void test_sender_cuts_2500_bytes_like_example_1(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    pkt_t            p;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 2500, 8, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(3, out.nsend); /* the FIN waits for every ACK */
    const uint16_t lens[] = { 1024, 1024, 452 };
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(0, pkt_decode(out.send[i].buf, out.send[i].len, &p));
        TEST_ASSERT_EQUAL_UINT8(PKT_DATA, p.type);
        TEST_ASSERT_EQUAL_UINT32(i, p.seq);
        TEST_ASSERT_EQUAL_UINT16(lens[i], p.len);
        TEST_ASSERT_EQUAL_MEMORY(FILE_BUF + i * 1024u, p.payload, p.len);
    }
    pkt_t ack = make_pkt(PKT_ACK, 3, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 3);
    ack.seq = 4;
    gbn_sender_on_ack(&s, &ack, 200, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    gbn_sender_destroy(&s);
}

void test_sender_exact_multiple_of_1024(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 2048, 8, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(2, out.nsend); /* no empty third DATA packet */
    TEST_ASSERT_EQUAL_size_t(PKT_MAX_LEN, out.send[1].len);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    assert_sent(&out, 0, PKT_FIN, 2);
    gbn_sender_destroy(&s);
}

void test_sender_stops_when_window_is_full(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 1000, &out);
    TEST_ASSERT_EQUAL_size_t(4, out.nsend);
    for (uint32_t i = 0; i < 4; i++) assert_sent(&out, i, PKT_DATA, i);
    TEST_ASSERT_EQUAL_UINT32(0, s.base);
    TEST_ASSERT_EQUAL_UINT32(4, s.next);
    TEST_ASSERT_EQUAL_INT64(1250, out.timer_due);

    gbn_sender_on_timer(&s, 1249, &out); /* not due yet: nothing happens */
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_INT64(1250, out.timer_due);
    gbn_sender_destroy(&s);
}

void test_sender_cumulative_ack_slides_several(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 0, &out);

    pkt_t ack = make_pkt(PKT_ACK, 3, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    TEST_ASSERT_EQUAL_UINT32(3, s.base);
    TEST_ASSERT_EQUAL_size_t(3, out.nsend); /* window slid by three */
    for (uint32_t i = 0; i < 3; i++) assert_sent(&out, i, PKT_DATA, 4 + i);
    TEST_ASSERT_EQUAL_INT64(350, out.timer_due); /* restarted */

    /* Everything in flight acknowledged at once: timer restarts for the refill. */
    ack.seq = 7;
    gbn_sender_on_ack(&s, &ack, 200, &out);
    TEST_ASSERT_EQUAL_size_t(3, out.nsend);
    assert_sent(&out, 2, PKT_DATA, 9);
    TEST_ASSERT_EQUAL_INT64(450, out.timer_due);
    gbn_sender_destroy(&s);
}

void test_sender_lost_acks_cost_nothing_example_3(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 5 * 1024, 3, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 10, &out); /* base 2, next 5 */
    TEST_ASSERT_EQUAL_UINT32(5, s.next);
    ack.seq = 5; /* ACK 3 and ACK 4 were dropped */
    gbn_sender_on_ack(&s, &ack, 20, &out);
    TEST_ASSERT_EQUAL_UINT32(5, s.base);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 5); /* nothing resent, straight to the FIN */
    TEST_ASSERT_EQUAL_UINT64(0, s.resent);
    gbn_sender_destroy(&s);
}

void test_sender_ignores_duplicate_and_bogus_acks(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 50, &out);
    TEST_ASSERT_EQUAL_size_t(2, out.nsend);

    gbn_sender_on_ack(&s, &ack, 60, &out); /* duplicate ACK 2 */
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);
    TEST_ASSERT_EQUAL_INT64(300, out.timer_due); /* not restarted */

    ack.seq = 1; /* older still */
    gbn_sender_on_ack(&s, &ack, 70, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);

    ack.seq = 9; /* beyond next: was never sent */
    gbn_sender_on_ack(&s, &ack, 80, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);

    pkt_t data = make_pkt(PKT_DATA, 4, "x"); /* not an ACK */
    gbn_sender_on_ack(&s, &data, 90, &out);
    gbn_sender_on_ack(&s, NULL, 90, &out);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    gbn_sender_destroy(&s);
}

void test_sender_timeout_resends_whole_window(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 10 * 1024, 4, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 2, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out); /* base 2, next 6, due 350 */

    gbn_sender_on_timer(&s, 350, &out);
    TEST_ASSERT_EQUAL_size_t(4, out.nsend); /* go back to base: 2, 3, 4, 5 */
    for (uint32_t i = 0; i < 4; i++) assert_sent(&out, i, PKT_DATA, 2 + i);
    TEST_ASSERT_EQUAL_INT64(600, out.timer_due);
    TEST_ASSERT_EQUAL_UINT64(4, s.resent);
    TEST_ASSERT_EQUAL_UINT32(1, s.timeouts);
    gbn_sender_destroy(&s);
}

void test_sender_gives_up_after_10_timeouts(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 5 * 1024, 2, 100));
    gbn_sender_start(&s, 0, &out);
    int64_t now = 0;
    for (int i = 1; i < (int)GBN_MAX_TIMEOUTS; i++) {
        now = out.timer_due;
        gbn_sender_on_timer(&s, now, &out);
        TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
        TEST_ASSERT_EQUAL_size_t(2, out.nsend);
    }
    gbn_sender_on_timer(&s, out.timer_due, &out); /* the 10th */
    TEST_ASSERT_EQUAL_INT(GBN_FAILED, out.status);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);

    /* A late ACK changes nothing once the sender has given up. */
    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, now + 1000, &out);
    TEST_ASSERT_EQUAL_INT(GBN_FAILED, out.status);
    gbn_sender_on_timer(&s, now + 5000, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.nsend);
    gbn_sender_destroy(&s);
}

void test_sender_progress_resets_timeout_count(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 5 * 1024, 2, 100));
    gbn_sender_start(&s, 0, &out);
    for (int i = 1; i < (int)GBN_MAX_TIMEOUTS; i++) gbn_sender_on_timer(&s, out.timer_due, &out);
    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, out.timer_due - 1, &out);
    TEST_ASSERT_EQUAL_UINT32(0, s.timeouts);
    for (int i = 1; i < (int)GBN_MAX_TIMEOUTS; i++) gbn_sender_on_timer(&s, out.timer_due, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    TEST_ASSERT_EQUAL_UINT64(18, s.total_timeouts);
    gbn_sender_destroy(&s);
}

void test_sender_retransmits_fin_until_acked(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 100, 1, 250));
    gbn_sender_start(&s, 0, &out);
    pkt_t ack = make_pkt(PKT_ACK, 1, NULL);
    gbn_sender_on_ack(&s, &ack, 100, &out);
    assert_sent(&out, 0, PKT_FIN, 1);
    gbn_sender_on_timer(&s, 350, &out); /* its ACK was lost */
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    assert_sent(&out, 0, PKT_FIN, 1);
    ack.seq = 2;
    gbn_sender_on_ack(&s, &ack, 400, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    gbn_sender_destroy(&s);
}

void test_sender_window_1_is_stop_and_wait(void)
{
    gbn_sender_t     s;
    gbn_sender_out_t out;
    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, FILE_BUF, 3 * 1024, 1, 250));
    gbn_sender_start(&s, 0, &out);
    TEST_ASSERT_EQUAL_size_t(1, out.nsend);
    for (uint32_t i = 1; i <= 3; i++) {
        pkt_t ack = make_pkt(PKT_ACK, i, NULL);
        gbn_sender_on_ack(&s, &ack, (int64_t)i * 100, &out);
        TEST_ASSERT_EQUAL_size_t(1, out.nsend);
        assert_sent(&out, 0, i < 3 ? PKT_DATA : PKT_FIN, i);
    }
    gbn_sender_destroy(&s);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 2: receiver
 * ═══════════════════════════════════════════════════════════════════ */

void test_receiver_delivers_in_order(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t p = make_pkt(PKT_DATA, 0, "ab");
    gbn_receiver_on_packet(&r, &p, 10, &out);
    TEST_ASSERT_EQUAL_size_t(2, out.deliver_len);
    TEST_ASSERT_EQUAL_MEMORY("ab", out.deliver, 2);
    assert_ack(&out, 1);
    TEST_ASSERT_EQUAL_INT(0, out.fin);

    p = make_pkt(PKT_DATA, 1, "cde");
    gbn_receiver_on_packet(&r, &p, 20, &out);
    TEST_ASSERT_EQUAL_MEMORY("cde", out.deliver, 3);
    assert_ack(&out, 2);
}

void test_receiver_reacks_duplicate(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t p = make_pkt(PKT_DATA, 0, "ab");
    gbn_receiver_on_packet(&r, &p, 10, &out);
    gbn_receiver_on_packet(&r, &p, 20, &out);
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 1);
}

void test_receiver_discards_packet_beyond_gap(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t p = make_pkt(PKT_DATA, 1, "late");
    gbn_receiver_on_packet(&r, &p, 10, &out);
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 0); /* still waiting for 0 */
    p = make_pkt(PKT_DATA, 0, "first");
    gbn_receiver_on_packet(&r, &p, 20, &out);
    TEST_ASSERT_NOT_NULL(out.deliver);
    assert_ack(&out, 1); /* 1 was thrown away, so it is wanted next */
}

void test_receiver_fin_linger_and_repeated_fin(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t d = make_pkt(PKT_DATA, 0, "x");
    gbn_receiver_on_packet(&r, &d, 10, &out);
    pkt_t fin = make_pkt(PKT_FIN, 1, NULL);
    gbn_receiver_on_packet(&r, &fin, 100, &out);
    TEST_ASSERT_EQUAL_INT(1, out.fin);
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 2);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    TEST_ASSERT_EQUAL_INT64(2100, out.timer_due);

    /* The ACK of the FIN was lost and the FIN comes again. */
    gbn_receiver_on_packet(&r, &fin, 600, &out);
    TEST_ASSERT_EQUAL_INT(0, out.fin);
    assert_ack(&out, 2);
    TEST_ASSERT_EQUAL_INT64(2100, out.timer_due); /* the linger is not extended */
    gbn_receiver_on_packet(&r, &d, 700, &out);   /* a stale DATA too */
    TEST_ASSERT_NULL(out.deliver);
    assert_ack(&out, 2);

    gbn_receiver_on_timer(&r, 2099, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    gbn_receiver_on_timer(&r, 2100, &out);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, out.status);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);

    gbn_receiver_on_packet(&r, &fin, 3000, &out); /* gone: no answer */
    TEST_ASSERT_EQUAL_size_t(0, out.ack.len);
}

void test_receiver_empty_file(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t fin = make_pkt(PKT_FIN, 0, NULL);
    gbn_receiver_on_packet(&r, &fin, 5, &out);
    TEST_ASSERT_EQUAL_INT(1, out.fin);
    assert_ack(&out, 1);
}

void test_receiver_fin_out_of_order_is_reacked(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t fin = make_pkt(PKT_FIN, 2, NULL);
    gbn_receiver_on_packet(&r, &fin, 5, &out);
    TEST_ASSERT_EQUAL_INT(0, out.fin);
    assert_ack(&out, 0);
}

void test_receiver_ignores_acks(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    pkt_t ack = make_pkt(PKT_ACK, 0, NULL);
    gbn_receiver_on_packet(&r, &ack, 5, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.ack.len);
    TEST_ASSERT_NULL(out.deliver);
    gbn_receiver_on_packet(&r, NULL, 5, &out);
    TEST_ASSERT_EQUAL_size_t(0, out.ack.len);
    TEST_ASSERT_EQUAL_INT64(GBN_IDLE_MS, out.timer_due); /* not a sign of life */
}

void test_receiver_gives_up_when_idle(void)
{
    gbn_receiver_t     r;
    gbn_receiver_out_t out;
    gbn_receiver_init(&r, 0, GBN_LINGER_MS, GBN_IDLE_MS);
    gbn_receiver_on_timer(&r, 29999, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    pkt_t d = make_pkt(PKT_DATA, 5, "x"); /* out of order, but valid: still alive */
    gbn_receiver_on_packet(&r, &d, 10000, &out);
    TEST_ASSERT_EQUAL_INT64(40000, out.timer_due);
    gbn_receiver_on_timer(&r, 39999, &out);
    TEST_ASSERT_EQUAL_INT(GBN_RUNNING, out.status);
    gbn_receiver_on_timer(&r, 40000, &out);
    TEST_ASSERT_EQUAL_INT(GBN_FAILED, out.status);
    TEST_ASSERT_EQUAL_INT64(-1, out.timer_due);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 2 end to end: both state machines through a lossy in-memory channel
 * ═══════════════════════════════════════════════════════════════════ */

#define CH_MAX   8192
#define CH_DELAY 50 /* ms each way, like the relay's --delay 50 */

typedef struct {
    uint8_t  buf[PKT_MAX_LEN];
    size_t   len;
    int64_t  at;
    uint64_t order;
    int      to_receiver;
} flight_t;

typedef struct {
    flight_t *q;
    size_t    n;
    uint64_t  order;
    uint64_t  rng;
    double    loss, corrupt, dup;
    unsigned  dropped, corrupted, duplicated;
} channel_t;

static void ch_push(channel_t *c, const uint8_t *buf, size_t len, int to_receiver, int64_t now)
{
    TEST_ASSERT_TRUE(c->n < CH_MAX);
    flight_t *f = &c->q[c->n++];
    memcpy(f->buf, buf, len);
    f->len         = len;
    f->at          = now + CH_DELAY;
    f->order       = c->order++;
    f->to_receiver = to_receiver;
}

/* Damage a datagram exactly the way the relay does: drop, else flip one bit,
 * else deliver twice, each independently for every datagram. */
static void ch_send(channel_t *c, const uint8_t *buf, size_t len, int to_receiver, int64_t now)
{
    if (rng_unit(&c->rng) < c->loss) {
        c->dropped++;
        return;
    }
    if (rng_unit(&c->rng) < c->corrupt) {
        uint8_t tmp[PKT_MAX_LEN];
        memcpy(tmp, buf, len);
        size_t bit = (size_t)(rng_next(&c->rng) % (len * 8u));
        tmp[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        ch_push(c, tmp, len, to_receiver, now);
        c->corrupted++;
        return;
    }
    ch_push(c, buf, len, to_receiver, now);
    if (rng_unit(&c->rng) < c->dup) {
        ch_push(c, buf, len, to_receiver, now);
        c->duplicated++;
    }
}

/* The datagram that arrives first (FIFO among equal times), or -1. */
static long ch_first(const channel_t *c)
{
    long best = -1;
    for (size_t i = 0; i < c->n; i++) {
        if (best < 0 || c->q[i].at < c->q[best].at ||
            (c->q[i].at == c->q[best].at && c->q[i].order < c->q[best].order))
            best = (long)i;
    }
    return best;
}

typedef struct {
    gbn_status_t sender, receiver;
    size_t       got_len;
    int          match;
    int          fin_seen;
    uint64_t     resent;
    int64_t      elapsed;
} transfer_t;

/* Run a whole transfer on a virtual clock, one event at a time. */
static transfer_t run_transfer(const uint8_t *data, size_t len, uint32_t window,
                               double rate, uint64_t seed)
{
    transfer_t res;
    memset(&res, 0, sizeof res);
    channel_t c;
    memset(&c, 0, sizeof c);
    c.q    = malloc(CH_MAX * sizeof *c.q);
    c.rng  = seed;
    c.loss = c.corrupt = c.dup = rate;
    TEST_ASSERT_NOT_NULL(c.q);
    uint8_t *got = malloc(len + 1u);
    TEST_ASSERT_NOT_NULL(got);

    gbn_sender_t       s;
    gbn_sender_out_t   so;
    gbn_receiver_t     r;
    gbn_receiver_out_t ro;
    pkt_t              p;
    int64_t            now = 0;

    TEST_ASSERT_EQUAL_INT(0, gbn_sender_init(&s, data, len, window, 250));
    gbn_receiver_init(&r, now, GBN_LINGER_MS, GBN_IDLE_MS);
    gbn_receiver_on_timer(&r, now, &ro);
    gbn_sender_start(&s, now, &so);
    for (size_t i = 0; i < so.nsend; i++) ch_send(&c, so.send[i].buf, so.send[i].len, 1, now);

    for (long steps = 0; steps < 1000000; steps++) {
        if (so.status != GBN_RUNNING && ro.status != GBN_RUNNING) break;
        long    i = ch_first(&c);
        int64_t t = i >= 0 ? c.q[i].at : INT64_MAX;
        if (so.status == GBN_RUNNING && so.timer_due >= 0 && so.timer_due < t) t = so.timer_due;
        if (ro.status == GBN_RUNNING && ro.timer_due < t) t = ro.timer_due;
        TEST_ASSERT_TRUE_MESSAGE(t != INT64_MAX, "deadlock: nothing in flight and no timer");
        now = t;

        if (i >= 0 && c.q[i].at == now) {
            flight_t f = c.q[i];
            c.q[i]     = c.q[--c.n];
            if (pkt_decode(f.buf, f.len, &p) != 0) continue; /* damaged: as if lost */
            if (f.to_receiver && ro.status == GBN_RUNNING) {
                gbn_receiver_on_packet(&r, &p, now, &ro);
                if (ro.deliver) {
                    TEST_ASSERT_TRUE(res.got_len + ro.deliver_len <= len);
                    memcpy(got + res.got_len, ro.deliver, ro.deliver_len);
                    res.got_len += ro.deliver_len;
                }
                if (ro.fin) res.fin_seen++;
                if (ro.ack.len) ch_send(&c, ro.ack.buf, ro.ack.len, 0, now);
            } else if (!f.to_receiver && so.status == GBN_RUNNING) {
                gbn_sender_on_ack(&s, &p, now, &so);
                for (size_t k = 0; k < so.nsend; k++) ch_send(&c, so.send[k].buf, so.send[k].len, 1, now);
            }
        } else if (so.status == GBN_RUNNING && so.timer_due == now) {
            gbn_sender_on_timer(&s, now, &so);
            for (size_t k = 0; k < so.nsend; k++) ch_send(&c, so.send[k].buf, so.send[k].len, 1, now);
        } else {
            gbn_receiver_on_timer(&r, now, &ro);
        }
    }

    res.sender   = so.status;
    res.receiver = ro.status;
    res.match    = res.got_len == len && (len == 0 || memcmp(got, data, len) == 0);
    res.resent   = s.resent;
    res.elapsed  = now;
    gbn_sender_destroy(&s);
    free(got);
    free(c.q);
    return res;
}

static void assert_transfer_ok(transfer_t t)
{
    TEST_ASSERT_EQUAL_INT(GBN_DONE, t.sender);
    TEST_ASSERT_EQUAL_INT(GBN_DONE, t.receiver);
    TEST_ASSERT_EQUAL_INT(1, t.fin_seen);
    TEST_ASSERT_TRUE_MESSAGE(t.match, "delivered bytes differ from the bytes sent");
}

void test_transfer_clean_channel(void)
{
    fill_random(FILE_BUF, sizeof FILE_BUF, 42);
    transfer_t t = run_transfer(FILE_BUF, 10 * 1024 + 300, 1, 0.0, 1);
    assert_transfer_ok(t);
    TEST_ASSERT_EQUAL_UINT64(0, t.resent);
    /* Stop-and-wait: 11 DATA + FIN, one 100 ms round trip each, then the linger. */
    TEST_ASSERT_EQUAL_INT64(12 * 100 - 50 + GBN_LINGER_MS, t.elapsed);

    t = run_transfer(FILE_BUF, 10 * 1024 + 300, 16, 0.0, 1);
    assert_transfer_ok(t);
}

void test_transfer_lossy_20_percent_seeded(void)
{
    /* 20% loss, 20% corruption and 20% duplication, in each direction. */
    fill_random(FILE_BUF, sizeof FILE_BUF, 7);
    const uint64_t seeds[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0x5eed, 0xc5425 };
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        transfer_t t = run_transfer(FILE_BUF, sizeof FILE_BUF, 8, 0.2, seeds[i]);
        assert_transfer_ok(t);
        TEST_ASSERT_TRUE(t.resent > 0); /* the channel really did bite */
    }
}

void test_transfer_lossy_stop_and_wait_and_big_window(void)
{
    fill_random(FILE_BUF, sizeof FILE_BUF, 99);
    assert_transfer_ok(run_transfer(FILE_BUF, 20 * 1024 + 1, 1, 0.2, 11));
    assert_transfer_ok(run_transfer(FILE_BUF, sizeof FILE_BUF, GBN_MAX_WINDOW, 0.2, 12));
}

void test_transfer_lossy_empty_and_exact_multiple(void)
{
    fill_random(FILE_BUF, sizeof FILE_BUF, 3);
    assert_transfer_ok(run_transfer(NULL, 0, 8, 0.2, 21));
    assert_transfer_ok(run_transfer(FILE_BUF, 16 * 1024, 8, 0.2, 22));
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 3: I/O over real loopback sockets, with the test playing the relay
 * ═══════════════════════════════════════════════════════════════════ */

static void sleep_ms(int ms)
{
    poll(NULL, 0, ms);
}

/* A UDP socket on 127.0.0.1 that plays the relay; its port goes into port. */
static int fake_relay(char *port, size_t cap)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, bind(fd, (struct sockaddr *)&a, sizeof a));
    socklen_t n = sizeof a;
    TEST_ASSERT_EQUAL_INT(0, getsockname(fd, (struct sockaddr *)&a, &n));
    snprintf(port, cap, "%u", (unsigned)ntohs(a.sin_port));
    return fd;
}

/* Aim the relay at the client, so replies can be queued before it asks. */
static void relay_aim(int relay, int client)
{
    struct sockaddr_storage a;
    socklen_t               n = sizeof a;
    TEST_ASSERT_EQUAL_INT(0, getsockname(client, (struct sockaddr *)&a, &n));
    TEST_ASSERT_EQUAL_INT(0, connect(relay, (struct sockaddr *)&a, n));
}

static void relay_say(int relay, const char *text)
{
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)send(relay, text, strlen(text), 0));
}

static void relay_send_pkt(int relay, uint8_t type, uint32_t seq, const char *payload)
{
    uint8_t buf[PKT_MAX_LEN];
    size_t  n = pkt_encode(buf, type, seq, (const uint8_t *)payload, payload ? strlen(payload) : 0);
    TEST_ASSERT_EQUAL_INT((int)n, (int)send(relay, buf, n, 0));
}

/* Read one datagram the client sent to the relay, without waiting long. */
static ssize_t relay_read(int relay, uint8_t *buf, size_t cap)
{
    struct pollfd pfd = { .fd = relay, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 200) <= 0) return -1;
    return recv(relay, buf, cap, 0);
}

static void relay_expect_pkt(int relay, uint8_t type, uint32_t seq, pkt_t *p)
{
    uint8_t buf[PKT_MAX_LEN + 1];
    ssize_t n = relay_read(relay, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(0, pkt_decode(buf, (size_t)n, p));
    TEST_ASSERT_EQUAL_UINT8(type, p->type);
    TEST_ASSERT_EQUAL_UINT32(seq, p->seq);
}

static void temp_path(char *path, size_t cap)
{
    snprintf(path, cap, "/tmp/cs425-p2-test-XXXXXX");
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
}

void test_io_now_ms_is_monotonic(void)
{
    int64_t a = io_now_ms();
    sleep_ms(5);
    int64_t b = io_now_ms();
    TEST_ASSERT_TRUE(b >= a + 4);
}

void test_io_open_rejects_bad_port(void)
{
    TEST_ASSERT_EQUAL_INT(-1, io_open("127.0.0.1", "not-a-port"));
}

void test_io_hello_ok_by_name(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("localhost", port); /* a name, not a dotted quad */
    TEST_ASSERT_TRUE(fd >= 0);
    relay_aim(relay, fd);
    relay_say(relay, "OK");
    TEST_ASSERT_EQUAL_INT(0, io_hello(fd, "HELLO t-1 recv", 5, 1000, NULL, 0));

    char    buf[64];
    ssize_t n = relay_read(relay, (uint8_t *)buf, sizeof buf - 1);
    TEST_ASSERT_EQUAL_INT(14, (int)n);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("HELLO t-1 recv", buf); /* no newline */
    close(fd);
    close(relay);
}

void test_io_hello_err_reports_reason(void)
{
    char port[16], reason[64];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    relay_say(relay, "ERR no receiver");
    TEST_ASSERT_EQUAL_INT(1, io_hello(fd, "HELLO t-1 send 0 0 0", 5, 1000, reason, sizeof reason));
    TEST_ASSERT_EQUAL_STRING("no receiver", reason);

    relay_say(relay, "ERR");
    TEST_ASSERT_EQUAL_INT(1, io_hello(fd, "HELLO t-1 send 0 0 0", 5, 1000, reason, sizeof reason));
    TEST_ASSERT_EQUAL_STRING("", reason);
    relay_say(relay, "ERR x");
    TEST_ASSERT_EQUAL_INT(1, io_hello(fd, "HELLO t-1 send 0 0 0", 5, 1000, NULL, 0));
    close(fd);
    close(relay);
}

void test_io_hello_ignores_noise(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    relay_say(relay, "WHAT");
    relay_say(relay, "OK");
    TEST_ASSERT_EQUAL_INT(0, io_hello(fd, "HELLO t-1 recv", 5, 1000, NULL, 0));
    close(fd);
    close(relay);
}

void test_io_hello_retries_on_silence(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    TEST_ASSERT_EQUAL_INT(-1, io_hello(fd, "HELLO t-1 recv", 3, 20, NULL, 0));
    uint8_t buf[64];
    int     hellos = 0;
    while (relay_read(relay, buf, sizeof buf) > 0) hellos++;
    TEST_ASSERT_EQUAL_INT(3, hellos);
    close(fd);
    close(relay);
}

void test_io_hello_with_no_relay_running(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    close(relay); /* nobody listens there now: ICMP port unreachable */
    int fd = io_open("127.0.0.1", port);
    TEST_ASSERT_EQUAL_INT(-1, io_hello(fd, "HELLO t-1 recv", 2, 30, NULL, 0));
    close(fd);
}

void test_io_read_file(void)
{
    char path[64];
    temp_path(path, sizeof path);
    fill_random(FILE_BUF, 2500, 5);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(FILE_BUF, 1, 2500, f);
    fclose(f);

    uint8_t *data = NULL;
    size_t   len  = 0;
    TEST_ASSERT_EQUAL_INT(0, io_read_file(path, &data, &len));
    TEST_ASSERT_EQUAL_size_t(2500, len);
    TEST_ASSERT_EQUAL_MEMORY(FILE_BUF, data, 2500);
    free(data);

    f = fopen(path, "wb"); /* now empty */
    fclose(f);
    TEST_ASSERT_EQUAL_INT(0, io_read_file(path, &data, &len));
    TEST_ASSERT_EQUAL_size_t(0, len);
    free(data);

    TEST_ASSERT_EQUAL_INT(0, truncate(path, (off_t)GBN_MAX_FILE + 1));
    TEST_ASSERT_EQUAL_INT(-1, io_read_file(path, &data, &len));
    unlink(path);
    TEST_ASSERT_EQUAL_INT(-1, io_read_file(path, &data, &len)); /* gone */
}

void test_io_run_sender_clean(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    fill_random(FILE_BUF, 1500, 9);

    /* The relay's side of the conversation, queued up in advance. */
    relay_say(relay, "junk");
    relay_send_pkt(relay, PKT_ACK, 1, NULL);
    relay_send_pkt(relay, PKT_ACK, 1, NULL); /* duplicate */
    relay_send_pkt(relay, PKT_ACK, 2, NULL);
    relay_send_pkt(relay, PKT_ACK, 3, NULL);
    TEST_ASSERT_EQUAL_INT(0, io_run_sender(fd, FILE_BUF, 1500, 4, 200));

    pkt_t p;
    relay_expect_pkt(relay, PKT_DATA, 0, &p);
    TEST_ASSERT_EQUAL_MEMORY(FILE_BUF, p.payload, 1024);
    relay_expect_pkt(relay, PKT_DATA, 1, &p);
    TEST_ASSERT_EQUAL_UINT16(476, p.len);
    TEST_ASSERT_EQUAL_MEMORY(FILE_BUF + 1024, p.payload, 476);
    relay_expect_pkt(relay, PKT_FIN, 2, &p);
    close(fd);
    close(relay);
}

void test_io_run_sender_gives_up(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    TEST_ASSERT_EQUAL_INT(2, io_run_sender(fd, FILE_BUF, 3000, 4, 2));
    TEST_ASSERT_EQUAL_INT(2, io_run_sender(fd, FILE_BUF, 3000, 0, 2)); /* bad window */
    close(relay);
    /* With the relay gone the sends bounce, and it still gives up cleanly. */
    TEST_ASSERT_EQUAL_INT(2, io_run_sender(fd, FILE_BUF, 3000, 4, 2));
    close(fd);
}

void test_io_run_receiver_writes_in_order(void)
{
    char port[16], path[64];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    temp_path(path, sizeof path);

    relay_send_pkt(relay, PKT_DATA, 0, "hello ");
    relay_send_pkt(relay, PKT_DATA, 0, "hello "); /* duplicate */
    relay_send_pkt(relay, PKT_DATA, 2, "!");      /* beyond the gap */
    uint8_t bad[PKT_MAX_LEN];
    size_t  n = pkt_encode(bad, PKT_DATA, 1, (const uint8_t *)"world", 5);
    bad[12] ^= 0x10; /* one flipped bit */
    TEST_ASSERT_EQUAL_INT((int)n, (int)send(relay, bad, n, 0));
    relay_send_pkt(relay, PKT_DATA, 1, "world");
    relay_send_pkt(relay, PKT_DATA, 2, "!");
    relay_send_pkt(relay, PKT_FIN, 3, NULL);
    relay_send_pkt(relay, PKT_FIN, 3, NULL); /* its ACK was "lost" */

    FILE *f = fopen(path, "wb");
    TEST_ASSERT_EQUAL_INT(0, io_run_receiver(fd, f, 30, 1000));

    const uint32_t acks[] = { 1, 1, 1, 2, 3, 4, 4 }; /* nothing for the damaged one */
    pkt_t          p;
    for (size_t i = 0; i < sizeof acks / sizeof acks[0]; i++)
        relay_expect_pkt(relay, PKT_ACK, acks[i], &p);

    char  text[32] = { 0 };
    FILE *in       = fopen(path, "rb");
    TEST_ASSERT_EQUAL_size_t(12, fread(text, 1, sizeof text, in));
    fclose(in);
    TEST_ASSERT_EQUAL_STRING("hello world!", text);
    unlink(path);
    close(fd);
    close(relay);
}

void test_io_run_receiver_gives_up_when_idle(void)
{
    char port[16], path[64];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);
    temp_path(path, sizeof path);
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, fopen(path, "wb"), 30, 30));

    /* The relay vanishes after one packet: the ACK bounces, then silence. */
    relay_send_pkt(relay, PKT_DATA, 0, "x");
    close(relay);
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, fopen(path, "wb"), 30, 50));
    unlink(path);
    close(fd);
}

void test_io_run_receiver_write_failures(void)
{
    char port[16];
    int  relay = fake_relay(port, sizeof port);
    int  fd    = io_open("127.0.0.1", port);
    relay_aim(relay, fd);

    /* Unbuffered, so the write itself fails. */
    FILE *f = fopen("/dev/full", "wb");
    TEST_ASSERT_NOT_NULL(f);
    setvbuf(f, NULL, _IONBF, 0);
    relay_send_pkt(relay, PKT_DATA, 0, "x");
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, f, 30, 1000));

    /* Buffered, so the failure only shows when the FIN closes the file. */
    f = fopen("/dev/full", "wb");
    relay_send_pkt(relay, PKT_DATA, 0, "x");
    relay_send_pkt(relay, PKT_FIN, 1, NULL);
    TEST_ASSERT_EQUAL_INT(2, io_run_receiver(fd, f, 30, 1000));
    close(fd);
    close(relay);
}

/* ═══════════════════════════════════════════════════════════════════
 * Layer 3: command line
 * ═══════════════════════════════════════════════════════════════════ */

static int parse_args(options_t *o, int argc, const char *const *args)
{
    char *argv[32];
    for (int i = 0; i < argc; i++) argv[i] = (char *)args[i];
    argv[argc] = NULL;
    return cli_parse(argc, argv, o);
}

#define PARSE(o, ...)                                                               \
    parse_args((o), (int)(sizeof((const char *[]){ __VA_ARGS__ }) / sizeof(char *)), \
               (const char *[]){ __VA_ARGS__ })

void test_cli_usage(void)
{
    FILE *f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    cli_usage(f);
    rewind(f);
    char line[128];
    TEST_ASSERT_NOT_NULL(fgets(line, sizeof line, f));
    TEST_ASSERT_EQUAL_STRING("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n", line);
    fclose(f);
}

void test_cli_valid_session(void)
{
    TEST_ASSERT_TRUE(cli_valid_session("jdoe-1"));
    TEST_ASSERT_TRUE(cli_valid_session("a"));
    TEST_ASSERT_TRUE(cli_valid_session("abcdefghijklmnopqrstuvwxyz-01234")); /* 32 */
    TEST_ASSERT_FALSE(cli_valid_session("abcdefghijklmnopqrstuvwxyz-012345"));
    TEST_ASSERT_FALSE(cli_valid_session(""));
    TEST_ASSERT_FALSE(cli_valid_session("JDoe"));
    TEST_ASSERT_FALSE(cli_valid_session("a_b"));
    TEST_ASSERT_FALSE(cli_valid_session("a b"));
    TEST_ASSERT_FALSE(cli_valid_session(NULL));
}

void test_cli_parse_prob(void)
{
    double v = -1;
    TEST_ASSERT_EQUAL_INT(0, cli_parse_prob("0", &v));
    TEST_ASSERT_TRUE(v == 0.0);
    TEST_ASSERT_EQUAL_INT(0, cli_parse_prob("0.5", &v));
    TEST_ASSERT_TRUE(v == 0.5);
    TEST_ASSERT_EQUAL_INT(0, cli_parse_prob(".05", &v));
    TEST_ASSERT_TRUE(v == 0.05);
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("0.51", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("-0.1", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("abc", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("0.1x", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("nan", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("1e999", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob("", &v));
    TEST_ASSERT_EQUAL_INT(-1, cli_parse_prob(NULL, &v));
}

void test_cli_parse_send_defaults(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "send", "-s", "jdoe-1", "127.0.0.1", "in.bin"));
    TEST_ASSERT_EQUAL_INT(MODE_SEND, o.mode);
    TEST_ASSERT_EQUAL_STRING("jdoe-1", o.session);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", o.relay);
    TEST_ASSERT_EQUAL_STRING("in.bin", o.file);
    TEST_ASSERT_EQUAL_STRING("4250", o.port);
    TEST_ASSERT_EQUAL_UINT32(8, o.window);
    TEST_ASSERT_EQUAL_INT64(250, o.timeout_ms);
    TEST_ASSERT_TRUE(o.loss == 0.0);
    TEST_ASSERT_TRUE(o.corrupt == 0.0);
    TEST_ASSERT_TRUE(o.dup == 0.0);
}

void test_cli_parse_send_all_options(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "send", "-s", "jdoe-1", "-w", "16", "-T", "100",
                                   "-l", "0.1", "-c", "0.05", "-d", "0.2", "-p", "20000",
                                   "relay.example.com", "in.bin"));
    TEST_ASSERT_EQUAL_UINT32(16, o.window);
    TEST_ASSERT_EQUAL_INT64(100, o.timeout_ms);
    TEST_ASSERT_TRUE(o.loss == 0.1);
    TEST_ASSERT_TRUE(o.corrupt == 0.05);
    TEST_ASSERT_TRUE(o.dup == 0.2);
    TEST_ASSERT_EQUAL_STRING("20000", o.port);
    TEST_ASSERT_EQUAL_STRING("relay.example.com", o.relay);
}

void test_cli_parse_recv(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "recv", "-s", "jdoe-1", "-p", "30000", "127.0.0.1", "out.bin"));
    TEST_ASSERT_EQUAL_INT(MODE_RECV, o.mode);
    TEST_ASSERT_EQUAL_STRING("30000", o.port);
    TEST_ASSERT_EQUAL_STRING("out.bin", o.file);
}

void test_cli_parse_errors(void)
{
    options_t o;
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "bogus", "-s", "a", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "recv", "-s", "a", "-w", "4", "h", "f")); /* send-only */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "h", "f"));                       /* no -s */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "Bad_Name", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "0", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "65", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "4x", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-w", "99999999999999999999", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-T", "0", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-l", "0.6", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-c", "x", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-d", "-1", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-p", "0", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "-p", "70000", "h", "f"));
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "h"));           /* no file */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s", "a", "h", "f", "x")); /* extra */
    TEST_ASSERT_EQUAL_INT(-1, PARSE(&o, "myapp", "send", "-s"));                     /* -s needs a value */
    /* And a good one right after the failures still parses: getopt was reset. */
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "recv", "-s", "a", "h", "f"));
}

void test_cli_hello(void)
{
    options_t o;
    char      buf[128];
    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "recv", "-s", "jdoe-1", "h", "f"));
    TEST_ASSERT_EQUAL_INT(0, cli_hello(&o, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 recv", buf);

    TEST_ASSERT_EQUAL_INT(0, PARSE(&o, "myapp", "send", "-s", "jdoe-1", "-l", "0.1", "-c", ".05", "h", "f"));
    TEST_ASSERT_EQUAL_INT(0, cli_hello(&o, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 send 0.1 0.05 0", buf);

    o.dup = 0.5;
    TEST_ASSERT_EQUAL_INT(0, cli_hello(&o, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 send 0.1 0.05 0.5", buf);
    TEST_ASSERT_EQUAL_INT(-1, cli_hello(&o, buf, 10)); /* does not fit */
}

int main(void)
{
    UNITY_BEGIN();
    /* Layer 1 */
    RUN_TEST(test_checksum_rfc1071_example);
    RUN_TEST(test_checksum_odd_length);
    RUN_TEST(test_checksum_folds_carry);
    RUN_TEST(test_checksum_of_good_packet_is_zero);
    RUN_TEST(test_encode_hi_example);
    RUN_TEST(test_encode_ack_example);
    RUN_TEST(test_encode_network_byte_order);
    RUN_TEST(test_encode_rejects_bad_arguments);
    RUN_TEST(test_decode_round_trip);
    RUN_TEST(test_decode_rejects_every_single_bit_flip);
    RUN_TEST(test_decode_rejects_too_short);
    RUN_TEST(test_decode_rejects_length_mismatch);
    RUN_TEST(test_decode_rejects_unknown_type_and_reserved);
    /* Layer 2: sender */
    RUN_TEST(test_sender_init_rejects_bad_arguments);
    RUN_TEST(test_sender_empty_file_is_a_single_fin);
    RUN_TEST(test_sender_cuts_2500_bytes_like_example_1);
    RUN_TEST(test_sender_exact_multiple_of_1024);
    RUN_TEST(test_sender_stops_when_window_is_full);
    RUN_TEST(test_sender_cumulative_ack_slides_several);
    RUN_TEST(test_sender_lost_acks_cost_nothing_example_3);
    RUN_TEST(test_sender_ignores_duplicate_and_bogus_acks);
    RUN_TEST(test_sender_timeout_resends_whole_window);
    RUN_TEST(test_sender_gives_up_after_10_timeouts);
    RUN_TEST(test_sender_progress_resets_timeout_count);
    RUN_TEST(test_sender_retransmits_fin_until_acked);
    RUN_TEST(test_sender_window_1_is_stop_and_wait);
    /* Layer 2: receiver */
    RUN_TEST(test_receiver_delivers_in_order);
    RUN_TEST(test_receiver_reacks_duplicate);
    RUN_TEST(test_receiver_discards_packet_beyond_gap);
    RUN_TEST(test_receiver_fin_linger_and_repeated_fin);
    RUN_TEST(test_receiver_empty_file);
    RUN_TEST(test_receiver_fin_out_of_order_is_reacked);
    RUN_TEST(test_receiver_ignores_acks);
    RUN_TEST(test_receiver_gives_up_when_idle);
    /* Layer 2: end to end */
    RUN_TEST(test_transfer_clean_channel);
    RUN_TEST(test_transfer_lossy_20_percent_seeded);
    RUN_TEST(test_transfer_lossy_stop_and_wait_and_big_window);
    RUN_TEST(test_transfer_lossy_empty_and_exact_multiple);
    /* Layer 3 */
    RUN_TEST(test_io_now_ms_is_monotonic);
    RUN_TEST(test_io_open_rejects_bad_port);
    RUN_TEST(test_io_hello_ok_by_name);
    RUN_TEST(test_io_hello_err_reports_reason);
    RUN_TEST(test_io_hello_ignores_noise);
    RUN_TEST(test_io_hello_retries_on_silence);
    RUN_TEST(test_io_hello_with_no_relay_running);
    RUN_TEST(test_io_read_file);
    RUN_TEST(test_io_run_sender_clean);
    RUN_TEST(test_io_run_sender_gives_up);
    RUN_TEST(test_io_run_receiver_writes_in_order);
    RUN_TEST(test_io_run_receiver_gives_up_when_idle);
    RUN_TEST(test_io_run_receiver_write_failures);
    RUN_TEST(test_cli_usage);
    RUN_TEST(test_cli_valid_session);
    RUN_TEST(test_cli_parse_prob);
    RUN_TEST(test_cli_parse_send_defaults);
    RUN_TEST(test_cli_parse_send_all_options);
    RUN_TEST(test_cli_parse_recv);
    RUN_TEST(test_cli_parse_errors);
    RUN_TEST(test_cli_hello);
    return UNITY_END();
}

```

## Scripts Files
### measure.sh

```c

#!/usr/bin/env bash
# Task 6: time a 1 MiB transfer through the relay at --delay 50, three runs for
# each window/loss combination, checking every copy with cmp.
#
#   make release && scripts/measure.sh [port]
#
# Prints one line per run and a Markdown table of the means at the end.
set -u
cd "$(dirname "$0")/.."

PORT=${1:-$((20000 + RANDOM % 40000))}
APP=./build/release/myapp
RUNS=3
WORK=$(mktemp -d)
trap 'kill $RELAY 2>/dev/null; rm -rf "$WORK"' EXIT

[[ -x $APP ]] || { echo "build first: make release" >&2; exit 1; }
head -c 1048576 /dev/urandom > "$WORK/1mib.bin"

python3 scripts/cs425_relay.py --port "$PORT" --delay 50 > "$WORK/relay.log" 2>&1 &
RELAY=$!
sleep 0.5

TIMEFORMAT=%R
TABLE=""
n=0
for combo in "1 0" "16 0" "1 0.05" "16 0.05"; do
    read -r W L <<< "$combo"
    total=0
    for run in $(seq 1 $RUNS); do
        n=$((n + 1))
        S="m-$$-$n"
        rm -f "$WORK/out.bin"
        $APP recv -s "$S" -p "$PORT" 127.0.0.1 "$WORK/out.bin" 2> /dev/null &
        R=$!
        sleep 0.2
        secs=$( { time $APP send -s "$S" -p "$PORT" -w "$W" -l "$L" 127.0.0.1 "$WORK/1mib.bin" 2> "$WORK/send.err"; } 2>&1 )
        wait $R
        if cmp -s "$WORK/1mib.bin" "$WORK/out.bin"; then ok=identical; else ok=DIFFERENT; fi
        echo "window $W loss $L run $run: ${secs}s $ok ($(cat "$WORK/send.err"))"
        total=$(echo "$total + $secs" | bc -l)
    done
    mean=$(echo "$total / $RUNS" | bc -l)
    TABLE+=$(printf '| %s | %s | 0 | 0 | %.2f | %.1f |' "$W" "$L" "$mean" "$(echo "1024 / $mean" | bc -l)")$'\n'
done

echo
echo "| Window | Loss | Corrupt | Dup | Mean time (s) | Throughput (KiB/s) |"
echo "|-------:|-----:|--------:|----:|--------------:|-------------------:|"
printf '%s' "$TABLE"

```

Report generated on 10/12/2026 at 03:53:54


---

## End of Report

SHA-256 Hash of the report: 45bb53e8a14543285d4faec6e7a52a0475952d7d73bff3652d22d8ad2a5377c6

Do not edit the generated report. Any changes will be reported as academic dishonesty

---
## GitHub Info
- GitHub repo name: christopherhasti/cs425-p2
- The repository visibility is public.
- The workflow was triggered by christopherhasti
