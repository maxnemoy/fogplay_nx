#pragma once
#include <sys/types.h>
#include <sys/_iovec.h>
#define UIO_MAXIOV 1024
#ifndef ERESTART
#define ERESTART (-1)
#endif
