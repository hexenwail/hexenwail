/* Exercise the hwsv loose-file download loop past signed 32-bit overflow. */
#include "sv_download.h"

#include <limits.h>
#include <stdio.h>

#define TEST_DOWNLOAD_SIZE	(22 * 1024 * 1024 + 1)
#define DOWNLOAD_BLOCK_SIZE	1024

int main (void)
{
	unsigned char buffer[DOWNLOAD_BLOCK_SIZE];
	FILE *download;
	int count = 0;
	int previous = 0;
	int percent = 0;

	if (TEST_DOWNLOAD_SIZE <= INT_MAX / 100)
	{
		fprintf (stderr, "fixture does not cross the signed-int overflow boundary\n");
		return 1;
	}

	download = tmpfile ();
	if (!download || fseek (download, TEST_DOWNLOAD_SIZE - 1, SEEK_SET) != 0 ||
		fputc (0, download) == EOF || fseek (download, 0, SEEK_SET) != 0)
	{
		fprintf (stderr, "could not create the sparse loose-file fixture\n");
		if (download)
			fclose (download);
		return 1;
	}

	while (count < TEST_DOWNLOAD_SIZE)
	{
		int wanted = TEST_DOWNLOAD_SIZE - count;
		size_t got;

		if (wanted > DOWNLOAD_BLOCK_SIZE)
			wanted = DOWNLOAD_BLOCK_SIZE;
		got = fread (buffer, 1, (size_t)wanted, download);
		if (got != (size_t)wanted)
		{
			fprintf (stderr, "short read at byte %d\n", count);
			fclose (download);
			return 1;
		}

		count += (int)got;
		percent = SV_DownloadPercent (count, TEST_DOWNLOAD_SIZE);
		if (percent < previous || percent > 100 ||
			(count < TEST_DOWNLOAD_SIZE && percent == 100))
		{
			fprintf (stderr, "invalid progress %d at byte %d\n", percent, count);
			fclose (download);
			return 1;
		}
		previous = percent;
	}

	fclose (download);
	if (percent != 100)
	{
		fprintf (stderr, "final progress is %d, expected 100\n", percent);
		return 1;
	}

	printf ("%d-byte loose download completed at %d%%\n",
		TEST_DOWNLOAD_SIZE, percent);
	return 0;
}
