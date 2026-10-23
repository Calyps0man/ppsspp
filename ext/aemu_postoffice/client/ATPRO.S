	.set noreorder

#include "pspimport.s"

	IMPORT_START "ATPRO",0x40090000
	IMPORT_FUNC  "ATPRO",0x3DF7F7D8,printk
	IMPORT_FUNC  "ATPRO",0x2,has_high_mem
	IMPORT_FUNC  "ATPRO",0x5,partition_to_use
