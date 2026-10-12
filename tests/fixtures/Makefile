MINGW_CC ?= x86_64-w64-mingw32-gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

.PHONY: all clean

all: proton_fixture.exe proton_fixture_access.so

proton_fixture.exe: proton_fixture.c
	$(MINGW_CC) $(CFLAGS) -o $@ $<

proton_fixture_access.so: proton_fixture_access.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -shared -pthread -o $@ $<

clean:
	rm -f proton_fixture.exe proton_fixture_access.so
