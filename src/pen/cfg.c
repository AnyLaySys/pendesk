#include "cfg.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
    char *end;
    while (isspace((unsigned char) *text)) ++text;
    end = text + strlen(text);
    while (end != text && isspace((unsigned char) end[-1])) --end;
    *end = '\0';
    return text;
}

static int port(const char *text, uint16_t *value) {
    char *end;
    unsigned long parsed = strtoul(text, &end, 10);
    if (*text == '\0' || *end != '\0' || !parsed || parsed > UINT16_MAX) return -1;
    *value = (uint16_t) parsed;
    return 0;
}

static int endpoint(char *host, size_t size, uint16_t *value, const char *text) {
    const char *separator = strrchr(text, ':');
    size_t length;
    if (!separator || separator == text) return -1;
    length = (size_t)(separator - text);
    if (length >= size || port(separator + 1, value) != 0) return -1;
    memcpy(host, text, length);
    host[length] = '\0';
    return 0;
}

static int nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int token(uint8_t output[32], const char *text) {
    if (strlen(text) != 64) return -1;
    for (size_t index = 0; index != 32; ++index) {
        int high = nibble(text[index * 2]);
        int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return -1;
        output[index] = (uint8_t)(high << 4 | low);
    }
    return 0;
}

int cfg_load(const char *path, struct cfg *cfg) {
    bool host = false;
    bool port_set = false;
    bool socks = false;
    bool token_set = false;
    char line[1024];
    struct in_addr address;
    struct in_addr proxy;
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        char *equal;
        char *key;
        char *value;
        if (!strchr(line, '\n') && !feof(file)) goto fail;
        equal = strchr(line, '=');
        if (!equal) continue;
        *equal = '\0';
        key = trim(line);
        value = trim(equal + 1);
        if (!*key || !*value) goto fail;
        if (!strcmp(key, "host")) {
            if (strlen(value) >= sizeof(cfg->host)) goto fail;
            strcpy(cfg->host, value);
            host = true;
        } else if (!strcmp(key, "port")) {
            if (port(value, &cfg->port) != 0) goto fail;
            port_set = true;
        } else if (!strcmp(key, "socks")) {
            if (endpoint(cfg->socks_host, sizeof(cfg->socks_host), &cfg->socks_port, value) != 0)
                goto fail;
            socks = true;
        } else if (!strcmp(key, "token")) {
            if (token(cfg->token, value) != 0) goto fail;
            token_set = true;
        } else goto fail;
    }
    fclose(file);
    return host && port_set && socks && token_set && inet_pton(AF_INET, cfg->host, &address) == 1 &&
           (ntohl(address.s_addr) & UINT32_C(0xffc00000)) == UINT32_C(0x64400000) &&
           inet_pton(AF_INET, cfg->socks_host, &proxy) == 1 && (ntohl(proxy.s_addr) >> 24) == 127
           ? 0 : -1;
    fail:
    fclose(file);
    return -1;
}

int cfg_control_path(char *path, size_t size, const char *config) {
    char package[512];
    char *separator;
    int length = snprintf(package, sizeof(package), "%s", config);
    if (length < 0 || (size_t) length >= sizeof(package) || !(separator = strrchr(package, '/')))
        return -1;
    *separator = '\0';
    if (!(separator = strrchr(package, '/'))) return -1;
    *separator = '\0';
    length = snprintf(path, size, "%s/data/input.sock", package);
    return length >= 0 && (size_t) length < size ? 0 : -1;
}