/*
 * Cryptoflex specific operation for PKCS #15 initialization
 *
 * Copyright (C) 2002  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <sys/types.h>
#include <opensc/opensc.h>
#include <opensc/cardctl.h>
#include "pkcs15-init.h"
#include "profile.h"

static void	invert_buf(u8 *dest, const u8 *src, size_t c);

/*
 * Erase the card via rm -rf
 */
static int cflex_erase_card(struct sc_profile *profile, struct sc_card *card)
{
	return sc_pkcs15init_erase_card_recursively(card, profile, -1);
}

/*
 * Initialize the Application DF
 */
static int cflex_init_app(struct sc_profile *profile, struct sc_card *card,
		const u8 *pin, size_t pin_len, const u8 *puk, size_t puk_len)
{
	if (pin && pin_len) {
		profile->cbs->error("Cryptoflex card driver doesn't "
				"support SO PIN\n");
		return SC_ERROR_NOT_SUPPORTED;
	}

	/* Create the application DF */
	if (sc_pkcs15init_create_file(profile, card, profile->df_info->file))
		return 1;

	return 0;
}

/*
 * Update the contents of a PIN file
 */
static int cflex_update_pin(struct sc_profile *profile, struct sc_card *card,
		sc_file_t *file,
		const u8 *pin, size_t pin_len, int pin_tries,
		const u8 *puk, size_t puk_len, int puk_tries)
{
	u8		buffer[23], *p = buffer;
	int		r;
	size_t		len;

	memset(p, 0xFF, 3);
	p += 3;
	memset(p, profile->pin_pad_char, 8);
	strncpy((char *) p, (const char *) pin, pin_len);
	p += 8;
	*p++ = pin_tries;
	*p++ = pin_tries;
	memset(p, profile->pin_pad_char, 8);
	strncpy((char *) p, (const char *) puk, puk_len);
	p += 8;
	*p++ = puk_tries;
	*p++ = puk_tries;
	len = 23;

	r = sc_pkcs15init_update_file(profile, card, file, buffer, len);
	if (r < 0)
		return r;
	return 0;
}

/*
 * Store a PIN
 */
static int
cflex_new_pin(struct sc_profile *profile, struct sc_card *card,
		struct sc_pkcs15_pin_info *info, unsigned int index,
		const u8 *pin, size_t pin_len,
		const u8 *puk, size_t puk_len)
{
	sc_file_t *pinfile;
	struct sc_pkcs15_pin_info tmpinfo;
	char _template[30];
	int pin_tries, puk_tries;
	int r;

	index++;
	sprintf(_template, "pinfile-%d", index);
	/* Profile must define a "pinfile" for each PIN */
	if (sc_profile_get_file(profile, _template, &pinfile) < 0) {
		profile->cbs->error("Profile doesn't define \"%s\"", _template);
		return SC_ERROR_INVALID_ARGUMENTS;
	}
	info->path = pinfile->path;
	if (info->path.len > 2)
		info->path.len -= 2;
	info->reference = 1;
	if (pin_len > 8)
		pin_len = 8;
	if (puk_len > 8)
		puk_len = 8;
	sc_profile_get_pin_info(profile, SC_PKCS15INIT_USER_PIN, &tmpinfo);
	pin_tries = tmpinfo.tries_left;
	sc_profile_get_pin_info(profile, SC_PKCS15INIT_USER_PUK, &tmpinfo);
	puk_tries = tmpinfo.tries_left;
	
	r = cflex_update_pin(profile, card, pinfile, pin, pin_len, pin_tries,
				puk, puk_len, puk_tries);
	sc_file_free(pinfile);
	return r;
}

/*
 * Allocate a file
 */
static int
cflex_new_file(struct sc_profile *profile, struct sc_card *card,
		unsigned int type, unsigned int num,
		struct sc_file **out)
{
	struct sc_file	*file;
	char		name[64], *tag, *desc;

	desc = tag = NULL;
	while (1) {
		switch (type) {
		case SC_PKCS15_TYPE_PRKEY_RSA:
			desc = "RSA private key";
			tag = "private-key";
			break;
		case SC_PKCS15_TYPE_PUBKEY_RSA:
			desc = "RSA public key";
			tag = "public-key";
			break;
		case SC_PKCS15_TYPE_PUBKEY_DSA:
			desc = "DSA public key";
			tag = "public-key";
			break;
		case SC_PKCS15_TYPE_PRKEY:
			desc = "extractable private key";
			tag = "extractable-key";
			break;
		case SC_PKCS15_TYPE_CERT:
			desc = "certificate";
			tag = "certificate";
			break;
		case SC_PKCS15_TYPE_DATA_OBJECT:
			desc = "data object";
			tag = "data";
			break;
		}
		if (tag)
			break;
		/* If this is a specific type such as
		 * SC_PKCS15_TYPE_CERT_FOOBAR, fall back to
		 * the generic class (SC_PKCS15_TYPE_CERT)
		 */
		if (!(type & ~SC_PKCS15_TYPE_CLASS_MASK)) {
			profile->cbs->error("File type %X not supported by card driver", type);
			return SC_ERROR_INVALID_ARGUMENTS;
		}
		type &= SC_PKCS15_TYPE_CLASS_MASK;
	}

	snprintf(name, sizeof(name), "template-%s-%d", tag, num+1);
	if (sc_profile_get_file(profile, name, &file) < 0) {
		profile->cbs->error("Profile doesn't define %s template '%s'\n",
				desc, name);
		return SC_ERROR_NOT_SUPPORTED;
	}

	*out = file;
	return 0;
}

/*
 * Get the EF-pubkey corresponding to the EF-prkey
 */
int
cflex_pubkey_file(struct sc_file **ret, struct sc_file *prkf, unsigned int size)
{
	struct sc_file	*pukf;

	sc_file_dup(&pukf, prkf);
	sc_file_clear_acl_entries(pukf, SC_AC_OP_READ);
	sc_file_add_acl_entry(pukf, SC_AC_OP_READ, SC_AC_NONE, SC_AC_KEY_REF_NONE);
	pukf->path.len -= 2;
	sc_append_path_id(&pukf->path, (const u8 *) "\x10\x12", 2);
	pukf->id = 0x1012;
	pukf->size = size;

	*ret = pukf;
	return 0;
}

/*
 * RSA key generation
 */
static int
cflex_generate_key(struct sc_profile *profile, struct sc_card *card,
		unsigned int index, unsigned int keybits,
		sc_pkcs15_pubkey_t *pubkey,
		struct sc_pkcs15_prkey_info *info)
{
	struct sc_cardctl_cryptoflex_genkey_info args;
	struct sc_file	*prkf = NULL, *pukf = NULL;
	unsigned char	raw_pubkey[256];
	unsigned char	pinbuf[8];
	size_t		pinlen;
	int		r, delete_pukf = 0;

	if ((r = cflex_new_file(profile, card, SC_PKCS15_TYPE_PRKEY_RSA, index, &prkf)) < 0) 
	 	goto failed;

	switch (keybits) {
	case  512: prkf->size = 166; break;
	case  768: prkf->size = 246; break;
	case 1024: prkf->size = 326; break;
	case 2048: prkf->size = 646; break;
	default:
		profile->cbs->error("Unsupported key size %u\n", keybits);
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	if ((r = cflex_pubkey_file(&pukf, prkf, prkf->size + 3)) < 0)
		goto failed;

	/* Get the CHV1 PIN */
	pinlen = sizeof(pinbuf);
	memset(pinbuf, 0, sizeof(pinbuf));
	if ((r = sc_pkcs15init_get_secret(profile, card, SC_AC_CHV, 1, pinbuf, &pinlen)) < 0)
		goto failed;

	if ((r = sc_pkcs15init_create_file(profile, card, prkf)) < 0
	 || (r = sc_pkcs15init_create_file(profile, card, pukf)) < 0)
		goto failed;
	delete_pukf = 1;

	/* Present the PIN */
	if ((r = sc_select_file(card, &pukf->path, NULL))
	 || (r = sc_verify(card, SC_AC_CHV, 1, pinbuf, sizeof(pinbuf), NULL)) < 0)
		goto failed;

	memset(&args, 0, sizeof(args));
	args.exponent = 0x10001;
	args.key_bits = keybits;
	r = sc_card_ctl(card, SC_CARDCTL_CRYPTOFLEX_GENERATE_KEY, &args);
	if (r < 0)
		goto failed;

	/* extract public key */
	pubkey->algorithm = SC_ALGORITHM_RSA;
	pubkey->u.rsa.modulus.len   = keybits / 8;
	pubkey->u.rsa.modulus.data  = malloc(keybits / 8);
	pubkey->u.rsa.exponent.len  = 3;
	pubkey->u.rsa.exponent.data = malloc(3);
	memcpy(pubkey->u.rsa.exponent.data, "\x01\x00\x01", 3);
	if ((r = sc_select_file(card, &pukf->path, NULL)) < 0
	 || (r = sc_read_binary(card, 4, raw_pubkey, pubkey->u.rsa.modulus.len, 0)) < 0)
		goto failed;
	invert_buf(pubkey->u.rsa.modulus.data, raw_pubkey, pubkey->u.rsa.modulus.len);

	info->key_reference = 1;
	info->path = prkf->path;

failed:	if (delete_pukf)
		sc_pkcs15init_rmdir(card, profile, pukf);
	if (r < 0)
		sc_pkcs15init_rmdir(card, profile, prkf);
	sc_file_free(pukf);
	sc_file_free(prkf);
	return r;
}

static void invert_buf(u8 *dest, const u8 *src, size_t c)
{
        int i;

        for (i = 0; i < c; i++)
                dest[i] = src[c-1-i];
}

static int bn2cf(sc_pkcs15_bignum_t *num, u8 *buf)
{
	invert_buf(buf, num->data, num->len);
	return num->len;
}

static int cflex_encode_private_key(struct sc_pkcs15_prkey_rsa *rsa, u8 *key, size_t *keysize, int key_num)
{
        u8 buf[5 * 128 + 6], *p = buf;
	u8 bnbuf[256];
        int base = 0; 
        int r;
        
        switch (rsa->modulus.len) {
        case 512 / 8:
                base = 32;
                break;
        case 768 / 8:
                base = 48;
                break;
        case 1024 / 8:
                base = 64;
                break;
        case 2048 / 8:
                base = 128;
                break;
        }
        if (base == 0) {
                fprintf(stderr, "Key length invalid.\n");
                return 2;
        }
        *p++ = (5 * base + 3) >> 8;
        *p++ = (5 * base + 3) & 0xFF;
        *p++ = key_num;
        r = bn2cf(&rsa->p, bnbuf);
        if (r != base) {
                fprintf(stderr, "Invalid private key.\n");
                return 2;
        }
        memcpy(p, bnbuf, base);
        p += base;

        r = bn2cf(&rsa->q, bnbuf);
        if (r != base) {
                fprintf(stderr, "Invalid private key.\n");
                return 2;
        }
        memcpy(p, bnbuf, base);
        p += base;

        r = bn2cf(&rsa->iqmp, bnbuf);
        if (r != base) {
                fprintf(stderr, "Invalid private key.\n");
                return 2;
        }
        memcpy(p, bnbuf, base);
        p += base;

        r = bn2cf(&rsa->dmp1, bnbuf);
        if (r != base) {
                fprintf(stderr, "Invalid private key.\n");
                return 2;
        }
        memcpy(p, bnbuf, base);
        p += base;

        r = bn2cf(&rsa->dmq1, bnbuf);
        if (r != base) {
                fprintf(stderr, "Invalid private key.\n");
                return 2;
        }
        memcpy(p, bnbuf, base);
        p += base;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;
	
        memcpy(key, buf, p - buf);
        *keysize = p - buf;

        return 0;
}

static int cflex_encode_public_key(struct sc_pkcs15_prkey_rsa *rsa, u8 *key, size_t *keysize, int key_num)
{
        u8 buf[5 * 128 + 10], *p = buf;
        u8 bnbuf[256];
        int base = 0; 
        int r;
        
        switch (rsa->modulus.len) {
        case 512 / 8:
                base = 32;
                break;
        case 768 / 8:
                base = 48;
                break;
        case 1024 / 8:
                base = 64;
                break;
        case 2048 / 8:
                base = 128;
                break;
        }
        if (base == 0) {
                fprintf(stderr, "Key length invalid.\n");
                return 2;
        }
        *p++ = (5 * base + 7) >> 8;
        *p++ = (5 * base + 7) & 0xFF;
        *p++ = key_num;
        r = bn2cf(&rsa->modulus, bnbuf);
        if (r != 2*base) {
                fprintf(stderr, "Invalid public key.\n");
                return 2;
        }
        memcpy(p, bnbuf, 2*base);
        p += 2*base;

        memset(p, 0, base);
        p += base;

	memset(bnbuf, 0, 2*base);
        memcpy(p, bnbuf, 2*base);
        p += 2*base;
	r = bn2cf(&rsa->exponent, bnbuf);
	memcpy(p, bnbuf, 4);
        p += 4;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;

        memcpy(key, buf, p - buf);
        *keysize = p - buf;

        return 0;
}

/*
 * Store a private key
 */
static int
cflex_new_key(struct sc_profile *profile, struct sc_card *card,
		struct sc_pkcs15_prkey *key, unsigned int index,
		struct sc_pkcs15_prkey_info *info)
{
	u8 prv[1024], pub[1024];
	size_t prvsize, pubsize;
	struct sc_file *keyfile = NULL, *tmpfile = NULL;
	struct sc_pkcs15_prkey_rsa *rsa = NULL;
        int r;

	if (key->algorithm != SC_ALGORITHM_RSA) {
		profile->cbs->error("Cryptoflex supports only RSA keys.");
		return SC_ERROR_NOT_SUPPORTED;
	}
	rsa = &key->u.rsa;
	r = cflex_encode_private_key(rsa, prv, &prvsize, 1);
	if (r)
		goto err;
	r = cflex_encode_public_key(rsa, pub, &pubsize, 1);
	if (r)
		goto err;
	printf("Updating RSA private key...\n");
	r = cflex_new_file(profile, card, SC_PKCS15_TYPE_PRKEY_RSA, index,
			    &keyfile);
	if (r < 0)
		goto err;
	keyfile->size = prvsize;
	r = sc_pkcs15init_update_file(profile, card, keyfile, prv, prvsize);
	if (r < 0)
		goto err;
	info->path = keyfile->path;
	info->modulus_length = rsa->modulus.len << 3;

	if ((r = cflex_pubkey_file(&tmpfile, keyfile, pubsize)) < 0)
		goto err;

	printf("Updating RSA public key...\n");
	r = sc_pkcs15init_update_file(profile, card, tmpfile, pub, pubsize);
err:
	if (tmpfile)
		sc_file_free(tmpfile);
	return r;
}

struct sc_pkcs15init_operations sc_pkcs15init_cflex_operations = {
	cflex_erase_card,
	cflex_init_app,
	cflex_new_pin,
	cflex_new_key,
	cflex_new_file,
	cflex_generate_key,
};
