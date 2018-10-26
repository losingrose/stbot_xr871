/*
 * libwebsockets - OpenSSL-specific server functions
 *
 * Copyright (C) 2010-2017 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "private-libwebsockets.h"
#if !defined(LWS_WITH_MBEDTLS) && defined(LWS_OPENSSL_SUPPORT)

extern int openssl_websocket_private_data_index,
	   openssl_SSL_CTX_private_data_index;

static int
OpenSSL_verify_callback(int preverify_ok, X509_STORE_CTX *x509_ctx)
{
	SSL *ssl;
	int n;
	struct lws *wsi;
	union lws_tls_cert_info_results ir;
	X509 *topcert = X509_STORE_CTX_get_current_cert(x509_ctx);

	ssl = X509_STORE_CTX_get_ex_data(x509_ctx,
		SSL_get_ex_data_X509_STORE_CTX_idx());

	/*
	 * !!! nasty openssl requires the index to come as a library-scope
	 * static
	 */
	wsi = SSL_get_ex_data(ssl, openssl_websocket_private_data_index);

	n = lws_tls_openssl_cert_info(topcert, LWS_TLS_CERT_INFO_COMMON_NAME, &ir,
				   sizeof(ir.ns.name));
	if (!n)
		lwsl_info("%s: client cert CN '%s'\n", __func__, ir.ns.name);
	else
		lwsl_info("%s: couldn't get client cert CN\n", __func__);

	n = wsi->vhost->protocols[0].callback(wsi,
			LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION,
					   x509_ctx, ssl, preverify_ok);

	/* convert return code from 0 = OK to 1 = OK */
	return !n;
}

int
lws_tls_server_client_cert_verify_config(struct lws_vhost *vh)
{
	int verify_options = SSL_VERIFY_PEER;

	/* as a server, are we requiring clients to identify themselves? */

	if (!lws_check_opt(vh->options,
			  LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT))
		return 0;

	if (!lws_check_opt(vh->options,
			   LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED))
		verify_options |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

	SSL_CTX_set_session_id_context(vh->ssl_ctx, (uint8_t *)vh->context,
				       sizeof(void *));

	/* absolutely require the client cert */
	SSL_CTX_set_verify(vh->ssl_ctx, verify_options, OpenSSL_verify_callback);

	return 0;
}

#if defined(SSL_TLSEXT_ERR_NOACK) && !defined(OPENSSL_NO_TLSEXT)
static int
lws_ssl_server_name_cb(SSL *ssl, int *ad, void *arg)
{
	struct lws_context *context = (struct lws_context *)arg;
	struct lws_vhost *vhost, *vh;
	const char *servername;

	if (!ssl)
		return SSL_TLSEXT_ERR_NOACK;

	/*
	 * We can only get ssl accepted connections by using a vhost's ssl_ctx
	 * find out which listening one took us and only match vhosts on the
	 * same port.
	 */
	vh = context->vhost_list;
	while (vh) {
		if (!vh->being_destroyed &&
		    vh->ssl_ctx == SSL_get_SSL_CTX(ssl))
			break;
		vh = vh->vhost_next;
	}

	if (!vh) {
		assert(vh); /* can't match the incoming vh? */
		return SSL_TLSEXT_ERR_OK;
	}

	servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (!servername) {
		/* the client doesn't know what hostname it wants */
		lwsl_info("SNI: Unknown ServerName: %s\n", servername);

		return SSL_TLSEXT_ERR_OK;
	}

	vhost = lws_select_vhost(context, vh->listen_port, servername);
	if (!vhost) {
		lwsl_info("SNI: none: %s:%d\n", servername, vh->listen_port);

		return SSL_TLSEXT_ERR_OK;
	}

	lwsl_info("SNI: Found: %s:%d\n", servername, vh->listen_port);

	/* select the ssl ctx from the selected vhost for this conn */
	SSL_set_SSL_CTX(ssl, vhost->ssl_ctx);

	return SSL_TLSEXT_ERR_OK;
}
#endif

/*
 * this may now get called after the vhost creation, when certs become
 * available.
 */
int
lws_tls_server_certs_load(struct lws_vhost *vhost, struct lws *wsi,
			  const char *cert, const char *private_key,
			  const char *mem_cert, size_t len_mem_cert,
			  const char *mem_privkey, size_t mem_privkey_len)
{
#if defined(LWS_HAVE_OPENSSL_ECDH_H)
	const char *ecdh_curve = "prime256v1";
	EC_KEY *ecdh, *EC_key = NULL;
	EVP_PKEY *pkey;
	X509 *x = NULL;
	int ecdh_nid;
	int KeyType;
#if defined(LWS_HAVE_SSL_EXTRA_CHAIN_CERTS)
	STACK_OF(X509) *extra_certs = NULL;
#endif
#endif
	unsigned long error;
	uint8_t *p;
	lws_filepos_t flen;

	int n = lws_tls_generic_cert_checks(vhost, cert, private_key), m;

	if (n == LWS_TLS_EXTANT_NO && (!mem_cert || !mem_privkey))
		return 0;

	if (n == LWS_TLS_EXTANT_NO)
		n = LWS_TLS_EXTANT_ALTERNATIVE;

	if (n == LWS_TLS_EXTANT_ALTERNATIVE && (!mem_cert || !mem_privkey))
		return 1; /* no alternative */

	if (n == LWS_TLS_EXTANT_ALTERNATIVE) {
		/*
		 * Although we have prepared update certs, we no longer have
		 * the rights to read our own cert + key we saved.
		 *
		 * If we were passed copies in memory buffers, use those
		 * instead.
		 *
		 * The passed memory-buffer cert image is in DER, and the
		 * memory-buffer private key image is PEM.
		 */
		if (SSL_CTX_use_certificate_ASN1(vhost->ssl_ctx,
						 (int)len_mem_cert,
						 (uint8_t *)mem_cert) != 1) {
			lwsl_err("Problem loading update cert\n");

			return 1;
		}

		if (lws_tls_alloc_pem_to_der_file(vhost->context, NULL,
						  mem_privkey, mem_privkey_len,
						  &p, &flen)) {
			lwsl_notice("unable to convert memory privkey\n");

			return 1;
		}
		if (SSL_CTX_use_PrivateKey_ASN1(EVP_PKEY_RSA, vhost->ssl_ctx,
						p, (long)(long long)flen) != 1) {
			lwsl_notice("unable to use memory privkey\n");

			return 1;
		}

		goto check_key;
	}

	/* set the local certificate from CertFile */
	m = SSL_CTX_use_certificate_chain_file(vhost->ssl_ctx, cert);
	if (m != 1) {
		error = ERR_get_error();
		lwsl_err("problem getting cert '%s' %lu: %s\n",
			 cert, error, ERR_error_string(error,
			       (char *)vhost->context->pt[0].serv_buf));

		return 1;
	}

	if (n != LWS_TLS_EXTANT_ALTERNATIVE && private_key) {
		/* set the private key from KeyFile */
		if (SSL_CTX_use_PrivateKey_file(vhost->ssl_ctx, private_key,
					        SSL_FILETYPE_PEM) != 1) {
			error = ERR_get_error();
			lwsl_err("ssl problem getting key '%s' %lu: %s\n",
				 private_key, error,
				 ERR_error_string(error,
				      (char *)vhost->context->pt[0].serv_buf));
			return 1;
		}
	} else {
		if (vhost->protocols[0].callback(wsi,
		    LWS_CALLBACK_OPENSSL_CONTEXT_REQUIRES_PRIVATE_KEY,
		    vhost->ssl_ctx, NULL, 0)) {
			lwsl_err("ssl private key not set\n");

			return 1;
		}
	}

check_key:
	/* verify private key */
	if (!SSL_CTX_check_private_key(vhost->ssl_ctx)) {
		lwsl_err("Private SSL key doesn't match cert\n");

		return 1;
	}

#if defined(LWS_HAVE_OPENSSL_ECDH_H)
	if (vhost->ecdh_curve[0])
		ecdh_curve = vhost->ecdh_curve;

	ecdh_nid = OBJ_sn2nid(ecdh_curve);
	if (NID_undef == ecdh_nid) {
		lwsl_err("SSL: Unknown curve name '%s'", ecdh_curve);
		return 1;
	}

	ecdh = EC_KEY_new_by_curve_name(ecdh_nid);
	if (NULL == ecdh) {
		lwsl_err("SSL: Unable to create curve '%s'", ecdh_curve);
		return 1;
	}
	SSL_CTX_set_tmp_ecdh(vhost->ssl_ctx, ecdh);
	EC_KEY_free(ecdh);

	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_SINGLE_ECDH_USE);

	lwsl_notice(" SSL ECDH curve '%s'\n", ecdh_curve);

	if (lws_check_opt(vhost->context->options, LWS_SERVER_OPTION_SSL_ECDH))
		lwsl_notice(" Using ECDH certificate support\n");

	/* Get X509 certificate from ssl context */
#if !defined(LWS_HAVE_SSL_EXTRA_CHAIN_CERTS)
	x = sk_X509_value(vhost->ssl_ctx->extra_certs, 0);
#else
	SSL_CTX_get_extra_chain_certs_only(vhost->ssl_ctx, &extra_certs);
	if (extra_certs)
		x = sk_X509_value(extra_certs, 0);
	else
		lwsl_err("%s: no extra certs\n", __func__);
#endif
	if (!x) {
		lwsl_err("%s: x is NULL\n", __func__);
		goto post_ecdh;
	}
	/* Get the public key from certificate */
	pkey = X509_get_pubkey(x);
	if (!pkey) {
		lwsl_err("%s: pkey is NULL\n", __func__);

		return 1;
	}
	/* Get the key type */
	KeyType = EVP_PKEY_type(EVP_PKEY_id(pkey));

	if (EVP_PKEY_EC != KeyType) {
		lwsl_notice("Key type is not EC\n");
		return 0;
	}
	/* Get the key */
	EC_key = EVP_PKEY_get1_EC_KEY(pkey);
	/* Set ECDH parameter */
	if (!EC_key) {
		lwsl_err("%s: ECDH key is NULL \n", __func__);
		return 1;
	}
	SSL_CTX_set_tmp_ecdh(vhost->ssl_ctx, EC_key);

	EC_KEY_free(EC_key);
#else
	lwsl_notice(" OpenSSL doesn't support ECDH\n");
#endif

post_ecdh:
	vhost->skipped_certs = 0;

	return 0;
}

int
lws_tls_server_vhost_backend_init(struct lws_context_creation_info *info,
				  struct lws_vhost *vhost,
				  struct lws *wsi)
{
	unsigned long error;
	SSL_METHOD *method = (SSL_METHOD *)SSLv23_server_method();

	if (!method) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl method %lu: %s\n",
				error, ERR_error_string(error,
				      (char *)vhost->context->pt[0].serv_buf));
		return 1;
	}
	vhost->ssl_ctx = SSL_CTX_new(method);	/* create context */
	if (!vhost->ssl_ctx) {
		error = ERR_get_error();
		lwsl_err("problem creating ssl context %lu: %s\n",
				error, ERR_error_string(error,
				      (char *)vhost->context->pt[0].serv_buf));
		return 1;
	}

	SSL_CTX_set_ex_data(vhost->ssl_ctx, openssl_SSL_CTX_private_data_index,
			    (char *)vhost->context);
	/* Disable SSLv2 and SSLv3 */
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
#ifdef SSL_OP_NO_COMPRESSION
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_NO_COMPRESSION);
#endif
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_SINGLE_DH_USE);
	SSL_CTX_set_options(vhost->ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);

	if (info->ssl_cipher_list)
		SSL_CTX_set_cipher_list(vhost->ssl_ctx, info->ssl_cipher_list);

#if !defined(OPENSSL_NO_TLSEXT)
	SSL_CTX_set_tlsext_servername_callback(vhost->ssl_ctx,
					       lws_ssl_server_name_cb);
	SSL_CTX_set_tlsext_servername_arg(vhost->ssl_ctx, vhost->context);
#endif

	if (info->ssl_ca_filepath &&
	    !SSL_CTX_load_verify_locations(vhost->ssl_ctx,
					   info->ssl_ca_filepath, NULL)) {
		lwsl_err("%s: SSL_CTX_load_verify_locations unhappy\n",
			 __func__);
	}

	if (info->ssl_options_set)
		SSL_CTX_set_options(vhost->ssl_ctx, info->ssl_options_set);

/* SSL_clear_options introduced in 0.9.8m */
#if (OPENSSL_VERSION_NUMBER >= 0x009080df) && !defined(USE_WOLFSSL)
	if (info->ssl_options_clear)
		SSL_CTX_clear_options(vhost->ssl_ctx, info->ssl_options_clear);
#endif

	lwsl_info(" SSL options 0x%lX\n", SSL_CTX_get_options(vhost->ssl_ctx));
	if (!vhost->use_ssl || !info->ssl_cert_filepath)
		return 0;

	lws_ssl_bind_passphrase(vhost->ssl_ctx, info);

	return lws_tls_server_certs_load(vhost, wsi, info->ssl_cert_filepath,
					 info->ssl_private_key_filepath,
					 NULL, 0, NULL, 0);
}

int
lws_tls_server_new_nonblocking(struct lws *wsi, lws_sockfd_type accept_fd)
{
#if !defined(USE_WOLFSSL)
	BIO *bio;
#endif

	errno = 0;
	wsi->ssl = SSL_new(wsi->vhost->ssl_ctx);
	if (wsi->ssl == NULL) {
		lwsl_err("SSL_new failed: %d (errno %d)\n",
			 lws_ssl_get_error(wsi, 0), errno);

		lws_ssl_elaborate_error();
		return 1;
	}

	SSL_set_ex_data(wsi->ssl, openssl_websocket_private_data_index, wsi);
	SSL_set_fd(wsi->ssl, (int)(long long)accept_fd);

#ifdef USE_WOLFSSL
#ifdef USE_OLD_CYASSL
	CyaSSL_set_using_nonblock(wsi->ssl, 1);
#else
	wolfSSL_set_using_nonblock(wsi->ssl, 1);
#endif
#else

	SSL_set_mode(wsi->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	bio = SSL_get_rbio(wsi->ssl);
	if (bio)
		BIO_set_nbio(bio, 1); /* nonblocking */
	else
		lwsl_notice("NULL rbio\n");
	bio = SSL_get_wbio(wsi->ssl);
	if (bio)
		BIO_set_nbio(bio, 1); /* nonblocking */
	else
		lwsl_notice("NULL rbio\n");
#endif

#if defined (LWS_HAVE_SSL_SET_INFO_CALLBACK)
		if (wsi->vhost->ssl_info_event_mask)
			SSL_set_info_callback(wsi->ssl, lws_ssl_info_callback);
#endif

	return 0;
}

int
lws_tls_server_abort_connection(struct lws *wsi)
{
	SSL_shutdown(wsi->ssl);
	SSL_free(wsi->ssl);

	return 0;
}

enum lws_ssl_capable_status
lws_tls_server_accept(struct lws *wsi)
{
	union lws_tls_cert_info_results ir;
	int m, n = SSL_accept(wsi->ssl);

	if (n == 1) {
		n = lws_tls_peer_cert_info(wsi, LWS_TLS_CERT_INFO_COMMON_NAME, &ir,
					   sizeof(ir.ns.name));
		if (!n)
			lwsl_notice("%s: client cert CN '%s'\n",
				    __func__, ir.ns.name);
		else
			lwsl_info("%s: couldn't get client cert CN\n", __func__);
		return LWS_SSL_CAPABLE_DONE;
	}

	m = lws_ssl_get_error(wsi, n);

	if (m == SSL_ERROR_SYSCALL || m == SSL_ERROR_SSL)
		return LWS_SSL_CAPABLE_ERROR;

	if (m == SSL_ERROR_WANT_READ || SSL_want_read(wsi->ssl)) {
		if (lws_change_pollfd(wsi, 0, LWS_POLLIN)) {
			lwsl_info("%s: WANT_READ change_pollfd failed\n",
				  __func__);
			return LWS_SSL_CAPABLE_ERROR;
		}

		lwsl_info("SSL_ERROR_WANT_READ\n");
		return LWS_SSL_CAPABLE_MORE_SERVICE_READ;
	}
	if (m == SSL_ERROR_WANT_WRITE || SSL_want_write(wsi->ssl)) {
		lwsl_debug("%s: WANT_WRITE\n", __func__);

		if (lws_change_pollfd(wsi, 0, LWS_POLLOUT)) {
			lwsl_info("%s: WANT_WRITE change_pollfd failed\n",
				  __func__);
			return LWS_SSL_CAPABLE_ERROR;
		}
		return LWS_SSL_CAPABLE_MORE_SERVICE_WRITE;
	}

	return LWS_SSL_CAPABLE_ERROR;
}

#if defined(LWS_WITH_ACME)
static int
lws_tls_openssl_rsa_new_key(RSA **rsa, int bits)
{
	BIGNUM *bn = BN_new();
	int n;

	if (!bn)
		return 1;

	if (BN_set_word(bn, RSA_F4) != 1) {
		BN_free(bn);
		return 1;
	}

	*rsa = RSA_new();
	if (!*rsa) {
		BN_free(bn);
		return 1;
	}

	n = RSA_generate_key_ex(*rsa, bits, bn, NULL);
	BN_free(bn);
	if (n == 1)
		return 0;

	RSA_free(*rsa);
	*rsa = NULL;

	return 1;
}

struct lws_tls_ss_pieces {
	X509 *x509;
	EVP_PKEY *pkey;
	RSA *rsa;
};

LWS_VISIBLE LWS_EXTERN int
lws_tls_acme_sni_cert_create(struct lws_vhost *vhost, const char *san_a,
			     const char *san_b)
{
	GENERAL_NAMES *gens = sk_GENERAL_NAME_new_null();
	GENERAL_NAME *gen = NULL;
	ASN1_IA5STRING *ia5 = NULL;
	X509_NAME *name;

	if (!gens)
		return 1;

	vhost->ss = lws_zalloc(sizeof(*vhost->ss), "sni cert");
	if (!vhost->ss) {
		GENERAL_NAMES_free(gens);
		return 1;
	}

	vhost->ss->x509 = X509_new();
	if (!vhost->ss->x509)
		goto bail;

	ASN1_INTEGER_set(X509_get_serialNumber(vhost->ss->x509), 1);
	X509_gmtime_adj(X509_get_notBefore(vhost->ss->x509), 0);
	X509_gmtime_adj(X509_get_notAfter(vhost->ss->x509), 3600);

	vhost->ss->pkey = EVP_PKEY_new();
	if (!vhost->ss->pkey)
		goto bail0;

	if (lws_tls_openssl_rsa_new_key(&vhost->ss->rsa, 4096))
		goto bail1;

	if (!EVP_PKEY_assign_RSA(vhost->ss->pkey, vhost->ss->rsa))
		goto bail2;

	X509_set_pubkey(vhost->ss->x509, vhost->ss->pkey);

	name = X509_get_subject_name(vhost->ss->x509);
	X509_NAME_add_entry_by_txt(name, "C",  MBSTRING_ASC,
				   (unsigned char *)"GB",          -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "O",  MBSTRING_ASC,
				   (unsigned char *)"somecompany", -1, -1, 0);
	if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
				   (unsigned char *)"temp.acme.invalid",
				   	   	   -1, -1, 0) != 1) {
		lwsl_notice("failed to add CN\n");
		goto bail2;
	}
	X509_set_issuer_name(vhost->ss->x509, name);

	/* add the SAN payloads */

	gen = GENERAL_NAME_new();
	ia5 = ASN1_IA5STRING_new();
	if (!ASN1_STRING_set(ia5, san_a, -1)) {
		lwsl_notice("failed to set ia5\n");
		GENERAL_NAME_free(gen);
		goto bail2;
	}
	GENERAL_NAME_set0_value(gen, GEN_DNS, ia5);
	sk_GENERAL_NAME_push(gens, gen);

	if (X509_add1_ext_i2d(vhost->ss->x509, NID_subject_alt_name,
			    gens, 0, X509V3_ADD_APPEND) != 1)
		goto bail2;

	GENERAL_NAMES_free(gens);

	if (san_b && san_b[0]) {
		gens = sk_GENERAL_NAME_new_null();
		gen = GENERAL_NAME_new();
		ia5 = ASN1_IA5STRING_new();
		if (!ASN1_STRING_set(ia5, san_a, -1)) {
			lwsl_notice("failed to set ia5\n");
			GENERAL_NAME_free(gen);
			goto bail2;
		}
		GENERAL_NAME_set0_value(gen, GEN_DNS, ia5);
		sk_GENERAL_NAME_push(gens, gen);

		if (X509_add1_ext_i2d(vhost->ss->x509, NID_subject_alt_name,
				    gens, 0, X509V3_ADD_APPEND) != 1)
			goto bail2;

		GENERAL_NAMES_free(gens);
	}

	/* sign it with our private key */
	if (!X509_sign(vhost->ss->x509, vhost->ss->pkey, EVP_sha256()))
		goto bail2;

#if 0
	{/* useful to take a sample of a working cert for mbedtls to crib */
		FILE *fp = fopen("/tmp/acme-temp-cert", "w+");

		i2d_X509_fp(fp, vhost->ss->x509);
		fclose(fp);
	}
#endif

	/* tell the vhost to use our crafted certificate */
	SSL_CTX_use_certificate(vhost->ssl_ctx, vhost->ss->x509);
	/* and to use our generated private key */
	SSL_CTX_use_PrivateKey(vhost->ssl_ctx, vhost->ss->pkey);

	return 0;

bail2:
	RSA_free(vhost->ss->rsa);
bail1:
	EVP_PKEY_free(vhost->ss->pkey);
bail0:
	X509_free(vhost->ss->x509);
bail:
	lws_free(vhost->ss);
	GENERAL_NAMES_free(gens);

	return 1;
}

void
lws_tls_acme_sni_cert_destroy(struct lws_vhost *vhost)
{
	if (!vhost->ss)
		return;

	EVP_PKEY_free(vhost->ss->pkey);
	X509_free(vhost->ss->x509);
	lws_free_set_NULL(vhost->ss);
}

static int
lws_tls_openssl_add_nid(X509_NAME *name, int nid, const char *value)
{
	X509_NAME_ENTRY *e;
	int n;

	if (!value || value[0] == '\0')
		value = "none";

	e = X509_NAME_ENTRY_create_by_NID(NULL, nid, MBSTRING_ASC,
					  (unsigned char *)value, -1);
	if (!e)
		return 1;
	n = X509_NAME_add_entry(name, e, -1, 0);
	X509_NAME_ENTRY_free(e);

	return n != 1;
}

static int nid_list[] = {
	NID_countryName,		/* LWS_TLS_REQ_ELEMENT_COUNTRY */
	NID_stateOrProvinceName,	/* LWS_TLS_REQ_ELEMENT_STATE */
	NID_localityName,		/* LWS_TLS_REQ_ELEMENT_LOCALITY */
	NID_organizationName,		/* LWS_TLS_REQ_ELEMENT_ORGANIZATION */
	NID_commonName,			/* LWS_TLS_REQ_ELEMENT_COMMON_NAME */
	NID_organizationalUnitName,	/* LWS_TLS_REQ_ELEMENT_EMAIL */
};

LWS_VISIBLE LWS_EXTERN int
lws_tls_acme_sni_csr_create(struct lws_context *context, const char *elements[],
			    uint8_t *csr, size_t csr_len, char **privkey_pem,
			    size_t *privkey_len)
{
	uint8_t *csr_in = csr;
	RSA *rsakey;
	X509_REQ *req;
	X509_NAME *subj;
	EVP_PKEY *pkey;
	char *p, *end;
	BIO *bio;
	long bio_len;
	int n, ret = -1;

	if (lws_tls_openssl_rsa_new_key(&rsakey, 4096))
		return -1;

	pkey = EVP_PKEY_new();
	if (!pkey)
		goto bail0;
	if (!EVP_PKEY_set1_RSA(pkey, rsakey))
		goto bail1;

	req = X509_REQ_new();
	if (!req)
	        goto bail1;

	X509_REQ_set_pubkey(req, pkey);

	subj = X509_NAME_new();
	if (!subj)
		goto bail2;

	for (n = 0; n < LWS_TLS_REQ_ELEMENT_COUNT; n++)
		if (lws_tls_openssl_add_nid(subj, nid_list[n], elements[n])) {
			lwsl_notice("%s: failed to add element %d\n", __func__, n);
			goto bail3;
		}

	if (X509_REQ_set_subject_name(req, subj) != 1)
		goto bail3;

	if (!X509_REQ_sign(req, pkey, EVP_sha256()))
		goto bail3;

	/*
	 * issue the CSR as PEM to a BIO, and translate to b64urlenc without
	 * headers, trailers, or whitespace
	 */

	bio = BIO_new(BIO_s_mem());
	if (!bio)
		goto bail3;

	if (PEM_write_bio_X509_REQ(bio, req) != 1) {
		BIO_free(bio);
		goto bail3;
	}

	bio_len = BIO_get_mem_data(bio, &p);
	end = p + bio_len;

	/* strip the header line */
	while (p < end && *p != '\n')
		p++;

	while (p < end && csr_len) {
		if (*p == '\n') {
			p++;
			continue;
		}

		if (*p == '-')
			break;

		if (*p == '+')
			*csr++ = '-';
		else
			if (*p == '/')
				*csr++ = '_';
			else
				*csr++ = *p;
		p++;
		csr_len--;
	}
	BIO_free(bio);
	if (!csr_len) {
		lwsl_notice("%s: need %ld for CSR\n", __func__, bio_len);
		goto bail3;
	}

	/*
	 * Also return the private key as a PEM in memory
	 * (platform may not have a filesystem)
	 */
	bio = BIO_new(BIO_s_mem());
	if (!bio)
		goto bail3;

	if (PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, 0, NULL) != 1) {
		BIO_free(bio);
		goto bail3;
	}
	bio_len = BIO_get_mem_data(bio, &p);
	*privkey_pem = malloc(bio_len); /* malloc so user code can own / free */
	*privkey_len = (size_t)bio_len;
	if (!*privkey_pem) {
		lwsl_notice("%s: need %ld for private key\n", __func__, bio_len);
		BIO_free(bio);
		goto bail3;
	}
	memcpy(*privkey_pem, p, (int)(long long)bio_len);
	BIO_free(bio);

	ret = lws_ptr_diff(csr, csr_in);

bail3:
	X509_NAME_free(subj);
bail2:
	X509_REQ_free(req);
bail1:
	EVP_PKEY_free(pkey);
bail0:
	RSA_free(rsakey);

	return ret;
}
#endif

#endif