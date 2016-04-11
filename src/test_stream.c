#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "stream.h"

#define TEST_URL "https://ftp.gnu.org/gnu/aspell/aspell-0.50.5.tar.gz"

static const unsigned char const sig[] =
{
	0x88, 0x3f, 0x03, 0x05, 0x00, 0x40, 0x29, 0x9c, 0x79, 0xb6, 0xd9, 0xd0,
	0xcc, 0x38, 0xb3, 0x27, 0xd7, 0x11, 0x02, 0xf1, 0xca, 0x00, 0x9c, 0x0a,
	0xb4, 0x86, 0xb4, 0x14, 0x56, 0x7c, 0x65, 0x46, 0xef, 0xb7, 0x7a, 0xef,
	0xde, 0xe4, 0x53, 0xbd, 0x1b, 0x89, 0xca, 0x00, 0x9e, 0x2d, 0xf8, 0xa3,
	0x2e, 0x25, 0x2a, 0x97, 0x3f, 0x76, 0x0f, 0x1d, 0x29, 0x8c, 0x50, 0x1a,
	0xe3, 0xb7, 0x6d, 0xe3, 0x4a
};

static const unsigned int sig_len = 65;
/* static unsigned char buf[1024 * 1024 * 2]; */


int
main()
{
	/*
	 * Right now this test is pretty useless, it only
	 * downloads a file. We need to check the signatures and
	 * test the seek functionality, as well as concurrent reads
	 * from multiple threads
	 */
	FILE *f;
	char buf[1024];

	Stream_Init();

	f = fopen("/tmp/avmount-test.txt", "w");
	if (f == NULL) {
		fprintf(stderr, "Could not open tmp file.");
		exit(1);
	}

	Stream *s = Stream_Open(TEST_URL);
	if (s == NULL) {
		fprintf(stderr, "Could not open %s\n", TEST_URL);
		return 1;
	}
	printf("Stream " TEST_URL " opened successfully.\n");

	printf("Reading stream...");
	fflush(stdout);
	while (Stream_Read(s, buf, 1024) > 0) {
		fwrite(buf, 1024, 1, f);
	}
	printf("\n");

	Stream_Close(s);
	printf("Stream closed successfully.\n");

	fclose(f);
	unlink("/tmp/avmount-test.txt");

	return 0;
}
