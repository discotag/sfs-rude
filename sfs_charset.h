#ifndef SFS_CHARSET_H
#define SFS_CHARSET_H
#include <SupportDefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SFS_CS_LATIN1, SFS_CS_CP1251, SFS_CS_ISO5, SFS_CS_UTF8 };
#ifndef SFS_CS_DEFAULT
#define SFS_CS_DEFAULT SFS_CS_LATIN1
#endif

int sfs_charset_parse(const char* args);
char* sfs_name_to_utf8(const uint8* s, size_t len, int cs);

#ifdef __cplusplus
}
#endif
#endif
