/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2024 1BitSquared <info@1bitsquared.com>
 * Written by Rachel Mant <git@dragonmux.network>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file implements transport generic ADIv6 functions.
 *
 * See the following ARM Reference Documents:
 * ARM Debug Interface v6 Architecture Specification, IHI0074 ver. e
 * - https://developer.arm.com/documentation/ihi0074/latest/
 */

#include "general.h"
#include "adiv6.h"
#include "adiv6_internal.h"

static bool adiv6_component_probe(adiv5_debug_port_s *dp, target_addr64_t base_address, uint32_t entry_number);
static uint32_t adiv6_ap_reg_read(adiv5_access_port_s *base_ap, uint16_t addr);
static void adiv6_ap_reg_write(adiv5_access_port_s *base_ap, uint16_t addr, uint32_t value);

static target_addr64_t adiv6_dp_read_base_address(adiv5_debug_port_s *const dp)
{
	/* BASEPTR0 is on bank 2 */
	adiv5_dp_write(dp, ADIV5_DP_SELECT, ADIV5_DP_BANK2);
	const uint32_t baseptr0 = adiv5_dp_read(dp, ADIV6_DP_BASEPTR0);
	/* BASEPTR1 is on bank 3 */
	adiv5_dp_write(dp, ADIV5_DP_SELECT, ADIV5_DP_BANK3);
	const uint32_t baseptr1 = adiv5_dp_read(dp, ADIV6_DP_BASEPTR1);
	/* Now re-combine the values and return */
	return baseptr0 | ((uint64_t)baseptr1 << 32U);
}

bool adiv6_dp_init(adiv5_debug_port_s *const dp)
{
	dp->ap_read = adiv6_ap_reg_read;
	dp->ap_write = adiv6_ap_reg_write;

	/* DPIDR1 is on bank 1 */
	adiv5_dp_write(dp, ADIV5_DP_SELECT, ADIV5_DP_BANK1);
	/* Read the other DPIDR and figure out the DP bus address width */
	const uint32_t dpidr1 = adiv5_dp_read(dp, ADIV6_DP_DPIDR1);
	dp->address_width = dpidr1 & ADIV6_DP_DPIDR1_ASIZE_MASK;

	DEBUG_INFO("DP DPIDR1 0x%08" PRIx32 " %u-bit addressing\n", dpidr1, dp->address_width);

	/* Now we know how wide the DP bus addresses are, read out the base pointers and validate them */
	target_addr64_t base_address = adiv6_dp_read_base_address(dp);
	if (!(base_address & ADIV6_DP_BASEPTR0_VALID)) {
		DEBUG_INFO("No valid base address on DP\n");
		return false;
	}
	if ((base_address & ((UINT64_C(1) << dp->address_width) - 1U)) != base_address) {
		DEBUG_INFO("Bad base address %" PRIx32 "%08" PRIx32 "on DP\n", (uint32_t)(base_address >> 32U),
			(uint32_t)base_address);
		return false;
	}
	base_address &= ADIV6_DP_BASE_ADDRESS_MASK;

	return adiv6_component_probe(dp, base_address, 0U);
}

static uint32_t adiv6_dp_read_id(adiv6_access_port_s *const ap, const uint16_t addr)
{
	/*
	 * Set up the DP resource bus to do the reads.
	 * Set SELECT1 in the DP up first
	 */
	adiv5_dp_write(ap->base.dp, ADIV5_DP_SELECT, ADIV5_DP_BANK5);
	adiv5_dp_write(ap->base.dp, ADIV6_DP_SELECT1, (uint32_t)(ap->ap_address >> 32U));
	/* Now set up SELECT in the DP */
	adiv5_dp_write(ap->base.dp, ADIV5_DP_SELECT, (uint32_t)ap->ap_address | (addr & 0x0ff0U));

	uint32_t result = 0;
	/* Loop through each CIDR register location and read it, pulling out only the relevant byte */
	for (size_t i = 0; i < 4U; ++i) {
		const uint32_t value = adiv5_dp_read(ap->base.dp, ADIV5_APnDP | (i << 2U));
		result |= (value & 0xffU) << (i * 8U);
	}
	return result;
}

static bool adiv6_component_probe(
	adiv5_debug_port_s *const dp, const target_addr64_t base_address, const uint32_t entry_number)
{
	/* Start out by making a fake AP to use for all the reads */
	adiv6_access_port_s base_ap = {
		.base.dp = dp,
		.ap_address = base_address,
	};

	const uint32_t cidr = adiv6_dp_read_id(&base_ap, CIDR0_OFFSET);
	/* CIDR preamble sanity check */
	if ((cidr & ~CID_CLASS_MASK) != CID_PREAMBLE) {
		DEBUG_WARN("%" PRIu32 " 0x%" PRIx32 "%08" PRIx32 ": 0x%08" PRIx32 " <- does not match preamble (0x%08" PRIx32
				   ")\n",
			entry_number, (uint32_t)(base_address >> 32U), (uint32_t)base_address, cidr, CID_PREAMBLE);
		return false;
	}
	/* Extract Component ID class nibble */
	const uint32_t cid_class = (cidr & CID_CLASS_MASK) >> CID_CLASS_SHIFT;

	return false;
}

static uint32_t adiv6_ap_reg_read(adiv5_access_port_s *const base_ap, const uint16_t addr)
{
	adiv6_access_port_s *const ap = (adiv6_access_port_s *)base_ap;
	/* Set SELECT1 in the DP up first */
	adiv5_dp_write(base_ap->dp, ADIV5_DP_SELECT, ADIV5_DP_BANK5);
	adiv5_dp_write(base_ap->dp, ADIV6_DP_SELECT1, (uint32_t)(ap->ap_address >> 32U));
	/* Now set up SELECT in the DP */
	const uint16_t bank = addr & ADIV6_AP_BANK_MASK;
	adiv5_dp_write(base_ap->dp, ADIV5_DP_SELECT, (uint32_t)ap->ap_address | ((bank & 0xf000U) >> 4U) | (bank & 0xf0U));
	return adiv5_dp_read(base_ap->dp, addr);
}

static void adiv6_ap_reg_write(adiv5_access_port_s *const base_ap, const uint16_t addr, const uint32_t value)
{
	adiv6_access_port_s *const ap = (adiv6_access_port_s *)base_ap;
	/* Set SELECT1 in the DP up first */
	adiv5_dp_write(base_ap->dp, ADIV5_DP_SELECT, ADIV5_DP_BANK5);
	adiv5_dp_write(base_ap->dp, ADIV6_DP_SELECT1, (uint32_t)(ap->ap_address >> 32U));
	/* Now set up SELECT in the DP */
	const uint16_t bank = addr & ADIV6_AP_BANK_MASK;
	adiv5_dp_write(base_ap->dp, ADIV5_DP_SELECT, (uint32_t)ap->ap_address | ((bank & 0xf000U) >> 4U) | (bank & 0xf0U));
	adiv5_dp_write(base_ap->dp, addr, value);
}