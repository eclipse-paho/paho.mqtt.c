#if defined(__cplusplus)
extern "C" {
#endif
#include "MQTTExportDeclarations.h"
#include <openssl/ssl.h>

LIBMQTT_API void SSLPluginInterface_setcallback(int (*callback)(SSL*));

#if defined(__cplusplus)
}
#endif