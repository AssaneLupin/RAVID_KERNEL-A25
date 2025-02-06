SECTIONS {
	__patchable_function_entries : { *(__patchable_function_entries) }
	.text : {
		__generate_irc_v1.cfi_jt = . + 0x4e6d0;
	}
}
