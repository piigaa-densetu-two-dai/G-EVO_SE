/*
 * 使い方
 *
 * dmgplay [-l count] comport file
 *
 * 例: dmgplay -l 3 com1 song.vgz
 *
 * comportはゲーミングボーイのCOMポート、fileにはVGM又はVGZファイルを指定します。
 * 無論、ゲームボーイ向けのファイルのみに対応です。COMポート名はデバイスマネージャーで確認して下さい。
 * -lオプションはループ再生パートの再生回数です。0以上の整数を指定して下さい。
 * -lオプション未指定の場合は1回の再生です。値に0を指定した場合は無限ループ再生となります
 * ループ再生パートの無いファイルの場合はこのオプションは無視されます。
 *
 * ゲーミングボーイの通信プロトコル
 *
 * COMポートに以下の順にデータを書き込むとゲームボーイ上の指定したアドレスに指定した値が書き込まれます。
 *
 * 0xb3, address, data (3バイト、何れも8ビット値)
 *
 * address 0x00はゲームボーイ上のアドレス0xff10に相当します。
 * 要はVGMファイルのGameBoy DMGコマンド(0xb3)をそのままCOMポートに書き込めばいいんだという。
 *
 * COMポートプログラミングのヒント
 *
 * 通信はUSBで完結しているのでCOMポートの通信速度等の設定は意味を持ちません。デフォルト設定のままで大丈夫です。
 *
 * その他
 *
 * 高精度タイマーを使用して高精度再生しますがCPU負荷高いです💦。
 * このソースコードはwindows上のmingw(gcc)でコンパイルできます。
 * gzipデータの伸張にuzlibを使用しています。
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <wchar.h>
#include <signal.h>
#include <windows.h>

#include "uzlib.h"

static int port;
static uint64_t freq, start;

static int uncompress(const uint8_t *src, size_t srclen, uint8_t **dst, size_t *dstlen)
{
/* produce decompressed output in chunks of this size */
/* default is to decompress byte by byte; can be any other length */
#define OUT_CHUNK_SIZE 1
	size_t dlen;
	int res;

	uzlib_init();

	/* -- get decompressed length -- */
	dlen =            src[srclen - 1];
	dlen = 256*dlen + src[srclen - 2];
	dlen = 256*dlen + src[srclen - 3];
	dlen = 256*dlen + src[srclen - 4];

	*dstlen = dlen;

	/* there can be mismatch between length in the trailer and actual
	   data stream; to avoid buffer overruns on overlong streams, reserve
	   one extra byte */
	dlen++;

	*dst = malloc(dlen);
	if (*dst == NULL) {
		return 1;
	}

	/* -- decompress data -- */
	struct uzlib_uncomp d;
	uzlib_uncompress_init(&d, NULL, 0);

	/* all 3 fields below must be initialized by user */
	d.source = src;
	d.source_limit = src + srclen - 4;
	d.source_read_cb = NULL;

	res = uzlib_gzip_parse_header(&d);
	if (res != TINF_OK) {
		printf("Error parsing header: %d\n", res);
		return 1;
	}

	d.dest_start = d.dest = *dst;

	while (dlen) {
		unsigned int chunk_len = dlen < OUT_CHUNK_SIZE ? dlen : OUT_CHUNK_SIZE;
		d.dest_limit = d.dest + chunk_len;
		res = uzlib_uncompress_chksum(&d);
		dlen -= chunk_len;
		if (res != TINF_OK) {
			break;
		}
	}

	if (res != TINF_DONE) {
		printf("Error during decompression: %d\n", res);
		return 1;
	}

	return 0;
}

static void wait_until(uint64_t time)
{
	uint64_t cnt;

	while (1) {
		QueryPerformanceCounter((LARGE_INTEGER *)&cnt);
		if ((uint64_t)(cnt - start) >= (freq * time / 44100)) {
			break;
		}
	}
}

static void playvgm(uint8_t *data, uint32_t len)
{
	uint32_t pos;
	static uint64_t time = 0;

	for (pos = 0; pos < len; pos++) {
		switch (data[pos]) {
			case 0xb3:
				write(port, &data[pos], 3);
				pos += 2;
				break;
			case 0x61:
				time += *(uint16_t *)&data[pos + 1]; /* アライメントされていないのでx86以外の場合はここ注意 */
				pos += 2;
				wait_until(time);
				break;
			case 0x62:
				time += 735;
				wait_until(time);
				break;
			case 0x63:
				time += 882;
				wait_until(time);
				break;
			case 0x66: /* end */
				return;
			case 0x70:
			case 0x71:
			case 0x72:
			case 0x73:
			case 0x74:
			case 0x75:
			case 0x76:
			case 0x77:
			case 0x78:
			case 0x79:
			case 0x7a:
			case 0x7b:
			case 0x7c:
			case 0x7d:
			case 0x7e:
			case 0x7f:
				time += ((data[pos] & 0x0f) + 1);
				wait_until(time);
				break;
			default:
				break;

		}
	}
}

static void stopvgm(int signum)
{
	uint8_t data[] = { 0xb3, 0x16, 0x00 };

	write(port, data, sizeof(data));
	close(port);

	exit(0);
}

static void usage(char *name)
{
	fprintf(stderr, "usage: %s [-l count] comport file", name);
	exit(1);
}

int main(int argc, char *argv[])
{
	int opt;
	int loop = 1;
	char *ptr;
	char pname[16];
	int file;
	struct stat st;
	uint8_t *vgmdat;
	uint8_t *buf;
	size_t buflen;
	uint32_t doffset;
	uint32_t loffset;
	uint32_t eoffset;
	uint32_t goffset;

	while ((opt = getopt(argc, argv, "l:")) != -1) {
		switch (opt) {
			case 'l':
				loop = strtol(optarg, &ptr, 10);
				if (*ptr || (loop < 0)) {
					usage(argv[0]);
				}
				break;
			default:
				usage(argv[0]);
		}
	}
	if ((argc - optind) != 2) {
		usage(argv[0]);
	}

	/* シリアルポートを開く */
	snprintf(pname, sizeof(pname), "\\\\.\\%s", argv[optind]);
	if ((port = open(pname, O_WRONLY | O_BINARY)) < 0) {
		perror(argv[optind]);
		return 1;
	}

	/* ファイルを読み込む */
	if ((file = open(argv[optind + 1], O_RDONLY | O_BINARY)) < 0) {
		perror(argv[optind + 1]);
		return 1;
	}
	if (fstat(file, &st) < 0) {
		perror("fstat()");
		return 1;
	}
	if (!(vgmdat = malloc(st.st_size))) {
		perror("malloc()");
		return 1;
	}
	if (read(file, vgmdat, st.st_size) != st.st_size) {
		perror("read()");
		return 1;
	}
	close(file);

	/* データのチェック */
	if ((vgmdat[0] == 0x1f) && (vgmdat[1] == 0x8b)) { /* gzip圧縮データなら伸張 */
		if (st.st_size < 6) {
			fprintf(stderr, "invalid file size\n");
			return 1;
		}
		if (uncompress(vgmdat, st.st_size, &buf, &buflen)) {
			fprintf(stderr, "uncompress error\n");
			return 1;
		}
		free(vgmdat);
		vgmdat = buf;
		st.st_size = buflen;
	}
	if (st.st_size < 256) {
		fprintf(stderr, "invalid file size\n");
		return 1;
	}
	if (memcmp(vgmdat, "Vgm", 3)) { /* VGMデータか */
		fprintf(stderr, "this isn't a VGM file\n");
		return 1;
	}
	if ((*(uint32_t *)&vgmdat[0x08] < 0x0161) || (*(uint32_t *)&vgmdat[0x80] == 0)) { /* v1.61以上のGameBoyデータか */
		fprintf(stderr, "this isn't a GameBoy DMG file\n");
		return 1;
	}

	/* オフセット情報の取得 */
	eoffset = *(uint32_t *)&vgmdat[0x04] + 0x04; /* EOF offset */
	if (st.st_size != eoffset) {
		fprintf(stderr, "invalid file size\n");
		return 1;
	}
	loffset = *(uint32_t *)&vgmdat[0x1c] + 0x1c; /* loop offset */
	doffset = *(uint32_t *)&vgmdat[0x34] + 0x34; /* VGM data offset */
	goffset = *(uint32_t *)&vgmdat[0x14] + 0x14; /* GD3 offset */

	/* GD3タグ情報の表示 */
	if (0x14 < goffset) {
		if (memcmp("Gd3", &vgmdat[goffset], 3) == 0) { /* GD3タグか */
			wchar_t *gd3tag[11] = { NULL };
			uint8_t cnt = 0;
			uint32_t pos = goffset + 12;
			/* アライメントされていないのでx86以外の場合は以下注意 */
			gd3tag[cnt] = (wchar_t *)&vgmdat[pos];
			for ( ; (pos < eoffset) && (cnt < 11); pos += 2) {
				if (*(wchar_t *)&vgmdat[pos] == 0) {
					gd3tag[++cnt] = (wchar_t *)&vgmdat[pos + 2];
				}
			}
			if (gd3tag[2]) {
				wprintf(L"Game  : %ls", gd3tag[2]);
				if (gd3tag[8]) {
					wprintf(L" [%ls]", gd3tag[8]);
				}
				wprintf(L"\n");
			}
			if (gd3tag[0]) {
				wprintf(L"Track : %ls\n", gd3tag[0]);
			}
			if (gd3tag[4]) {
				wprintf(L"System: %ls\n", gd3tag[4]);
			}
			if (gd3tag[6]) {
				wprintf(L"Artist: %ls\n", gd3tag[6]);
			}
			if (gd3tag[9]) {
				wprintf(L"Dumper: %ls\n", gd3tag[9]);
			}
		}
	}

	/* 再生準備 */
	signal(SIGINT, stopvgm);
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
	QueryPerformanceCounter((LARGE_INTEGER *)&start);

	/* 再生 */
	playvgm(&vgmdat[doffset], goffset - doffset);
	for (int i = 0; (0x1c < loffset) && (!loop || (i < loop)); i++) {
		playvgm(&vgmdat[loffset], goffset - loffset);
	}
	stopvgm(0);

	return 0;
}
