/* Copyright (c) 2021 Intel Corporation
 * Copyright (c) 2020-2021 Alibaba Cloud
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <rats-tls/log.h>
#include <rats-tls/attester.h>
#include <stddef.h>
#include <stdio.h>
#include "../../verifiers/tdx-ecdsa/tdx-ecdsa.h"

#define VSOCK

// clang-format off
#ifdef VSOCK
  #include <stdlib.h>
  #include <stdio.h>
  #include <stdint.h>
  #include <time.h>
  #include <tdx_attest.h>
#endif
// clang-format on

#define TDEL_INFO "/sys/firmware/acpi/tables/TDEL"
#define TDEL_DATA "/sys/firmware/acpi/tables/data/TDEL"
#define TDEL_INFO_SZ   0x38
#define TDEL_DATA_SZ   0x10000

static int tdx_get_report(const tdx_report_data_t *report_data, tdx_report_t *tdx_report)
{
	/* Get report by tdcall */
	if (tdx_att_get_report(report_data, tdx_report) != TDX_ATTEST_SUCCESS) {
		RTLS_ERR("failed to ioctl get tdx report data.\n");
		return -1;
	}

	return 0;
}

enclave_attester_err_t tdx_get_tdel_info(enclave_attester_ctx_t *ctx, attestation_evidence_t *evidence)
{
    int fd = 0;
    char tdel_info[TDEL_INFO_SZ];

    RTLS_DEBUG("ctx %p, evidence %p\n", ctx, evidence);
    if (fd = open(TDEL_INFO, O_RDONLY) < 0) {
        RTLS_ERR("failed to open TDEL info device\n");
        return -ENCLAVE_ATTESTER_ERR_INVALID;
    }

    int tdel_info_sz = read(fd, tdel_info, TDEL_INFO_SZ);
    if (tdel_info_sz != TDEL_INFO_SZ) {
        RTLS_ERR("failed to read tdinfo\n");
        return -ENCLAVE_ATTESTER_ERR_INVALID;
    }

    memcpy(&(evidence->tdx.quote[8192]), tdel_info, TDEL_INFO_SZ);

    return ENCLAVE_ATTESTER_ERR_NONE;
}

enclave_attester_err_t tdx_get_tdel_data(enclave_attester_ctx_t *ctx,
        attestation_evidence_t *evidence)
{
    int fd = 0;
    char tdel_data[TDEL_DATA_SZ];

    RTLS_DEBUG("ctx %p, evidence %p\n", ctx, evidence);
    if (fd = open(TDEL_DATA, O_RDONLY) < 0) {
        RTLS_ERR("failed to open TDEL info device\n");
        return -ENCLAVE_ATTESTER_ERR_INVALID;
    }

    int tdel_data_sz = read(fd, tdel_data, TDEL_DATA_SZ);

    memcpy(&(evidence->tdx.quote[8192 + TDEL_INFO_SZ]), tdel_data, tdel_data_sz);

    return ENCLAVE_ATTESTER_ERR_NONE;
}

static int tdx_gen_quote(uint8_t *hash, uint8_t *quote_buf, uint32_t *quote_size)
{
	if (hash == NULL) {
		RTLS_ERR("empty hash pointer.\n");
		return -1;
	}

	tdx_report_t tdx_report = { { 0 } };
	tdx_report_data_t report_data = { { 0 } };
	assert(sizeof(report_data.d) >= SHA256_HASH_SIZE);
	memcpy(report_data.d, hash, SHA256_HASH_SIZE);
	int ret = tdx_get_report(&report_data, &tdx_report);
	if (ret != 0) {
		RTLS_ERR("failed to get tdx report.\n");
		return -1;
	}

#ifdef VSOCK
	tdx_uuid_t selected_att_key_id = { { 0 } };
	uint8_t *p_quote = NULL;
	uint32_t p_quote_size = 0;
	if (tdx_att_get_quote(&report_data, NULL, 0, &selected_att_key_id, &p_quote, &p_quote_size,
			      0) != TDX_ATTEST_SUCCESS) {
		RTLS_ERR("failed to get tdx quote.\n");
		return -1;
	}

	if (p_quote_size > *quote_size) {
		RTLS_ERR("quote buffer is too small.\n");
		tdx_att_free_quote(p_quote);
		return -1;
	}

	memcpy(quote_buf, p_quote, p_quote_size);
	*quote_size = p_quote_size;
	tdx_att_free_quote(p_quote);
#else
	/* This branch is for getting quote size and quote by tdcall,
	 * it depends on the implemetation in qemu.
	 */
	#error "using tdcall to retrieve TD quote is still not supported!"
#endif

	return 0;
}

enclave_attester_err_t tdx_ecdsa_collect_evidence(enclave_attester_ctx_t *ctx,
						  attestation_evidence_t *evidence,
						  rats_tls_cert_algo_t algo, uint8_t *hash,
						  __attribute__((unused)) uint32_t hash_len)
{
    RTLS_DEBUG("ctx %p, evidence %p, algo %d, hash %p\n", ctx, evidence, algo, hash);

    if (tdx_gen_quote(hash, evidence->tdx.quote, &evidence->tdx.quote_len)) {
        RTLS_ERR("failed to generate quote\n");
        return -ENCLAVE_ATTESTER_ERR_INVALID;
    }

    RTLS_DEBUG("Succeed to generate the quote!\n");

    if (tdx_get_tdel_info(ctx, evidence) != ENCLAVE_ATTESTER_ERR_NONE)
        return -ENCLAVE_ATTESTER_ERR_INVALID;

    if (tdx_get_tdel_data(ctx, evidence) != ENCLAVE_ATTESTER_ERR_NONE)
        return -ENCLAVE_ATTESTER_ERR_INVALID;

	/* Essentially speaking, QGS generates the same
	 * format of quote as sgx_ecdsa.
	 */
	snprintf(evidence->type, sizeof(evidence->type), "tdx_ecdsa");

	RTLS_DEBUG("ctx %p, evidence %p, quote_size %d\n", ctx, evidence, evidence->tdx.quote_len);

	return ENCLAVE_ATTESTER_ERR_NONE;
}
