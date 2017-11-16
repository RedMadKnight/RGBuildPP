#include "stdafx.h"
#include "util.h"
#include "sfcx.h"

struct sfc sfc = {0};
//static unsigned char sfcx_page[MAX_PAGE_SZ];   //Max known hardware physical page size
//static unsigned char sfcx_block[MAX_BLOCK_SZ]; //Max known hardware physical block size

__declspec( naked ) void sfcx_writereg(BYTE addr, DWORD data)
{
	(data);(addr);                 // suppress "unreferenced parameter" warnings
                                        //%r3=Register, %r4 = Value
#ifdef _XBOX
    __asm
    {
        li          r6, 0x00
        lis         r5, 0x7FEA          //%r5=0x0000_0000_7FEA_0000
        ori         r5, r5, 0xC000      //%r5=0x0000_0000_7FEA_C000
        or          r5, r5, r3          //%r5=0x0000_0000_7FEA_C00X Add the register offset
        stwbrx      r4, r6, r5          //store %r4 le to address pointed to by %r5
        //eieio                         //Enforce In-Order Execution of I/O
        blr
    }
	//*(volatile unsigned int*)(0xea00c000UL | addr) = bswap32X(data);
#endif
}
#ifdef _XBOX
__declspec( naked ) unsigned long sfcx_readreg(BYTE addr)
{
	(addr);                         // suppress "unreferenced parameter" warnings
                                        //%r3=Register, Return value stored in %r3
    __asm
    {
        li          r5, 0x00
        lis         r4, 0x7FEA          //%r4=0x0000_0000_7FEA_0000
        ori         r4, r4, 0xC000      //%r4=0x0000_0000_7FEA_C000
        or          r4, r4, r3          //%r4=0x0000_0000_7FEA_C00X Add the register offset
        lwbrx       r3, r5, r4          //load data from address pointed to by %r4 into %r3 le
        //eieio                         //Enforce In-Order Execution of I/O
        blr
    }
	//return bswap32X(*(volatile unsigned int*)(0xea00c000UL | addr));
}
#else
unsigned long sfcx_readreg(BYTE addr)
{
	return 0;
}
#endif

int sfcx_read_page(unsigned char *data, int address, int raw)
{
	int status;

	sfcx_writereg(SFCX_STATUS, sfcx_readreg(SFCX_STATUS));

	// Set flash address (logical)
	//address &= 0x3fffe00; // Align to page
	sfcx_writereg(SFCX_ADDRESS, address);

	// Command the read
	// Either a logical read (0x200 bytes, no meta data)
	// or a Physical read (0x210 bytes with meta data)
	sfcx_writereg(SFCX_COMMAND, raw ? PHY_PAGE_TO_BUF : LOG_PAGE_TO_BUF);

	// Wait Busy
	while ((status = sfcx_readreg(SFCX_STATUS)) & STATUS_BUSY);

	if (!SFCX_SUCCESS(status))
	{
		if (status & STATUS_BB_ER)
			printf(" ! SFCX: Bad block found at %08X\n", sfcx_address_to_block(address));
		else if (status & STATUS_ECC_ER)
		//	printf(" ! SFCX: (Corrected) ECC error at address %08X: %08X\n", address, status);
			status = status;
		else if (!raw && (status & STATUS_ILL_LOG))
			printf(" ! SFCX: Illegal logical block at %08X (status: %08X)\n", sfcx_address_to_block(address), status);
		else
			printf(" ! SFCX: Unknown error at address %08X: %08X. Please worry.\n", address, status);
	}

	// Set internal page buffer pointer to 0
	sfcx_writereg(SFCX_ADDRESS, 0);

	int i;
	int page_sz = raw ? sfc.page_sz_phys : sfc.page_sz;

	for (i = 0; i < page_sz ; i += 4)
	{
		// Transfer data from buffer to register
		sfcx_writereg(SFCX_COMMAND, PAGE_BUF_TO_REG);

		// Read out our data through the register
		*(int*)(data + i) = bswap32X(sfcx_readreg(SFCX_DATA));
	}

	return status;
}

int sfcx_write_page(unsigned char *data, int address)
{
	sfcx_writereg(SFCX_STATUS, 0xFF);

	// Enable Writes
	sfcx_writereg(SFCX_CONFIG, sfcx_readreg(SFCX_CONFIG) | CONFIG_WP_EN);

	// Set internal page buffer pointer to 0
	sfcx_writereg(SFCX_ADDRESS, 0);

	int i;
	for (i = 0; i < sfc.page_sz_phys; i+=4)
	{
		// Write out our data through the register
		sfcx_writereg(SFCX_DATA, bswap32X(*(int*)(data + i)));

		// Transfer data from register to buffer
		sfcx_writereg(SFCX_COMMAND, REG_TO_PAGE_BUF);
	}

	// Set flash address (logical)
	//address &= 0x3fffe00; // Align to page
	sfcx_writereg(SFCX_ADDRESS, address);

	// Unlock sequence (for write)
	sfcx_writereg(SFCX_COMMAND, UNLOCK_CMD_0);
	sfcx_writereg(SFCX_COMMAND, UNLOCK_CMD_1);

	// Wait Busy
	while (sfcx_readreg(SFCX_STATUS) & STATUS_BUSY);

	// Command the write
	sfcx_writereg(SFCX_COMMAND, WRITE_PAGE_TO_PHY);

	// Wait Busy
	while (sfcx_readreg(SFCX_STATUS) & STATUS_BUSY);

	int status = sfcx_readreg(SFCX_STATUS);
	if (!SFCX_SUCCESS(status))
		printf(" ! SFCX: Unexpected sfcx_write_page status %08X\n", status);

	// Disable Writes
	sfcx_writereg(SFCX_CONFIG, sfcx_readreg(SFCX_CONFIG) & ~CONFIG_WP_EN);

	return status;
}

int sfcx_read_block(unsigned char *data, int address, int raw)
{
	int p;
	int status = 0;
	int page_sz = raw ? sfc.page_sz_phys : sfc.page_sz;

	for (p = 0; p < sfc.pages_in_block; p++)
	{
		status |= sfcx_read_page(&data[p * page_sz], address + (p * sfc.page_sz), raw);
		//if (!SFCX_SUCCESS(status))
		//	break;
	}
	return status;
}

int sfcx_write_block(unsigned char *data, int address)
{
	int p;
	int status = 0;

	for (p = 0; p < sfc.pages_in_block; p++)
	{
		status |= sfcx_write_page(&data[p * sfc.page_sz_phys], address + (p * sfc.page_sz));
		//if (!SFCX_SUCCESS(status))
		//	break;
	}
	return status;
}

int sfcx_erase_block(int address)
{
	// Enable Writes
	sfcx_writereg(SFCX_CONFIG, sfcx_readreg(SFCX_CONFIG) | CONFIG_WP_EN);
	sfcx_writereg(SFCX_STATUS, 0xFF);

	// Set flash address (logical)
	//address &= 0x3fffe00; // Align to page
	sfcx_writereg(SFCX_ADDRESS, address);

	// Wait Busy
	while (sfcx_readreg(SFCX_STATUS) & STATUS_BUSY);

	// Unlock sequence (for erase)
	sfcx_writereg(SFCX_COMMAND, UNLOCK_CMD_1);
	sfcx_writereg(SFCX_COMMAND, UNLOCK_CMD_0);

	// Wait Busy
	while (sfcx_readreg(SFCX_STATUS) & STATUS_BUSY);

	// Command the block erase
	sfcx_writereg(SFCX_COMMAND, BLOCK_ERASE);

	// Wait Busy
	while (sfcx_readreg(SFCX_STATUS) & STATUS_BUSY);

	int status = sfcx_readreg(SFCX_STATUS);
	//if (!SFCX_SUCCESS(status))
	//	printf(" ! SFCX: Unexpected sfcx_erase_block status %08X\n", status);
	sfcx_writereg(SFCX_STATUS, 0xFF);

	// Disable Writes
	sfcx_writereg(SFCX_CONFIG, sfcx_readreg(SFCX_CONFIG) & ~CONFIG_WP_EN);

	return status;
}

void sfcx_calcecc(unsigned int *data)
{
	unsigned int i=0, val=0;
	unsigned char *edc = ((unsigned char*)data) + sfc.page_sz;

	unsigned int v=0;

	for (i = 0; i < 0x1066; i++)
	{
		if (!(i & 31))
			v = ~bswap32Xe(*data++);
		val ^= v & 1;
		v>>=1;
		if (val & 1)
			val ^= 0x6954559;
		val >>= 1;
	}

	val = ~val;

	// 26 bit ecc data
	edc[0xC] = ((val << 6) | (edc[0xC] & 0x3F)) & 0xFF;
	edc[0xD] = (val >> 2) & 0xFF;
	edc[0xE] = (val >> 10) & 0xFF;
	edc[0xF] = (val >> 18) & 0xFF;
}

int sfcx_get_blocknumber(unsigned char *data)
{
	int num = 0;
	switch (sfc.meta_type)
	{
	case META_TYPE_0:
		num = (data[sfc.page_sz + 0x1] << 8) | (data[sfc.page_sz + 0x0]);
		break;
	case META_TYPE_1:
		num = (data[sfc.page_sz + 0x2] << 8) | (data[sfc.page_sz + 0x1]);
		break;
	case META_TYPE_2:
		num = (data[sfc.page_sz + 0x2] << 8) | (data[sfc.page_sz + 0x1]);
		break;
	}
	return num;
}

void sfcx_set_blocknumber(unsigned char *data, int num)
{
	switch (sfc.meta_type)
	{
	case META_TYPE_0:
		data[sfc.page_sz + 0x1] = (num >> 8) & 0xFF;
		data[sfc.page_sz + 0x0] = (num >> 0) & 0xFF;
		break;
	case META_TYPE_1:
		data[sfc.page_sz + 0x2] = (num >> 8) & 0xFF;
		data[sfc.page_sz + 0x1] = (num >> 0) & 0xFF;
		break;
	case META_TYPE_2:
		data[sfc.page_sz + 0x2] = (num >> 8) & 0xFF;
		data[sfc.page_sz + 0x1] = (num >> 0) & 0xFF;
		break;
	}
}

int sfcx_get_blockversion(unsigned char *data)
{
	int ver = 0;
	switch (sfc.meta_type)
	{
	case META_TYPE_0:
		ver = (data[sfc.page_sz + 0x6] << 24) | (data[sfc.page_sz + 0x4] << 16) |
		      (data[sfc.page_sz + 0x3] << 8)  | (data[sfc.page_sz + 0x2]);
		break;
	case META_TYPE_1:
		ver = (data[sfc.page_sz + 0x6] << 24) | (data[sfc.page_sz + 0x4] << 16) |
		      (data[sfc.page_sz + 0x3] << 8)  | (data[sfc.page_sz + 0x0]);
		break;
	case META_TYPE_2:
		ver = (data[sfc.page_sz + 0x6] << 24) | (data[sfc.page_sz + 0x4] << 16) |
		      (data[sfc.page_sz + 0x3] << 8)  | (data[sfc.page_sz + 0x5]);
		break;
	}
	return ver;
}

void sfcx_set_blockversion(unsigned char *data, int ver)
{
	switch (sfc.meta_type)
	{
	case META_TYPE_0:
		data[sfc.page_sz + 0x2] = (ver >> 0)  & 0xFF;
		data[sfc.page_sz + 0x3] = (ver >> 8)  & 0xFF;
		data[sfc.page_sz + 0x4] = (ver >> 16) & 0xFF;
		data[sfc.page_sz + 0x6] = (ver >> 24) & 0xFF;
		break;
	case META_TYPE_1:
		data[sfc.page_sz + 0x0] = (ver >> 0)  & 0xFF;
		data[sfc.page_sz + 0x3] = (ver >> 8)  & 0xFF;
		data[sfc.page_sz + 0x4] = (ver >> 16) & 0xFF;
		data[sfc.page_sz + 0x6] = (ver >> 24) & 0xFF;
		break;
	case META_TYPE_2:
		data[sfc.page_sz + 0x5] = (ver >> 0)  & 0xFF;
		data[sfc.page_sz + 0x3] = (ver >> 8)  & 0xFF;
		data[sfc.page_sz + 0x4] = (ver >> 16) & 0xFF;
		data[sfc.page_sz + 0x6] = (ver >> 24) & 0xFF;
		break;
	}
}

void sfcx_set_pagevalid(unsigned char *data)
{
	switch (sfc.meta_type)
	{
	case META_TYPE_0:
		data[sfc.page_sz + 0x5] = 0xFF;
		break;
	case META_TYPE_1:
		data[sfc.page_sz + 0x5] = 0xFF;
		break;
	case META_TYPE_2:
		data[sfc.page_sz + 0x0] = 0xFF;
		break;
	}
}

void sfcx_set_pageinvalid(unsigned char *data)
{
	switch (sfc.meta_type)
	{
	case META_TYPE_0:
		data[sfc.page_sz + 0x5] = 0x00;
		break;
	case META_TYPE_1:
		data[sfc.page_sz + 0x5] = 0x00;
		break;
	case META_TYPE_2:
		data[sfc.page_sz + 0x0] = 0x00;
		break;
	}
}

int sfcx_is_pagevalid(unsigned char *data)
{
	int valid = 0;
	switch (sfc.meta_type)
	{
	case META_TYPE_0:
		valid = data[sfc.page_sz + 0x5] == 0xFF;
		break;
	case META_TYPE_1:
		valid = data[sfc.page_sz + 0x5] == 0xFF;
		break;
	case META_TYPE_2:
		valid = data[sfc.page_sz + 0x0] == 0xFF;
		break;
	}
	return valid;
}

int sfcx_is_pagezeroed(unsigned char *data)
{
	int i;
	for(i = 0; i < sfc.page_sz; i++)
	{
	  if (data[i]!=0x00)
	    return 0;
	}
	return 1;
}

int sfcx_is_pageerased(unsigned char *data)
{
	int i;
	for(i = 0; i < sfc.page_sz_phys; i++)
	{
	  if (data[i]!=0xFF)
	    return 0;
	}
	return 1;
}

int sfcx_block_to_address(int block)
{
	return block * sfc.block_sz;
}

int sfcx_address_to_block(int address)
{
	return address / sfc.block_sz;
}

int sfcx_block_to_rawaddress(int block)
{
        return block * sfc.block_sz_phys;
}

int sfcx_rawaddress_to_block(int address)
{
        return address / sfc.block_sz_phys;
}

/*
int sfcx_read_metadata_type(void)
{
	int bid = 0; //Block ID

	//Lets read what is actually on the nand
	sfcx_read_page(sfcx_page, 0x8000, 1);

	// Normal Xenon Type
	bid = (sfcx_page[sfc.page_sz + 0x1] << 8) | (sfcx_page[sfc.page_sz + 0x0]);
	if (bid == 2 && sfcx_page[sfc.page_sz + 0x5] == 0xFF){
		return META_TYPE_0;
	}

	// Jasper 16MB Small Block
	bid = (sfcx_page[sfc.page_sz + 0x2] << 8) | (sfcx_page[sfc.page_sz + 0x1]);
	if (bid == 2 && sfcx_page[sfc.page_sz + 0x5] == 0xFF){
		return META_TYPE_1;
	}

	// Jasper 256/512 Large Block
	bid = (sfcx_page[sfc.page_sz + 0x2] << 8) | (sfcx_page[sfc.page_sz + 0x1]);
	if (bid == 0 && sfcx_page[sfc.page_sz + 0x0] == 0xFF){
		return META_TYPE_2;
	}

	return -1;

}
*/
unsigned int sfcx_reset(void)
{
	unsigned int config = sfcx_readreg(SFCX_CONFIG);
	if(!sfc.initialized) return config;
	sfcx_writereg(SFCX_CONFIG, config | 0xC);
	sfc.initialized = false;
	return config;
}
unsigned int sfcx_init(void)
{
	unsigned int config = sfcx_readreg(SFCX_CONFIG);

	if (sfc.initialized) return config;

	sfc.initialized = 0;
	sfc.meta_type = 0;
	sfc.page_sz = 0x200;
	sfc.meta_sz = 0x10;
	sfc.page_sz_phys = sfc.page_sz + sfc.meta_sz;

	//Turn off interrupts, turn off WP_EN, and set DMA pages to 0
	sfcx_writereg(SFCX_CONFIG, config &~ (CONFIG_INT_EN|CONFIG_WP_EN|CONFIG_DMA_LEN));

	switch ((config >> 17) & 0x03)
	{
	case 0: // Small block original SFC (pre jasper)
		sfc.meta_type = META_TYPE_0;
		sfc.blocks_per_lg_block = 8;

		switch ((config >> 4) & 0x3)
		{
		case 0: // Unsupported 8MB?
			printf(" ! SFCX: Unsupported Type A-0\n");
			return 1;

			//sfc.block_sz = 0x4000; // 16 KB
			//sfc.size_blocks = 0x200;
			//sfc.size_bytes = sfc.size_blocks << 0xE;
			//sfc.size_usable_fs = 0xXXX;
			//sfc.addr_config = 0x07BE000 - 0x4000;

		case 1: // 16MB
			sfc.block_sz = 0x4000; // 16 KB
			sfc.size_blocks = 0x400;
			sfc.size_bytes = sfc.size_blocks << 0xE;
			sfc.size_usable_fs = 0x3E0;
			sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
			break;

		case 2: // 32MB
			sfc.block_sz = 0x4000; // 16 KB
			sfc.size_blocks = 0x800;
			sfc.size_bytes = sfc.size_blocks << 0xE;
			sfc.size_usable_fs = 0x7C0;
			sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
			break;

		case 3: // 64MB
			sfc.block_sz = 0x4000; // 16 KB
			sfc.size_blocks = 0x1000;
			sfc.size_bytes = sfc.size_blocks << 0xE;
			sfc.size_usable_fs = 0xF80;
			sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
			break;
		}
		break;

	case 1: // New SFC/Southbridge: Codename "Panda"?
	case 2: // New SFC/Southbridge: Codename "Panda" v2?
		switch ((config >> 4) & 0x3)
		{
		case 0: 
			
			if(((config >> 17) & 0x03) == 0x01)
			{
				// Unsupported
				sfc.meta_type = META_TYPE_0;
				printf(" ! SFCX: Unsupported Type B-0\n");
				return 2;
			}
			else
			{
				sfc.meta_type = META_TYPE_1;
				sfc.block_sz = 0x4000; // 16 KB
				sfc.size_blocks = 0x400;
				sfc.size_bytes = sfc.size_blocks << 0xE;
				sfc.blocks_per_lg_block = 8;
				sfc.size_usable_fs = 0x3E0;
				sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
				break;
			}

		case 1: 

			if(((config >> 17) & 0x03) == 0x01)
			{
				// Small block 16MB setup
				sfc.meta_type = META_TYPE_1;
				sfc.block_sz = 0x4000; // 16 KB
				sfc.size_blocks = 0x400;
				sfc.size_bytes = sfc.size_blocks << 0xE;
				sfc.blocks_per_lg_block = 8;
				sfc.size_usable_fs = 0x3E0;
				sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
				break;
			}
			else
			{
				// Small block 64MB setup
				sfc.meta_type = META_TYPE_1;
				sfc.block_sz = 0x4000; // 16 KB
				sfc.size_blocks = 0x1000;
				sfc.size_bytes = sfc.size_blocks << 0xE;
				sfc.blocks_per_lg_block = 8;
				sfc.size_usable_fs = 0xF80;
				sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
				break;
			}

		case 2: // Large Block: Current Jasper 256MB and 512MB
			sfc.meta_type = META_TYPE_2;
			sfc.block_sz = 0x20000; // 128KB
			sfc.size_bytes = 0x1 << (((config >> 19) & 0x3) + ((config >> 21) & 0xF) + 0x17);
			sfc.size_blocks = sfc.size_bytes >> 0x11;
			sfc.blocks_per_lg_block = 1;
			sfc.size_usable_fs = 0x1E0;
			sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
			break;

		case 3: // Large Block: Future or unknown hardware
			sfc.meta_type = META_TYPE_2;
			sfc.block_sz = 0x40000; // 256KB
			sfc.size_bytes = 0x1 << (((config >> 19) & 0x3) + ((config >> 21) & 0xF) + 0x17);
			sfc.size_blocks = sfc.size_bytes >> 0x12;
			sfc.blocks_per_lg_block = 1;
			sfc.size_usable_fs = 0xF0;
			sfc.addr_config = (sfc.size_usable_fs - CONFIG_BLOCKS) * sfc.block_sz;
			break;
		}
		break;

	default:
		printf(" ! SFCX: Unsupported Type\n");
		return 3;
	}

	sfc.len_config = sfc.block_sz * 0x04; //4 physical blocks

	sfc.pages_in_block = sfc.block_sz / sfc.page_sz;
	sfc.block_sz_phys = sfc.pages_in_block * sfc.page_sz_phys;

	sfc.size_pages = sfc.size_bytes / sfc.page_sz;
	sfc.size_blocks = sfc.size_bytes / sfc.block_sz;

	sfc.size_bytes_phys = sfc.block_sz_phys * sfc.size_blocks;
	sfc.size_mb = sfc.size_bytes >> 20;

/*
	int meta_type;

	meta_type = sfcx_read_metadata_type();
	if (meta_type == -1){
		printf(" ! SFCX: Meta Type detection error\n");
		delay(5);
		return 4;
	}

	if (meta_type != sfc.meta_type){
		printf(" ! SFCX: Meta Type detection difference\n");
		printf(" ! SFCX: expecting type: '%d' detected: '%d'\n", sfc.meta_type, meta_type);
		sfc.meta_type = meta_type;
	}

*/


#if 0
	printf("   config register     = %08X\n", config);

	printf("   sfc:page_sz         = %08X\n", sfc.page_sz);
	printf("   sfc:meta_sz         = %08X\n", sfc.meta_sz);
	printf("   sfc:page_sz_phys    = %08X\n", sfc.page_sz_phys);

	printf("   sfc:pages_in_block  = %08X\n", sfc.pages_in_block);
	printf("   sfc:block_sz        = %08X\n", sfc.block_sz);
	printf("   sfc:block_sz_phys   = %08X\n", sfc.block_sz_phys);

	printf("   sfc:size_mb         = %dMB\n", sfc.size_mb);
	printf("   sfc:size_bytes      = %08X\n", sfc.size_bytes);
	printf("   sfc:size_bytes_phys = %08X\n", sfc.size_bytes_phys);

	printf("   sfc:size_pages      = %08X\n", sfc.size_pages);
	printf("   sfc:size_blocks     = %08X\n", sfc.size_blocks);
	printf("\n");
#endif

	sfc.initialized = SFCX_INITIALIZED;
	return config;
}