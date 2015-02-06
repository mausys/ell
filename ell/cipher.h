/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2015  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __ELL_CIPHER_H
#define __ELL_CIPHER_H

#ifdef __cplusplus
extern "C" {
#endif

struct l_cipher;

enum l_cipher_type {
	L_CIPHER_AES,
	L_CIPHER_ARC4,
};

struct l_cipher *l_cipher_new(enum l_cipher_type type,
				const void *key, size_t key_length);

void l_cipher_free(struct l_cipher *cipher);

void l_cipher_encrypt(struct l_cipher *cipher,
			const void *in, void *out, size_t len);

void l_cipher_decrypt(struct l_cipher *cipher,
			const void *in, void *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __ELL_CIPHER_H */