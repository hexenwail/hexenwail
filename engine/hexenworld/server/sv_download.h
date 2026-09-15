/* Shared with the focused regression in engine/tests/hw_download. */
#ifndef __HW_SV_DOWNLOAD_H
#define __HW_SV_DOWNLOAD_H

#include <stdint.h>

static inline int SV_DownloadPercent (int count, int size)
{
	return (int)((int64_t)count * 100 / size);
}

#endif /* __HW_SV_DOWNLOAD_H */
