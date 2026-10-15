# Desktop build of the WhatsApp6 core (Linux/macOS), for the offline self-tests and the pairing CLI.
# mbedTLS sources are expected under ../vendor/mbedtls (fetched by CI or by hand), configured with
# ../vendor/mbedtls_config_host.h. Monocypher is vendored in-tree.
#
#   make selftest   -> ./wa_selftest   (no network; run in CI)
#   make cli        -> ./wa_pair_cli    (live pairing; run it yourself, not in CI)

ROOT := ..
CORE := $(ROOT)/core
VENDOR := $(ROOT)/vendor
MBEDTLS := $(VENDOR)/mbedtls

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS += -I$(CORE) -I$(CORE)/rtc -I$(VENDOR)/monocypher -I$(MBEDTLS)/include -I$(MBEDTLS)/library
CFLAGS += -DMBEDTLS_USER_CONFIG_FILE='"mbedtls_config_host.h"' -I$(VENDOR)
LDLIBS := -lz -lpthread

# The live relay transport (wa_rtc + core/rtc gf_*) is iOS-only (arc4random, VideoToolbox, device sockets); the
# offline self-tests don't exercise it, so it's excluded from the desktop build.
CORE_SRC := $(filter-out $(CORE)/wa_rtc.c, $(wildcard $(CORE)/*.c)) $(VENDOR)/monocypher/monocypher.c
MBEDTLS_SRC := $(wildcard $(MBEDTLS)/library/*.c)

CORE_OBJ := $(CORE_SRC:.c=.o)
MBEDTLS_OBJ := $(MBEDTLS_SRC:.c=.o)

all: selftest cli

selftest: wa_selftest.o $(CORE_OBJ) $(MBEDTLS_OBJ)
	$(CC) $(CFLAGS) -o wa_selftest $^ $(LDLIBS)

cli: wa_pair_cli.o $(CORE_OBJ) $(MBEDTLS_OBJ)
	$(CC) $(CFLAGS) -o wa_pair_cli wa_pair_cli.o $(CORE_OBJ) $(MBEDTLS_OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f wa_selftest wa_pair_cli *.o $(CORE_OBJ) $(MBEDTLS_OBJ)

.PHONY: all selftest cli clean
