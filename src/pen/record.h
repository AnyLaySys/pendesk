#ifndef PENDESK_RECORD_H
#define PENDESK_RECORD_H

#include <stddef.h>

int recording_file(const char *subdir, const char *extension, char *path, size_t size);

#endif