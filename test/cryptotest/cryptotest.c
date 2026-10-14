/*
 * cryptotest: exercise /dev/crypto on NetBSD/esp32s31 (espcrypto(4)):
 * SHA-1, HMAC-SHA-256/512 and a 2048-bit modular exponentiation.
 * Built by test/cryptotest/build.sh; fetched and run by boot-test.py.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/ioctl.h>
#include <crypto/cryptodev.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vectors.h"

static int fails;

static void
check(const char *what, const void *a, const void *b, size_t n)
{
	int ok = memcmp(a, b, n) == 0;

	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	fails += !ok;
}

static void
hash(int fd, int alg, const void *key, size_t keylen, const void *msg,
    size_t len, uint8_t *out)
{
	struct session_op so;
	struct crypt_op co;
	char buf[2048];

	memset(&so, 0, sizeof(so));
	so.mac = alg;
	so.mackey = __UNCONST(key);
	so.mackeylen = keylen;	/* must be the algorithm's key size */
	if (ioctl(fd, CIOCGSESSION, &so) == -1)
		err(1, "CIOCGSESSION %d", alg);
	memset(&co, 0, sizeof(co));
	co.ses = so.ses;
	co.op = COP_ENCRYPT;
	co.len = len;
	if (co.len > sizeof(buf))
		errx(1, "message too long");
	memcpy(buf, msg, co.len);	/* dst is written: not .rodata */
	co.src = co.dst = buf;
	memset(out, 0, 64);
	co.mac = out;
	if (ioctl(fd, CIOCCRYPT, &co) == -1)
		err(1, "CIOCCRYPT %d", alg);
	ioctl(fd, CIOCFSESSION, &so.ses);
}

int
main(void)
{
	static const char fox[] =
	    "The quick brown fox jumps over the lazy dog";
	static const uint8_t sha1_abc[20] = {
	    0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
	    0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d };
	static const uint8_t sha1_hmac_hi[20] = {	/* RFC 2202 1 */
	    0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
	    0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00 };
	uint8_t k1[20], k[64], msg[1000];
	uint8_t out[64], z[sizeof(rsa_m)];
	struct crypt_kop kop;
	int fd, cfd;

	if ((fd = open("/dev/crypto", O_RDWR)) == -1)
		err(1, "/dev/crypto");
	if (ioctl(fd, CRIOGET, &cfd) == -1)
		err(1, "CRIOGET");

	for (size_t i = 0; i < sizeof(k1); i++)
		k1[i] = 0x0b;
	for (size_t i = 0; i < sizeof(k); i++)
		k[i] = i;
	for (size_t i = 0; i < sizeof(msg); i++)
		msg[i] = i * 7 + 3;

	hash(cfd, CRYPTO_SHA1, NULL, 0, "abc", 3, out);
	check("sha1", out, sha1_abc, sizeof(sha1_abc));
	hash(cfd, CRYPTO_SHA1_HMAC, k1, sizeof(k1), "Hi There", 8, out);
	check("hmac-sha1", out, sha1_hmac_hi, sizeof(sha1_hmac_hi));
	/* HMAC-SHA-256 is CRYPTO_SHA2_HMAC with a 32-byte key */
	hash(cfd, CRYPTO_SHA2_HMAC, k, 32, fox, strlen(fox), out);
	/* opencrypto truncates SHA-2 HMACs to half (RFC 4868) */
	check("hmac-sha256", out, hmac256_fox, 16);
	hash(cfd, CRYPTO_SHA2_512_HMAC, k, 64, msg, sizeof(msg), out);
	check("hmac-sha512", out, hmac512_long, 32);

	memset(&kop, 0, sizeof(kop));
	kop.crk_op = CRK_MOD_EXP;
	kop.crk_iparams = 3;
	kop.crk_oparams = 1;
	kop.crk_param[0].crp_p = __UNCONST(rsa_x);
	kop.crk_param[0].crp_nbits = sizeof(rsa_x) * 8;
	kop.crk_param[1].crp_p = __UNCONST(rsa_y);
	kop.crk_param[1].crp_nbits = sizeof(rsa_y) * 8;
	kop.crk_param[2].crp_p = __UNCONST(rsa_m);
	kop.crk_param[2].crp_nbits = sizeof(rsa_m) * 8;
	kop.crk_param[3].crp_p = z;
	kop.crk_param[3].crp_nbits = sizeof(z) * 8;
	if (ioctl(cfd, CIOCKEY, &kop) == -1)
		err(1, "CIOCKEY");
	if (kop.crk_status != 0)
		errx(1, "CIOCKEY: status %u", kop.crk_status);
	check("modexp-2048", z, rsa_z, sizeof(z));

	printf("cryptotest: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
