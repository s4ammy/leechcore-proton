#include "proton_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static int rejects(const char *connection)
{
    ProtonConfig config;
    char error[256];
    return proton_parse_connection(connection, &config, error, sizeof(error)) != 0;
}

int main(void)
{
    ProtonConfig config;
    char error[256];

    CHECK(proton_parse_connection("proton://pid=12345", &config,
                                  error, sizeof(error)) == 0);
    CHECK(config.pid == 12345);
    CHECK(config.read_only);
    CHECK(proton_parse_connection("proton://ro=0,pid=7", &config,
                                  error, sizeof(error)) == 0);
    CHECK(config.pid == 7);
    CHECK(!config.read_only);
    CHECK(proton_parse_connection("proton://pid=8;ro=1", &config,
                                  error, sizeof(error)) == 0);
    CHECK(config.read_only);

    CHECK(rejects("proton://"));
    CHECK(rejects("proton://ro=1"));
    CHECK(rejects("proton://pid=0"));
    CHECK(rejects("proton://pid=-1"));
    CHECK(rejects("proton://pid=999999999999999999999999"));
    CHECK(rejects("proton://pid=1,pid=2"));
    CHECK(rejects("proton://pid=1,ro=1,ro=0"));
    CHECK(rejects("proton://pid=1,readonly=1"));
    CHECK(rejects("proton://pid=1,ro=true"));
    CHECK(rejects("proton://pid=1,ro=2"));
    CHECK(rejects("proton://pid=1,"));
    CHECK(rejects("other://pid=1"));

    puts("test_config: PASS");
    return 0;
}
