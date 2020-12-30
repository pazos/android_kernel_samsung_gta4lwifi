// SPDX-License-Identifier: GPL-2.0
/*
 * key management facility for FS encryption support.
 *
 * Copyright (C) 2015, Google, Inc.
 *
 * This contains encryption key functions.
 *
 * Written by Michael Halcrow, Ildar Muslukhov, and Uday Savagaonkar, 2015.
 */

#include <keys/user-type.h>
#include <linux/hashtable.h>
#include <linux/scatterlist.h>
#include <linux/ratelimit.h>
#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/sha.h>
#include <crypto/skcipher.h>
#include "fscrypt_private.h"
#include "fscrypt_ice.h"

#ifdef CONFIG_FSCRYPT_SDP
static int derive_fek(struct inode *inode,
		const struct fscrypt_context *ctx,
		struct fscrypt_info *crypt_info,
		u8 *fek, u32 fek_len);
#endif

static struct crypto_shash *essiv_hash_tfm;

/* Table of keys referenced by FS_POLICY_FLAG_DIRECT_KEY policies */
static DEFINE_HASHTABLE(fscrypt_master_keys, 6); /* 6 bits = 64 buckets */
static DEFINE_SPINLOCK(fscrypt_master_keys_lock);

/*
 * Key derivation function.  This generates the derived key by encrypting the
 * master key with AES-128-ECB using the inode's nonce as the AES key.
 *
 * The master key must be at least as long as the derived key.  If the master
 * key is longer, then only the first 'derived_keysize' bytes are used.
 */
static int derive_key_aes(const u8 *master_key,
			  const struct fscrypt_context *ctx,
			  u8 *derived_key, unsigned int derived_keysize)
{
	int res = 0;
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist src_sg, dst_sg;
	struct crypto_skcipher *tfm = crypto_alloc_skcipher("ecb(aes)", 0, 0);

	if (IS_ERR(tfm)) {
		res = PTR_ERR(tfm);
		tfm = NULL;
		goto out;
	}
	crypto_skcipher_set_flags(tfm, CRYPTO_TFM_REQ_WEAK_KEY);
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req) {
		res = -ENOMEM;
		goto out;
	}
	skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			crypto_req_done, &wait);
	res = crypto_skcipher_setkey(tfm, ctx->nonce, sizeof(ctx->nonce));
	if (res < 0)
		goto out;

	sg_init_one(&src_sg, master_key, derived_keysize);
	sg_init_one(&dst_sg, derived_key, derived_keysize);
	skcipher_request_set_crypt(req, &src_sg, &dst_sg, derived_keysize,
				   NULL);
	res = crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
out:
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	return res;
}

/*
 * Search the current task's subscribed keyrings for a "logon" key with
 * description prefix:descriptor, and if found acquire a read lock on it and
 * return a pointer to its validated payload in *payload_ret.
 */
static struct key *
find_and_lock_process_key(const char *prefix,
			  const u8 descriptor[FS_KEY_DESCRIPTOR_SIZE],
			  unsigned int min_keysize,
			  const struct fscrypt_key **payload_ret)
{
	char *description;
	struct key *key;
	const struct user_key_payload *ukp;
	const struct fscrypt_key *payload;

	description = kasprintf(GFP_NOFS, "%s%*phN", prefix,
				FS_KEY_DESCRIPTOR_SIZE, descriptor);
	if (!description)
		return ERR_PTR(-ENOMEM);

	key = request_key(&key_type_logon, description, NULL);
	kfree(description);
	if (IS_ERR(key))
		return key;

	down_read(&key->sem);
	ukp = user_key_payload_locked(key);

	if (!ukp) /* was the key revoked before we acquired its semaphore? */
		goto invalid;

	payload = (const struct fscrypt_key *)ukp->data;

	if (ukp->datalen != sizeof(struct fscrypt_key) ||
	    payload->size < 1 || payload->size > FS_MAX_KEY_SIZE) {
		fscrypt_warn(NULL,
			     "key with description '%s' has invalid payload",
			     key->description);
		goto invalid;
	}

	if (payload->size < min_keysize) {
		fscrypt_warn(NULL,
			     "key with description '%s' is too short (got %u bytes, need %u+ bytes)",
			     key->description, payload->size, min_keysize);
		goto invalid;
	}

	*payload_ret = payload;
	return key;

invalid:
	up_read(&key->sem);
	key_put(key);
	return ERR_PTR(-ENOKEY);
}

static struct fscrypt_mode available_modes[] = {
	[FS_ENCRYPTION_MODE_AES_256_XTS] = {
		.friendly_name = "AES-256-XTS",
		.cipher_str = "xts(aes)",
		.keysize = 64,
		.ivsize = 16,
	},
	[FS_ENCRYPTION_MODE_AES_256_CTS] = {
		.friendly_name = "AES-256-CTS-CBC",
		.cipher_str = "cts(cbc(aes))",
		.keysize = 32,
		.ivsize = 16,
	},
	[FS_ENCRYPTION_MODE_AES_128_CBC] = {
		.friendly_name = "AES-128-CBC",
		.cipher_str = "cbc(aes)",
		.keysize = 16,
		.ivsize = 16,
		.needs_essiv = true,
	},
	[FS_ENCRYPTION_MODE_AES_128_CTS] = {
		.friendly_name = "AES-128-CTS-CBC",
		.cipher_str = "cts(cbc(aes))",
		.keysize = 16,
		.ivsize = 16,
	},
	[FS_ENCRYPTION_MODE_ADIANTUM] = {
		.friendly_name = "Adiantum",
		.cipher_str = "adiantum(xchacha12,aes)",
		.keysize = 32,
		.ivsize = 32,
	},
	[FS_ENCRYPTION_MODE_PRIVATE] = {
		.friendly_name = "ice",
		.cipher_str = "xts(aes)",
		.keysize = 64,
		.ivsize = 16,
		.inline_encryption = true,
	},
};

static struct fscrypt_mode *
select_encryption_mode(const struct fscrypt_info *ci, const struct inode *inode)
{
	struct fscrypt_mode *mode = NULL;

	if (!fscrypt_valid_enc_modes(ci->ci_data_mode, ci->ci_filename_mode)) {
		fscrypt_warn(inode->i_sb,
			     "inode %lu uses unsupported encryption modes (contents mode %d, filenames mode %d)",
			     inode->i_ino, ci->ci_data_mode,
			     ci->ci_filename_mode);
		return ERR_PTR(-EINVAL);
	}

	if (S_ISREG(inode->i_mode)) {
		mode = &available_modes[ci->ci_data_mode];
		if (IS_ERR(mode)) {
			fscrypt_warn(inode->i_sb, "Invalid mode");
			return ERR_PTR(-EINVAL);
		}
		if (mode->inline_encryption &&
				!fscrypt_is_ice_capable(inode->i_sb)) {
			fscrypt_warn(inode->i_sb, "ICE support not available");
			return ERR_PTR(-EINVAL);
		}
		return mode;
	}

	if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode))
		return &available_modes[ci->ci_filename_mode];

	WARN_ONCE(1, "fscrypt: filesystem tried to load encryption info for inode %lu, which is not encryptable (file type %d)\n",
		  inode->i_ino, (inode->i_mode & S_IFMT));
	return ERR_PTR(-EINVAL);
}

/* Find the master key, then derive the inode's actual encryption key */
static int find_and_derive_key(const struct inode *inode,
			       const struct fscrypt_context *ctx,
			       u8 *derived_key, const struct fscrypt_mode *mode)
{
	struct key *key;
	const struct fscrypt_key *payload;
	int err;

	key = find_and_lock_process_key(FS_KEY_DESC_PREFIX,
					ctx->master_key_descriptor,
					mode->keysize, &payload);
	if (key == ERR_PTR(-ENOKEY) && inode->i_sb->s_cop->key_prefix) {
		key = find_and_lock_process_key(inode->i_sb->s_cop->key_prefix,
						ctx->master_key_descriptor,
						mode->keysize, &payload);
	}
	if (IS_ERR(key))
		return PTR_ERR(key);

	if (ctx->flags & FS_POLICY_FLAG_DIRECT_KEY) {
		if (mode->ivsize < offsetofend(union fscrypt_iv, nonce)) {
			fscrypt_warn(inode->i_sb,
				     "direct key mode not allowed with %s",
				     mode->friendly_name);
			err = -EINVAL;
		} else if (ctx->contents_encryption_mode !=
			   ctx->filenames_encryption_mode) {
			fscrypt_warn(inode->i_sb,
				     "direct key mode not allowed with different contents and filenames modes");
			err = -EINVAL;
		} else {
			memcpy(derived_key, payload->raw, mode->keysize);
			err = 0;
		}
	} else if (mode->inline_encryption) {
		memcpy(derived_key, payload->raw, mode->keysize);
		err = 0;
	} else {
		err = derive_key_aes(payload->raw, ctx, derived_key,
				     mode->keysize);
	}
	up_read(&key->sem);
	key_put(key);
	return err;
}

/* Allocate and key a symmetric cipher object for the given encryption mode */
static struct crypto_skcipher *
allocate_skcipher_for_mode(struct fscrypt_mode *mode, const u8 *raw_key,
			   const struct inode *inode)
{
	struct crypto_skcipher *tfm;
	int err;

	tfm = crypto_alloc_skcipher(mode->cipher_str, 0, 0);
	if (IS_ERR(tfm)) {
		fscrypt_warn(inode->i_sb,
			     "error allocating '%s' transform for inode %lu: %ld",
			     mode->cipher_str, inode->i_ino, PTR_ERR(tfm));
		return tfm;
	}
	if (unlikely(!mode->logged_impl_name)) {
		/*
		 * fscrypt performance can vary greatly depending on which
		 * crypto algorithm implementation is used.  Help people debug
		 * performance problems by logging the ->cra_driver_name the
		 * first time a mode is used.  Note that multiple threads can
		 * race here, but it doesn't really matter.
		 */
		mode->logged_impl_name = true;
		pr_info("fscrypt: %s using implementation \"%s\"\n",
			mode->friendly_name,
			crypto_skcipher_alg(tfm)->base.cra_driver_name);
	}
	crypto_skcipher_set_flags(tfm, CRYPTO_TFM_REQ_WEAK_KEY);
	err = crypto_skcipher_setkey(tfm, raw_key, mode->keysize);
	if (err)
		goto err_free_tfm;

	return tfm;

err_free_tfm:
	crypto_free_skcipher(tfm);
	return ERR_PTR(err);
}

/* Master key referenced by FS_POLICY_FLAG_DIRECT_KEY policy */
struct fscrypt_master_key {
	struct hlist_node mk_node;
	refcount_t mk_refcount;
	const struct fscrypt_mode *mk_mode;
	struct crypto_skcipher *mk_ctfm;
	u8 mk_descriptor[FS_KEY_DESCRIPTOR_SIZE];
	u8 mk_raw[FS_MAX_KEY_SIZE];
};

static void free_master_key(struct fscrypt_master_key *mk)
{
	if (mk) {
		crypto_free_skcipher(mk->mk_ctfm);
		kzfree(mk);
	}
}

static void put_master_key(struct fscrypt_master_key *mk)
{
	if (!refcount_dec_and_lock(&mk->mk_refcount, &fscrypt_master_keys_lock))
		return;
	hash_del(&mk->mk_node);
	spin_unlock(&fscrypt_master_keys_lock);

	free_master_key(mk);
}

/*
 * Find/insert the given master key into the fscrypt_master_keys table.  If
 * found, it is returned with elevated refcount, and 'to_insert' is freed if
 * non-NULL.  If not found, 'to_insert' is inserted and returned if it's
 * non-NULL; otherwise NULL is returned.
 */
static struct fscrypt_master_key *
find_or_insert_master_key(struct fscrypt_master_key *to_insert,
			  const u8 *raw_key, const struct fscrypt_mode *mode,
			  const struct fscrypt_info *ci)
{
	unsigned long hash_key;
	struct fscrypt_master_key *mk;

	/*
	 * Careful: to avoid potentially leaking secret key bytes via timing
	 * information, we must key the hash table by descriptor rather than by
	 * raw key, and use crypto_memneq() when comparing raw keys.
	 */

	BUILD_BUG_ON(sizeof(hash_key) > FS_KEY_DESCRIPTOR_SIZE);
	memcpy(&hash_key, ci->ci_master_key_descriptor, sizeof(hash_key));

	spin_lock(&fscrypt_master_keys_lock);
	hash_for_each_possible(fscrypt_master_keys, mk, mk_node, hash_key) {
		if (memcmp(ci->ci_master_key_descriptor, mk->mk_descriptor,
			   FS_KEY_DESCRIPTOR_SIZE) != 0)
			continue;
		if (mode != mk->mk_mode)
			continue;
		if (crypto_memneq(raw_key, mk->mk_raw, mode->keysize))
			continue;
		/* using existing tfm with same (descriptor, mode, raw_key) */
		refcount_inc(&mk->mk_refcount);
		spin_unlock(&fscrypt_master_keys_lock);
		free_master_key(to_insert);
		return mk;
	}
	if (to_insert)
		hash_add(fscrypt_master_keys, &to_insert->mk_node, hash_key);
	spin_unlock(&fscrypt_master_keys_lock);
	return to_insert;
}

/* Prepare to encrypt directly using the master key in the given mode */
static struct fscrypt_master_key *
fscrypt_get_master_key(const struct fscrypt_info *ci, struct fscrypt_mode *mode,
		       const u8 *raw_key, const struct inode *inode)
{
	struct fscrypt_master_key *mk;
	int err;

	/* Is there already a tfm for this key? */
	mk = find_or_insert_master_key(NULL, raw_key, mode, ci);
	if (mk)
		return mk;

	/* Nope, allocate one. */
	mk = kzalloc(sizeof(*mk), GFP_NOFS);
	if (!mk)
		return ERR_PTR(-ENOMEM);
	refcount_set(&mk->mk_refcount, 1);
	mk->mk_mode = mode;
	mk->mk_ctfm = allocate_skcipher_for_mode(mode, raw_key, inode);
	if (IS_ERR(mk->mk_ctfm)) {
		err = PTR_ERR(mk->mk_ctfm);
		mk->mk_ctfm = NULL;
		goto err_free_mk;
	}
	memcpy(mk->mk_descriptor, ci->ci_master_key_descriptor,
	       FS_KEY_DESCRIPTOR_SIZE);
	memcpy(mk->mk_raw, raw_key, mode->keysize);

	return find_or_insert_master_key(mk, raw_key, mode, ci);

err_free_mk:
	free_master_key(mk);
	return ERR_PTR(err);
}

static int derive_essiv_salt(const u8 *key, int keysize, u8 *salt)
{
	struct crypto_shash *tfm = READ_ONCE(essiv_hash_tfm);

	/* init hash transform on demand */
	if (unlikely(!tfm)) {
		struct crypto_shash *prev_tfm;

		tfm = crypto_alloc_shash("sha256", 0, 0);
		if (IS_ERR(tfm)) {
			fscrypt_warn(NULL,
				     "error allocating SHA-256 transform: %ld",
				     PTR_ERR(tfm));
			return PTR_ERR(tfm);
		}
		prev_tfm = cmpxchg(&essiv_hash_tfm, NULL, tfm);
		if (prev_tfm) {
			crypto_free_shash(tfm);
			tfm = prev_tfm;
		}
	}

	{
		SHASH_DESC_ON_STACK(desc, tfm);
		desc->tfm = tfm;
		desc->flags = 0;

		return crypto_shash_digest(desc, key, keysize, salt);
	}
}

static int init_essiv_generator(struct fscrypt_info *ci, const u8 *raw_key,
				int keysize)
{
	int err;
	struct crypto_cipher *essiv_tfm;
	u8 salt[SHA256_DIGEST_SIZE];

	essiv_tfm = crypto_alloc_cipher("aes", 0, 0);
	if (IS_ERR(essiv_tfm))
		return PTR_ERR(essiv_tfm);

	ci->ci_essiv_tfm = essiv_tfm;

	err = derive_essiv_salt(raw_key, keysize, salt);
	if (err)
		goto out;

	/*
	 * Using SHA256 to derive the salt/key will result in AES-256 being
	 * used for IV generation. File contents encryption will still use the
	 * configured keysize (AES-128) nevertheless.
	 */
	err = crypto_cipher_setkey(essiv_tfm, salt, sizeof(salt));
	if (err)
		goto out;

out:
	memzero_explicit(salt, sizeof(salt));
	return err;
}

void __exit fscrypt_essiv_cleanup(void)
{
	crypto_free_shash(essiv_hash_tfm);
}

/*
 * Given the encryption mode and key (normally the derived key, but for
 * FS_POLICY_FLAG_DIRECT_KEY mode it's the master key), set up the inode's
 * symmetric cipher transform object(s).
 */
static int setup_crypto_transform(struct fscrypt_info *ci,
				  struct fscrypt_mode *mode,
				  const u8 *raw_key, const struct inode *inode)
{
	struct fscrypt_master_key *mk;
	struct crypto_skcipher *ctfm;
	int err;

	if (ci->ci_flags & FS_POLICY_FLAG_DIRECT_KEY) {
		mk = fscrypt_get_master_key(ci, mode, raw_key, inode);
		if (IS_ERR(mk))
			return PTR_ERR(mk);
		ctfm = mk->mk_ctfm;
	} else {
		mk = NULL;
		ctfm = allocate_skcipher_for_mode(mode, raw_key, inode);
		if (IS_ERR(ctfm))
			return PTR_ERR(ctfm);
	}
	ci->ci_master_key = mk;
	ci->ci_ctfm = ctfm;

	if (mode->needs_essiv) {
		/* ESSIV implies 16-byte IVs which implies !DIRECT_KEY */
		WARN_ON(mode->ivsize != AES_BLOCK_SIZE);
		WARN_ON(ci->ci_flags & FS_POLICY_FLAG_DIRECT_KEY);

		err = init_essiv_generator(ci, raw_key, mode->keysize);
		if (err) {
			fscrypt_warn(inode->i_sb,
				     "error initializing ESSIV generator for inode %lu: %d",
				     inode->i_ino, err);
			return err;
		}
	}
	return 0;
}

static void put_crypt_info(struct fscrypt_info *ci)
{
	if (!ci)
		return;

#ifdef CONFIG_FSCRYPT_SDP
	fscrypt_sdp_put_sdp_info(ci->ci_sdp_info);
#endif

	if (ci->ci_master_key) {
		put_master_key(ci->ci_master_key);
	} else {
		if (ci->ci_ctfm)
			crypto_free_skcipher(ci->ci_ctfm);
		if (ci->ci_essiv_tfm)
			crypto_free_cipher(ci->ci_essiv_tfm);
	}
	memset(ci->ci_raw_key, 0, sizeof(ci->ci_raw_key));
	kmem_cache_free(fscrypt_info_cachep, ci);
}

static int fscrypt_data_encryption_mode(struct inode *inode)
{
	return fscrypt_should_be_processed_by_ice(inode) ?
	FS_ENCRYPTION_MODE_PRIVATE : FS_ENCRYPTION_MODE_AES_256_XTS;
}

int fscrypt_get_encryption_info(struct inode *inode)
{
	struct fscrypt_info *crypt_info;
	struct fscrypt_context ctx;
	struct fscrypt_mode *mode;
	u8 *raw_key = NULL;
	int res;
#ifdef CONFIG_FSCRYPT_SDP
	sdp_fs_command_t *cmd = NULL;
#endif

	if (fscrypt_has_encryption_key(inode))
		return 0;

	res = fscrypt_initialize(inode->i_sb->s_cop->flags);
	if (res)
		return res;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		if (!fscrypt_dummy_context_enabled(inode) ||
		    IS_ENCRYPTED(inode))
			return res;
		/* Fake up a context for an unencrypted directory */
		memset(&ctx, 0, sizeof(ctx));
		ctx.format = FS_ENCRYPTION_CONTEXT_FORMAT_V1;
		ctx.contents_encryption_mode =
			fscrypt_data_encryption_mode(inode);
		ctx.filenames_encryption_mode = FS_ENCRYPTION_MODE_AES_256_CTS;
		memset(ctx.master_key_descriptor, 0x42, FS_KEY_DESCRIPTOR_SIZE);
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	crypt_info = kmem_cache_zalloc(fscrypt_info_cachep, GFP_NOFS);
	if (!crypt_info)
		return -ENOMEM;

	crypt_info->ci_flags = ctx.flags;
	crypt_info->ci_data_mode = ctx.contents_encryption_mode;
	crypt_info->ci_filename_mode = ctx.filenames_encryption_mode;
	memcpy(crypt_info->ci_master_key_descriptor, ctx.master_key_descriptor,
	       FS_KEY_DESCRIPTOR_SIZE);
	memcpy(crypt_info->ci_nonce, ctx.nonce, FS_KEY_DERIVATION_NONCE_SIZE);
#ifdef CONFIG_FSCRYPT_SDP
	crypt_info->ci_sdp_info = NULL;
#endif

	mode = select_encryption_mode(crypt_info, inode);
	if (IS_ERR(mode)) {
		res = PTR_ERR(mode);
		goto out;
	}
	WARN_ON(mode->ivsize > FSCRYPT_MAX_IV_SIZE);
	crypt_info->ci_mode = mode;

	/*
	 * This cannot be a stack buffer because it may be passed to the
	 * scatterlist crypto API as part of key derivation.
	 */
	res = -ENOMEM;
	raw_key = kmalloc(mode->keysize, GFP_NOFS);
	if (!raw_key)
		goto out;

#ifdef CONFIG_FSCRYPT_SDP
	if ((FSCRYPT_SDP_PARSE_FLAG_SDP_ONLY(ctx.knox_flags) & FSCRYPT_KNOX_FLG_SDP_MASK)) {
		crypt_info->ci_sdp_info = fscrypt_sdp_alloc_sdp_info();
		if (!crypt_info->ci_sdp_info) {
			res = -ENOMEM;
			goto out;
		}

		res = fscrypt_sdp_update_sdp_info(inode, &ctx, crypt_info);
		if (res)
			goto out;

		if (fscrypt_sdp_is_classified(crypt_info)) {
			res = derive_fek(inode, &ctx, crypt_info, raw_key, mode->keysize);
			if (res) {
				if (fscrypt_sdp_is_sensitive(crypt_info)) {
					cmd = sdp_fs_command_alloc(FSOP_AUDIT_FAIL_DECRYPT,
							current->tgid, crypt_info->ci_sdp_info->engine_id, -1,
							inode->i_ino, res, GFP_NOFS);
					if (cmd) {
						sdp_fs_request(cmd, NULL);
						sdp_fs_command_free(cmd);
					}
				}

				goto out;
			}
			fscrypt_sdp_update_conv_status(crypt_info);
			goto sdp_dek;
		}
	}
#endif

	res = find_and_derive_key(inode, &ctx, raw_key, mode);
	if (res)
		goto out;

#ifdef CONFIG_FSCRYPT_SDP
sdp_dek:
#endif

	if (!mode->inline_encryption) {
		res = setup_crypto_transform(crypt_info, mode, raw_key, inode);
		if (res)
			goto out;
	} else {
		memcpy(crypt_info->ci_raw_key, raw_key, mode->keysize);
	}

	if (cmpxchg_release(&inode->i_crypt_info, NULL, crypt_info) == NULL)
		crypt_info = NULL;
#ifdef CONFIG_FSCRYPT_SDP
	if (crypt_info == NULL) //Call only when i_crypt_info is loaded initially
		fscrypt_sdp_finalize_tasks(inode, raw_key, mode->keysize);
#endif
out:
	if (res == -ENOKEY)
		res = 0;
	put_crypt_info(crypt_info);
	kzfree(raw_key);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_info);

/**
 * fscrypt_put_encryption_info - free most of an inode's fscrypt data
 *
 * Free the inode's fscrypt_info.  Filesystems must call this when the inode is
 * being evicted.  An RCU grace period need not have elapsed yet.
 */
void fscrypt_put_encryption_info(struct inode *inode)
{
#ifdef CONFIG_FSCRYPT_SDP
	fscrypt_sdp_cache_remove_inode_num(inode);
#endif
	put_crypt_info(inode->i_crypt_info);
	inode->i_crypt_info = NULL;
}
EXPORT_SYMBOL(fscrypt_put_encryption_info);

/**
 * fscrypt_free_inode - free an inode's fscrypt data requiring RCU delay
 *
 * Free the inode's cached decrypted symlink target, if any.  Filesystems must
 * call this after an RCU grace period, just before they free the inode.
 */
void fscrypt_free_inode(struct inode *inode)
{
	if (IS_ENCRYPTED(inode) && S_ISLNK(inode->i_mode)) {
		kfree(inode->i_link);
		inode->i_link = NULL;
	}
}
EXPORT_SYMBOL(fscrypt_free_inode);

#ifdef CONFIG_FSCRYPT_SDP
static inline int __find_and_derive_fskey(const struct inode *inode,
						const struct fscrypt_context *ctx,
						struct fscrypt_key *fskey, unsigned int min_keysize)
{
	struct key *key;
	const struct fscrypt_key *payload;

	key = find_and_lock_process_key(FS_KEY_DESC_PREFIX,
					ctx->master_key_descriptor,
					min_keysize, &payload);
	if (key == ERR_PTR(-ENOKEY) && inode->i_sb->s_cop->key_prefix) {
		key = find_and_lock_process_key(inode->i_sb->s_cop->key_prefix,
						ctx->master_key_descriptor,
						min_keysize, &payload);
	}
	if (IS_ERR(key))
		return PTR_ERR(key);
	memcpy(fskey, payload, sizeof(struct fscrypt_key));
	up_read(&key->sem);
	key_put(key);
	return 0;
}

/* The function is only for regular files */
static int derive_fek(struct inode *inode,
						const struct fscrypt_context *ctx,
						struct fscrypt_info *crypt_info,
						u8 *fek, u32 fek_len)
{
	int res = 0;
	/*
	 * 1. [ Native / Uninitialized / To_sensitive ]  --> Plain fek
	 * 2. [ Native / Uninitialized / Non_sensitive ] --> Plain fek
	 */
	if (fscrypt_sdp_is_uninitialized(crypt_info))
	{
		res = fscrypt_sdp_derive_uninitialized_dek(crypt_info, fek, fek_len);
	}
	/*
	 * 3. [ Native / Initialized / Sensitive ]     --> { fek }_SDPK
	 * 4. [ Non_native / Initialized / Sensitive ] --> { fek }_SDPK
	 */
	else if (fscrypt_sdp_is_sensitive(crypt_info))
	{
		res = fscrypt_sdp_derive_dek(crypt_info, fek, fek_len);
	}
	/*
	 * 5. [ Native / Initialized / Non_sensitive ] --> { fek }_cekey
	 */
	else if (fscrypt_sdp_is_native(crypt_info))
	{
		res = fscrypt_sdp_derive_fek(inode, crypt_info, fek, fek_len);
	}
	/*
	 * else { N/A }
	 *
	 * Not classified file.
	 * 6. [ Non_native / Initialized / Non_sensitive ]
	 * 7. [ Non_native / Initialized / To_sensitive ]
	 */
	return res;
}

int fscrypt_get_encryption_key(struct inode *inode, struct fscrypt_key *key)
{
	struct fscrypt_info *crypt_info;
	struct fscrypt_context ctx;
	struct fscrypt_mode *mode;
	u8 *raw_key = NULL;
	int res;

	// fscrypt_info in inode is not initialized yet. It should be called after
	// getting fscrypt_info.
	if (!fscrypt_has_encryption_key(inode))
		return -EINVAL;
	crypt_info = inode->i_crypt_info;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		return res;
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	mode = select_encryption_mode(crypt_info, inode);
	if (IS_ERR(mode)) {
		res = PTR_ERR(mode);
		goto out;
	}

	if (FS_MAX_KEY_SIZE < mode->keysize) {
		return -EPERM;
	}

	/*
	 * This cannot be a stack buffer because it is passed to the scatterlist
	 * crypto API as part of key derivation.
	 */
	res = -ENOMEM;
	raw_key = kmalloc(mode->keysize, GFP_NOFS);
	if (!raw_key)
		goto out;

	res = find_and_derive_key(inode, &ctx, raw_key, mode);
	if (res)
		goto out;

	memcpy(key->raw, raw_key, mode->keysize);
	key->size = mode->keysize;

out:
	kzfree(raw_key);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_key);

int fscrypt_get_encryption_key_classified(struct inode *inode, struct fscrypt_key *key)
{
	struct fscrypt_info *crypt_info;
	struct fscrypt_context ctx;
	struct fscrypt_mode *mode;
	u8 *raw_key = NULL;
	int res;

	// fscrypt_info in inode is not initialized yet. It should be called after
	// getting fscrypt_info.
	if (!fscrypt_has_encryption_key(inode)) {
		return -EINVAL;
	}
	crypt_info = inode->i_crypt_info;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		return res;
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	mode = select_encryption_mode(crypt_info, inode);
	if (IS_ERR(mode)) {
		res = PTR_ERR(mode);
		goto out;
	}

	if (FS_MAX_KEY_SIZE < mode->keysize) {
		return -EPERM;
	}

	/*
	 * This cannot be a stack buffer because it is passed to the scatterlist
	 * crypto API as part of key derivation.
	 */
	res = -ENOMEM;
	raw_key = kmalloc(mode->keysize, GFP_NOFS);
	if (!raw_key)
		goto out;

	res = derive_fek(inode, &ctx, crypt_info, raw_key, mode->keysize);
	if (res)
		goto out;

	memcpy(key->raw, raw_key, mode->keysize);
	key->size = mode->keysize;

out:
	kzfree(raw_key);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_key_classified);

int fscrypt_get_encryption_kek(struct inode *inode,
							struct fscrypt_info *crypt_info,
							struct fscrypt_key *kek)
{
	struct fscrypt_context ctx;
	struct fscrypt_mode *mode;
	int res;

	if (!crypt_info)
		return -EINVAL;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		return res;
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	mode = select_encryption_mode(crypt_info, inode);
	if (IS_ERR(mode)) {
		res = PTR_ERR(mode);
		goto out;
	}

	if (FS_MAX_KEY_SIZE < mode->keysize) {
		return -EPERM;
	}

	res = __find_and_derive_fskey(inode, &ctx, kek, mode->keysize);

out:
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_kek);
#endif