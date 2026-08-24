# Opt-in BYO build of the Keel lwIP reference.
#
# lwIP is NOT vendored; point LWIP_DIR at your own lwIP checkout (the main repo,
# which also carries contrib/ports/unix for the host arch headers):
#     make LWIP_DIR=/path/to/lwip
#
# `make check` (default) compiles the reference socket provider + event backend
# against your lwIP + the public Keel headers. A clean compile IS the validation:
# it proves Keel's public provider/event API (<keel/socket.h>, <keel/event.h>) is
# sufficient to author an lwIP platform using no internal Keel or host-POSIX
# types. Running a full server/client over lwIP means building libkeel with these
# two TUs as the platform's socket + event backend: the promotion path in
# docs/lwip_platform_design.md; see README.md.

LWIP_DIR ?= ../../../lwip
CC       ?= cc
CFLAGS    = -std=c11 -Wall -Wextra -Werror -O2 \
            -I../../include -I. \
            -I$(LWIP_DIR)/src/include \
            -I$(LWIP_DIR)/contrib/ports/unix/port/include

check: socket_lwip.o event_lwip.o
	@echo "OK: lwIP reference compiles against $(LWIP_DIR) (public API is sufficient)"

%.o: %.c lwipopts.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f socket_lwip.o event_lwip.o

.PHONY: check clean
