#include <keel/error.h>

static const char *kl_error_messages[] = {
    [KL_ERR_NONE]          = "no error",
    [KL_ERR_INVALID_ARG]   = "invalid argument",
    [KL_ERR_ALLOC]         = "memory allocation failed",
    [KL_ERR_OVERFLOW]      = "integer overflow",
    [KL_ERR_SOCKET]        = "socket creation failed",
    [KL_ERR_BIND]          = "bind failed",
    [KL_ERR_LISTEN]        = "listen failed",
    [KL_ERR_CONNECT]       = "connection failed",
    [KL_ERR_IO]            = "I/O error",
    [KL_ERR_TIMEOUT]       = "operation timed out",
    [KL_ERR_DNS]           = "DNS resolution failed",
    [KL_ERR_TLS_INIT]      = "TLS initialization failed",
    [KL_ERR_TLS_HANDSHAKE] = "TLS handshake failed",
    [KL_ERR_TLS_VTABLE]    = "TLS vtable incomplete",
    [KL_ERR_URL]           = "URL parse error",
    [KL_ERR_PARSE]         = "HTTP parse error",
    [KL_ERR_TOO_LARGE]     = "request or response too large",
    [KL_ERR_HEADER]        = "header injection detected",
    [KL_ERR_EVENT_INIT]    = "event loop init failed",
    [KL_ERR_EVENT_ADD]     = "event registration failed",
    [KL_ERR_QUEUE_FULL]    = "thread pool queue full",
    [KL_ERR_THREAD]        = "thread creation failed",
    [KL_ERR_PIPE]          = "pipe creation failed",
};

const char *kl_strerror(KlError err)
{
    if (err < 0 || err >= KL_ERR__COUNT)
        return "unknown error";
    return kl_error_messages[err];
}
